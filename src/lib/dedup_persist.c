// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_persist.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Persisted dedup key set and pending buffer save/load.
//
// --[ License ] --------------------------------------------------------------
//
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as
//     published by the Free Software Foundation, either version 3 of the
//     License, or (at your option) any later version.
//
//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.
//
//     You should have received a copy of the GNU General Public License
//     along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------
// Copyright © 2015-2026 Natalia Portillo
// ****************************************************************************/

#include "dedup_internal.h"

/* ------------------------------------------------------------------ */
/*  Persisted dedup key set (save / load)                              */
/* ------------------------------------------------------------------ */

/** On-disk header for the persisted key set extent. */
#define KEYSET_PERSIST_MAGIC 0x53594B44444E4F4DULL /* "MONDKEYS" LE */

struct __attribute__((packed)) keyset_persist_header
{
    uint64_t magic;    /**< KEYSET_PERSIST_MAGIC */
    uint64_t count;    /**< Number of uint64_t keys following this header */
    uint64_t checksum; /**< XXH64 of the packed key array (count * 8 bytes) */
};

/**
 * Persist the in-memory dedup key set to a contiguous extent on disk.
 *
 * Packs all non-empty keys into a flat uint64_t array, prepends a
 * small header with magic + count + XXH64 checksum, and writes the
 * result to contiguously allocated blocks.  Updates
 * ctx->sb.keyset_lba / keyset_blocks so the next superblock write
 * records the location.
 *
 * If a previous keyset extent exists its blocks are freed first.
 *
 * @param ctx  Filesystem context (must have bitmap + fd).
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_dedup_keyset_save(struct obmafs3_ctx *ctx)
{
    if(!ctx || !ctx->dedup_key_set || !ctx->bitmap || ctx->fd < 0) return OBMAFS3_ERR_INVAL;

    struct dedup_key_set *ks = (struct dedup_key_set *)ctx->dedup_key_set;
    if(ks->count == 0)
    {
        /* Nothing to persist — free old extent if any. */
        if(ctx->sb.keyset_lba != 0 && ctx->sb.keyset_blocks != 0)
            obmafs3_free_blocks(ctx, ctx->sb.keyset_lba, ctx->sb.keyset_blocks);
        ctx->sb.keyset_lba    = 0;
        ctx->sb.keyset_blocks = 0;
        return OBMAFS3_OK;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Compute sizes up-front so we only allocate ONE buffer. */
    uint32_t count         = ks->count;
    size_t   payload_bytes = sizeof(struct keyset_persist_header) + (size_t)count * sizeof(uint64_t);
    uint64_t block_size    = ctx->sb.block_size;
    uint64_t needed_blocks = (payload_bytes + block_size - 1) / block_size;
    size_t   buf_size      = (size_t)(needed_blocks * block_size);

    /* Single buffer: [header][packed keys][zero-padded tail].
     * Only zero the tail padding, not the entire buffer. */
    uint8_t *buf = malloc(buf_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Pack non-empty keys directly after the header space. */
    uint64_t *key_dst = (uint64_t *)(buf + sizeof(struct keyset_persist_header));
    uint32_t  n       = 0;
    for(uint32_t i = 0; i < ks->capacity; i++)
    {
        if(ks->keys[i] != KEYSET_EMPTY) key_dst[n++] = ks->keys[i];
    }

    /* Zero-pad tail to block boundary. */
    size_t used = sizeof(struct keyset_persist_header) + (size_t)n * sizeof(uint64_t);
    if(used < buf_size) memset(buf + used, 0, buf_size - used);

    /* Build header (checksum computed over the packed keys in-place). */
    struct keyset_persist_header hdr;
    hdr.magic    = KEYSET_PERSIST_MAGIC;
    hdr.count    = n;
    hdr.checksum = obmafs3_checksum_xxh64(key_dst, (size_t)n * sizeof(uint64_t));
    memcpy(buf, &hdr, sizeof(hdr));

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double pack_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fprintf(stderr, "[dedup-keyset] packed %u keys (%.1f MiB) in %.1f ms\n", n,
            (double)(n * sizeof(uint64_t)) / (1024.0 * 1024.0), pack_ms);

    /* Free old extent if present. */
    if(ctx->sb.keyset_lba != 0 && ctx->sb.keyset_blocks != 0)
        obmafs3_free_blocks(ctx, ctx->sb.keyset_lba, ctx->sb.keyset_blocks);

    /* Allocate contiguous blocks. */
    uint64_t start_lba = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = obmafs3_alloc_blocks(ctx, needed_blocks, &start_lba);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double alloc_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fprintf(stderr, "[dedup-keyset] alloc %" PRIu64 " blocks: %s in %.1f ms\n", needed_blocks,
            rc == OBMAFS3_OK ? "ok" : "FAILED", alloc_ms);

    if(rc != OBMAFS3_OK)
    {
        free(buf);
        ctx->sb.keyset_lba    = 0;
        ctx->sb.keyset_blocks = 0;
        return rc;
    }

    /* Write the entire extent in a single pwrite. */
    fprintf(stderr, "[dedup-keyset] writing %" PRIu64 " blocks (%.1f MiB) to LBA %" PRIu64 "...\n", needed_blocks,
            (double)buf_size / (1024.0 * 1024.0), start_lba);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    {
        off_t  offset    = (off_t)(start_lba * block_size);
        size_t remaining = buf_size;
        size_t written   = 0;

        while(remaining > 0)
        {
            ssize_t w = pwrite(ctx->fd, buf + written, remaining, offset + (off_t)written);
            if(w <= 0)
            {
                rc = OBMAFS3_ERR_IO;
                free(buf);
                obmafs3_free_blocks(ctx, start_lba, needed_blocks);
                ctx->sb.keyset_lba    = 0;
                ctx->sb.keyset_blocks = 0;
                return rc;
            }
            written += (size_t)w;
            remaining -= (size_t)w;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double write_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    free(buf);

    ctx->sb.keyset_lba    = start_lba;
    ctx->sb.keyset_blocks = needed_blocks;

    fprintf(stderr,
            "[dedup-keyset] persisted %u keys (%" PRIu64 " blocks at LBA %" PRIu64 ") — "
            "write %.1f ms\n",
            n, needed_blocks, start_lba, write_ms);

    return OBMAFS3_OK;
}

/**
 * Load a persisted dedup key set from disk.
 *
 * Reads the contiguous extent at ctx->sb.keyset_lba, validates the
 * header (magic + XXH64 checksum), and bulk-inserts all keys into a
 * freshly created key set.
 *
 * @param ctx  Filesystem context.
 * @return @c OBMAFS3_OK on success (key set stored in ctx->dedup_key_set),
 *         or an error code on failure (caller should fall back to tree scan).
 */
int obmafs3_dedup_keyset_load(struct obmafs3_ctx *ctx)
{
    if(!ctx || ctx->sb.keyset_lba == 0 || ctx->sb.keyset_blocks == 0) return OBMAFS3_ERR_NOTFOUND;

    uint64_t block_size    = ctx->sb.block_size;
    uint64_t needed_blocks = ctx->sb.keyset_blocks;
    size_t   buf_size      = (size_t)(needed_blocks * block_size);

    uint8_t *buf = malloc(buf_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Read the entire extent in a single pread. */
    {
        off_t  offset    = (off_t)(ctx->sb.keyset_lba * block_size);
        size_t remaining = buf_size;
        size_t rd        = 0;

        while(remaining > 0)
        {
            ssize_t n = pread(ctx->fd, buf + rd, remaining, offset + (off_t)rd);
            if(n <= 0)
            {
                free(buf);
                return OBMAFS3_ERR_IO;
            }
            rd += (size_t)n;
            remaining -= (size_t)n;
        }
    }

    /* Validate header. */
    if(buf_size < sizeof(struct keyset_persist_header))
    {
        free(buf);
        return OBMAFS3_ERR_INVAL;
    }

    struct keyset_persist_header hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if(hdr.magic != KEYSET_PERSIST_MAGIC)
    {
        fprintf(stderr, "[dedup-keyset] bad magic in persisted keyset — falling back to tree scan\n");
        free(buf);
        return OBMAFS3_ERR_BADMAGIC;
    }

    /* Sanity-check count fits in the extent. */
    size_t payload_size = (size_t)hdr.count * sizeof(uint64_t);
    if(sizeof(hdr) + payload_size > buf_size)
    {
        fprintf(stderr, "[dedup-keyset] persisted keyset count %" PRIu64 " overflows extent\n", hdr.count);
        free(buf);
        return OBMAFS3_ERR_INVAL;
    }

    /* Verify checksum. */
    const uint8_t *key_data = buf + sizeof(hdr);
    uint64_t       computed = obmafs3_checksum_xxh64(key_data, payload_size);
    if(computed != hdr.checksum)
    {
        fprintf(stderr, "[dedup-keyset] checksum mismatch in persisted keyset — falling back to tree scan\n");
        free(buf);
        return OBMAFS3_ERR_CHECKSUM;
    }

    /* Create key set and bulk-insert. */
    /* Choose initial capacity: next power-of-2 >= count / 0.75 */
    uint32_t min_cap = (uint32_t)((hdr.count * 4 + 2) / 3); /* ceil(count / 0.75) */
    uint32_t cap     = KEYSET_INIT_CAP;
    while(cap < min_cap) cap *= 2;

    struct dedup_key_set *ks = calloc(1, sizeof(*ks));
    if(!ks)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }
    ks->capacity = cap;
    ks->keys     = calloc(cap, sizeof(uint64_t));
    if(!ks->keys)
    {
        free(ks);
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    const uint64_t *keys = (const uint64_t *)key_data;
    uint32_t        mask = cap - 1;
    for(uint64_t i = 0; i < hdr.count; i++)
    {
        uint64_t key = keys[i];
        if(key == KEYSET_EMPTY) continue;
        uint32_t idx = keyset_hash(key, mask);
        for(uint32_t j = 0; j < cap; j++)
        {
            uint32_t s = (idx + j) & mask;
            if(ks->keys[s] == KEYSET_EMPTY)
            {
                ks->keys[s] = key;
                ks->count++;
                break;
            }
            if(ks->keys[s] == key) break; /* duplicate */
        }
    }
    free(buf);

    ctx->dedup_key_set = ks;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Persisted pending insert buffer (save / load)                      */
/* ------------------------------------------------------------------ */

/** On-disk header for the persisted pending buffer extent. */
#define PENDING_PERSIST_MAGIC 0x474E49444E455055ULL /* "UPENDING" LE */

struct __attribute__((packed)) pending_persist_header
{
    uint64_t magic;       /**< PENDING_PERSIST_MAGIC */
    uint64_t count;       /**< Number of dedup_entry records */
    uint16_t sector_size; /**< Sector size of the pending buffer */
    uint8_t  _pad[6];     /**< Alignment padding */
    uint64_t checksum;    /**< XXH64 of the packed entry array */
};

/**
 * Persist the in-memory pending insert buffer(s) to disk.
 *
 * Packs all non-empty entries from both the active pending buffer
 * (ctx->dedup_pending) and the draining buffer (ctx->dedup_pending_draining)
 * into a flat dedup_entry array, prepends a header with magic + count +
 * sector_size + XXH64 checksum, and writes to contiguously allocated blocks.
 *
 * @param ctx  Filesystem context.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_dedup_pending_save(struct obmafs3_ctx *ctx)
{
    if(!ctx || !ctx->bitmap || ctx->fd < 0) return OBMAFS3_ERR_INVAL;

    const struct dedup_pending_buf *pb1 = (const struct dedup_pending_buf *)ctx->dedup_pending;
    const struct dedup_pending_buf *pb2 = (const struct dedup_pending_buf *)ctx->dedup_pending_draining;

    uint32_t total_count = 0;
    uint16_t sector_size = 0;
    if(pb1)
    {
        total_count += pb1->count;
        if(pb1->sector_size) sector_size = pb1->sector_size;
    }
    if(pb2)
    {
        total_count += pb2->count;
        if(pb2->sector_size) sector_size = pb2->sector_size;
    }

    if(total_count == 0)
    {
        /* Nothing to persist — free old extent if any. */
        if(ctx->sb.pending_lba != 0 && ctx->sb.pending_blocks != 0)
            obmafs3_free_blocks(ctx, ctx->sb.pending_lba, ctx->sb.pending_blocks);
        ctx->sb.pending_lba    = 0;
        ctx->sb.pending_blocks = 0;
        return OBMAFS3_OK;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t   payload_bytes = sizeof(struct pending_persist_header) + (size_t)total_count * sizeof(struct dedup_entry);
    uint64_t block_size    = ctx->sb.block_size;
    uint64_t needed_blocks = (payload_bytes + block_size - 1) / block_size;
    size_t   buf_size      = (size_t)(needed_blocks * block_size);

    uint8_t *buf = malloc(buf_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Pack entries after header space. */
    struct dedup_entry *dst = (struct dedup_entry *)(buf + sizeof(struct pending_persist_header));
    uint32_t            n   = 0;

    if(pb1)
    {
        for(uint32_t i = 0; i < pb1->capacity; i++)
            if(pb1->slots[i].hash != KEYSET_EMPTY) dst[n++] = pb1->slots[i];
    }
    if(pb2)
    {
        for(uint32_t i = 0; i < pb2->capacity; i++)
            if(pb2->slots[i].hash != KEYSET_EMPTY) dst[n++] = pb2->slots[i];
    }

    /* Zero-pad tail. */
    size_t used = sizeof(struct pending_persist_header) + (size_t)n * sizeof(struct dedup_entry);
    if(used < buf_size) memset(buf + used, 0, buf_size - used);

    /* Build header. */
    struct pending_persist_header hdr;
    hdr.magic       = PENDING_PERSIST_MAGIC;
    hdr.count       = n;
    hdr.sector_size = sector_size;
    memset(hdr._pad, 0, sizeof(hdr._pad));
    hdr.checksum = obmafs3_checksum_xxh64(dst, (size_t)n * sizeof(struct dedup_entry));
    memcpy(buf, &hdr, sizeof(hdr));

    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "[dedup-pending] packed %u entries (%.1f KiB) in %.1f ms\n", n,
            (double)(n * sizeof(struct dedup_entry)) / 1024.0,
            (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

    /* Free old extent. */
    if(ctx->sb.pending_lba != 0 && ctx->sb.pending_blocks != 0)
        obmafs3_free_blocks(ctx, ctx->sb.pending_lba, ctx->sb.pending_blocks);

    /* Allocate contiguous blocks. */
    uint64_t start_lba = 0;
    int      rc        = obmafs3_alloc_blocks(ctx, needed_blocks, &start_lba);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        ctx->sb.pending_lba    = 0;
        ctx->sb.pending_blocks = 0;
        return rc;
    }

    /* Single pwrite. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    {
        off_t  offset    = (off_t)(start_lba * block_size);
        size_t remaining = buf_size;
        size_t written   = 0;
        while(remaining > 0)
        {
            ssize_t w = pwrite(ctx->fd, buf + written, remaining, offset + (off_t)written);
            if(w <= 0)
            {
                free(buf);
                obmafs3_free_blocks(ctx, start_lba, needed_blocks);
                ctx->sb.pending_lba    = 0;
                ctx->sb.pending_blocks = 0;
                return OBMAFS3_ERR_IO;
            }
            written += (size_t)w;
            remaining -= (size_t)w;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(buf);

    ctx->sb.pending_lba    = start_lba;
    ctx->sb.pending_blocks = needed_blocks;

    fprintf(stderr,
            "[dedup-pending] persisted %u entries (%" PRIu64 " blocks at LBA %" PRIu64 ") — "
            "write %.1f ms\n",
            n, needed_blocks, start_lba, (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

    return OBMAFS3_OK;
}

/**
 * Load a persisted pending insert buffer from disk.
 *
 * Reads the extent at ctx->sb.pending_lba, validates the header,
 * and creates a pending buffer with all entries.  When
 * @p keyset_from_disk is false, also inserts loaded hashes into the
 * keyset so the write path's fast existence check sees them.
 * When the keyset was loaded from its persisted file, it already
 * contains the pending hashes (they were present at save time),
 * so the insertion loop is skipped.
 *
 * @param ctx              Filesystem context.
 * @param keyset_from_disk Non-zero when the keyset was loaded from
 *                         disk (skip redundant keyset inserts).
 * @return @c OBMAFS3_OK on success, or an error code.
 */
int obmafs3_dedup_pending_load(struct obmafs3_ctx *ctx, int keyset_from_disk)
{
    if(!ctx || ctx->sb.pending_lba == 0 || ctx->sb.pending_blocks == 0) return OBMAFS3_ERR_NOTFOUND;

    uint64_t block_size    = ctx->sb.block_size;
    uint64_t needed_blocks = ctx->sb.pending_blocks;
    size_t   buf_size      = (size_t)(needed_blocks * block_size);

    uint8_t *buf = malloc(buf_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Read extent. */
    {
        off_t  offset    = (off_t)(ctx->sb.pending_lba * block_size);
        size_t remaining = buf_size;
        size_t rd        = 0;
        while(remaining > 0)
        {
            ssize_t n = pread(ctx->fd, buf + rd, remaining, offset + (off_t)rd);
            if(n <= 0)
            {
                free(buf);
                return OBMAFS3_ERR_IO;
            }
            rd += (size_t)n;
            remaining -= (size_t)n;
        }
    }

    /* Validate header. */
    if(buf_size < sizeof(struct pending_persist_header))
    {
        free(buf);
        return OBMAFS3_ERR_INVAL;
    }

    struct pending_persist_header hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if(hdr.magic != PENDING_PERSIST_MAGIC)
    {
        fprintf(stderr, "[dedup-pending] bad magic in persisted pending buffer\n");
        free(buf);
        return OBMAFS3_ERR_BADMAGIC;
    }

    size_t payload_size = (size_t)hdr.count * sizeof(struct dedup_entry);
    if(sizeof(hdr) + payload_size > buf_size)
    {
        fprintf(stderr, "[dedup-pending] persisted pending count %" PRIu64 " overflows extent\n", hdr.count);
        free(buf);
        return OBMAFS3_ERR_INVAL;
    }

    const uint8_t *entry_data = buf + sizeof(hdr);
    uint64_t       computed   = obmafs3_checksum_xxh64(entry_data, payload_size);
    if(computed != hdr.checksum)
    {
        fprintf(stderr, "[dedup-pending] checksum mismatch in persisted pending buffer\n");
        free(buf);
        return OBMAFS3_ERR_CHECKSUM;
    }

    /* Create pending buffer pre-sized for the persisted entry count
     * so that pending_insert() never triggers pending_grow() — avoids
     * O(N log N) rehash churn when restoring large pending buffers. */
    struct dedup_pending_buf *pb = pending_create_presized(hdr.count);
    if(!pb)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }
    pb->sector_size = hdr.sector_size;

    const struct dedup_entry *entries = (const struct dedup_entry *)entry_data;
    for(uint64_t i = 0; i < hdr.count; i++)
    {
        if(entries[i].hash == KEYSET_EMPTY) continue;
        pending_insert(pb, &entries[i]);
    }

    /* Insert loaded hashes into the keyset for fast lookups — but
     * only when the keyset was rebuilt via tree scan.  When the
     * keyset was loaded from its persisted file it already contains
     * all pending hashes (they were present at save time), so
     * re-inserting them would just probe a dense 243 M-entry table
     * for every entry with no benefit. */
    if(!keyset_from_disk)
    {
        struct dedup_key_set *ks = (struct dedup_key_set *)ctx->dedup_key_set;
        if(ks)
        {
            for(uint64_t i = 0; i < hdr.count; i++)
            {
                if(entries[i].hash == KEYSET_EMPTY) continue;
                keyset_insert(ks, entries[i].hash);
            }
        }
    }

    free(buf);
    ctx->dedup_pending = pb;

    fprintf(stderr, "[dedup-pending] loaded %u persisted entries (sector_size=%u)\n", pb->count, pb->sector_size);

    return OBMAFS3_OK;
}
