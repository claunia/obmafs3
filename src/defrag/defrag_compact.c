// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : defrag_compact.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     Compaction engine.  Moves blocks to defragment the volume:
//       Phase 1 — data blocks → start of disk
//       Phase 2 — dedup blocks → after data blocks
//       Phase 3 — B+Tree nodes → end of disk (contiguous, with clump gaps)
//
//     Every block move is crash-safe: data is written to the new
//     location first, then all references are updated, then a sync
//     is issued before the old location is freed in the bitmap.
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

#include "defrag_compact.h"
#include "defrag_analysis.h"
#include "obmafs.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

#define CANCELLED(s) (atomic_load(&(s)->cancel_requested))

/* ------------------------------------------------------------------ */
/*  Phase labels                                                       */
/* ------------------------------------------------------------------ */

const char *compact_phase_labels[COMPACT_NUM_PHASES] = {
    "Preparing",
    "Relocating trees",
    "Moving data blocks",
    "Moving dedup blocks",
    "Updating references",
    "Flushing metadata",
    "Done"
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** qsort comparator for uint64_t (ascending). */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/** Maximum bytes to copy in one I/O call (16 MB). */
#define COPY_BUF_SIZE (16ULL * 1024 * 1024)

/** Pre-allocated reusable I/O buffer (allocated once in compact_run). */
static __thread uint8_t *g_io_buf  = NULL;
static __thread size_t   g_io_cap  = 0;

static int ensure_io_buf(size_t needed)
{
    if(g_io_buf && g_io_cap >= needed) return 0;
    size_t cap = (needed > COPY_BUF_SIZE) ? needed : COPY_BUF_SIZE;
    uint8_t *p = realloc(g_io_buf, cap);
    if(!p) return -1;
    g_io_buf = p;
    g_io_cap = cap;
    return 0;
}

/** Advance the write cursor past any already-occupied blocks. */
static uint64_t next_free_lba(struct compact_state *state, uint64_t from)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t total = ctx->sb.total_bytes / ctx->sb.block_size;

    while(from < total && obmafs3_bitmap_is_set(ctx, from))
        from++;

    return from;
}

/** Copy a single block using the reusable buffer.
 *  Issues a posix_fadvise prefetch hint before reading. */
static int copy_block(struct compact_state *state, uint64_t src, uint64_t dst, size_t size)
{
    struct obmafs3_ctx *ctx = state->ctx;

    if(ensure_io_buf(size) != 0) return OBMAFS3_ERR_NOMEM;

    posix_fadvise(ctx->fd, (off_t)(src * size), (off_t)size, POSIX_FADV_WILLNEED);

    int rc = obmafs3_block_read(ctx, src, g_io_buf, size);
    if(rc != OBMAFS3_OK) return rc;

    return obmafs3_block_write(ctx, dst, g_io_buf, size);
}

/**
 * Bulk copy a contiguous range of blocks using a loop with
 * retry on short reads/writes.  Handles large extents that
 * exceed the I/O buffer by copying in chunks.
 */
static int copy_blocks(struct compact_state *state, uint64_t src, uint64_t dst,
                       uint64_t count, size_t block_size)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t total_bytes = (size_t)count * block_size;

    /* Prefetch the source range */
    posix_fadvise(ctx->fd, (off_t)(src * block_size), (off_t)total_bytes, POSIX_FADV_WILLNEED);

    if(ensure_io_buf(total_bytes) != 0)
    {
        /* Fallback: copy block-by-block */
        for(uint64_t i = 0; i < count; i++)
        {
            int rc = copy_block(state, src + i, dst + i, block_size);
            if(rc != OBMAFS3_OK) return rc;
        }
        return OBMAFS3_OK;
    }

    /* Read with retry on short reads */
    size_t bytes_read = 0;
    while(bytes_read < total_bytes)
    {
        ssize_t rd = pread(ctx->fd, g_io_buf + bytes_read,
                           total_bytes - bytes_read,
                           (off_t)(src * block_size + bytes_read));
        if(rd <= 0) return OBMAFS3_ERR_IO;
        bytes_read += (size_t)rd;
    }

    /* Write with retry on short writes */
    size_t bytes_written = 0;
    while(bytes_written < total_bytes)
    {
        ssize_t wr = pwrite(ctx->fd, g_io_buf + bytes_written,
                            total_bytes - bytes_written,
                            (off_t)(dst * block_size + bytes_written));
        if(wr <= 0) return OBMAFS3_ERR_IO;
        bytes_written += (size_t)wr;
    }

    return OBMAFS3_OK;
}

/** Sync the file descriptor to ensure durability. */
static void sync_fd(struct compact_state *state)
{
    fdatasync(state->ctx->fd);
}

/** Update bitmap: set new, clear old, flush. */
static int bitmap_move(struct compact_state *state, uint64_t old_lba,
                       uint64_t new_lba, uint64_t count)
{
    struct obmafs3_ctx *ctx = state->ctx;

    obmafs3_bitmap_set(ctx, new_lba, count);
    obmafs3_bitmap_clear(ctx, old_lba, count);

    return obmafs3_bitmap_write(ctx);
}

/** Find a free LBA near the end of the disk for eviction targets.
 *  Uses a cached cursor to avoid rescanning from the end every time. */
static uint64_t find_free_at_end(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t total = ctx->sb.total_bytes / ctx->sb.block_size;
    static __thread uint64_t end_cursor = 0;

    if(end_cursor == 0 || end_cursor >= total - 1)
        end_cursor = total - 2;

    /* Scan backwards from the cached position */
    while(end_cursor > 0)
    {
        if(!obmafs3_bitmap_is_set(ctx, end_cursor))
            return end_cursor--;
        end_cursor--;
    }

    /* Wrapped — reset and try again from the very end */
    end_cursor = total - 2;
    while(end_cursor > 0)
    {
        if(!obmafs3_bitmap_is_set(ctx, end_cursor))
            return end_cursor--;
        end_cursor--;
    }

    return 0;
}

/**
 * Find a contiguous free range of @p count blocks near the end of
 * the disk.  Uses a decreasing search cursor to avoid O(n) rescans.
 */
static uint64_t find_free_range_at_end(struct compact_state *state,
                                       uint64_t count, uint64_t not_before)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t total = ctx->sb.total_bytes / ctx->sb.block_size;
    static __thread uint64_t range_cursor = 0;

    if(range_cursor == 0 || range_cursor >= total - 1)
        range_cursor = total - 2;

    while(range_cursor >= not_before + count)
    {
        uint64_t base = range_cursor - count + 1;
        int ok = 1;
        for(uint64_t j = 0; j < count; j++)
        {
            if(obmafs3_bitmap_is_set(ctx, base + j))
            {
                ok = 0;
                range_cursor = base > 0 ? base - 1 : 0;
                break;
            }
        }
        if(ok)
        {
            range_cursor = base > 0 ? base - 1 : 0;
            return base;
        }
        if(base == 0) break;
    }
    return 0;
}

/**
 * Find a contiguous free range of @p count blocks starting at or
 * after @p from. Evicts dedup blocks that are in the way.
 * Returns the start LBA of the free range, or 0 if none found.
 */
static uint64_t find_free_range(struct compact_state *state, uint64_t from,
                                uint64_t count, uint64_t not_past)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;
    uint64_t total = ctx->sb.total_bytes / ctx->sb.block_size;
    uint8_t *block_types = state->analysis->block_types;

    if(not_past == 0) not_past = total;

    uint64_t pos = from;
    uint64_t positions_scanned = 0;
    while(pos + count <= total && pos + count <= not_past)
    {
        positions_scanned++;
        /* Skip metadata blocks */
        if(block_types[pos] == BT_META) { pos++; continue; }
        
        /* Quick skip: if first block is BT_USED/BT_TREE, skip it
         * immediately without checking the whole range */
        if(block_types[pos] == BT_USED || block_types[pos] == BT_TREE)
        { pos++; continue; }

        /* Check if [pos, pos+count) can be made free.
         * Only BT_FREE and BT_DEDUP are acceptable — dedup will be evicted.
         * BT_META, BT_TREE, and BT_USED are immovable from here
         * (BT_USED belongs to other inodes and must NOT be overwritten). */
        int usable = 1;
        for(uint64_t i = 0; i < count; i++)
        {
            uint8_t bt = block_types[pos + i];
            if(bt == BT_META || bt == BT_TREE || bt == BT_USED)
            {
                /* Immovable — skip past */
                pos = pos + i + 1;
                usable = 0;
                break;
            }
            /* BT_FREE and BT_DEDUP are OK (dedup will be evicted) */
        }
        if(!usable) continue;

        /* Evict dedup blocks in our target range.
         * Each dedup DATA block must be evicted as a complete unit
         * (all physical blocks together) to avoid splitting them.
         * We find the base LBA (first block of the dedup data block),
         * read its block_header to get the actual physical size,
         * and move the entire thing. */
        int evicted_any = 0;
        struct timespec t_evict_start;
        clock_gettime(CLOCK_MONOTONIC, &t_evict_start);
        uint64_t i = 0;
        while(i < count)
        {
            if(block_types[pos + i] != BT_DEDUP)
            {
                i++;
                continue;
            }

            /* Find the base of this dedup data block by scanning
             * backwards from pos+i to find the first BT_DEDUP block
             * that's preceded by a non-BT_DEDUP block. */
            uint64_t base_lba = pos + i;
            while(base_lba > 0 && block_types[base_lba - 1] == BT_DEDUP)
                base_lba--;

            /* Read the block_header at the base to get actual size */
            struct block_header dbhdr;
            ssize_t hrd = pread(ctx->fd, &dbhdr, sizeof(dbhdr),
                                (off_t)(base_lba * bsz));
            uint64_t phys_blocks;
            if(hrd >= (ssize_t)sizeof(dbhdr) && dbhdr.magic == OBMAFS3_BLOCK_MAGIC)
            {
                uint64_t payload = (dbhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                                       ? dbhdr.compressed_size
                                       : dbhdr.original_size;
                phys_blocks = (sizeof(dbhdr) + payload + bsz - 1) / bsz;
            }
            else
            {
                /* Can't read header — use contiguous BT_DEDUP run */
                phys_blocks = 1;
                while(base_lba + phys_blocks < total &&
                      block_types[base_lba + phys_blocks] == BT_DEDUP)
                    phys_blocks++;
            }

            /* Evict the ENTIRE dedup data block [base_lba, base_lba+phys_blocks) */
            uint64_t evict_dst = find_free_range_at_end(state, phys_blocks,
                                                        pos + count);
            if(evict_dst == 0)
            {
                /* Can't find contiguous space — skip past this dedup block */
                i = (base_lba + phys_blocks > pos) ? (base_lba + phys_blocks - pos) : i + 1;
                continue;
            }

            atomic_store(&state->current_src_lba, base_lba);
            atomic_store(&state->current_dst_lba, evict_dst);

            int rc = copy_blocks(state, base_lba, evict_dst, phys_blocks, bsz);
            if(rc != OBMAFS3_OK) return 0;

            obmafs3_bitmap_set(ctx, evict_dst, phys_blocks);
            obmafs3_bitmap_clear(ctx, base_lba, phys_blocks);

            for(uint64_t j = 0; j < phys_blocks; j++)
            {
                block_types[evict_dst + j] = BT_DEDUP;
                if(base_lba + j < total)
                    block_types[base_lba + j] = BT_FREE;
            }
            evicted_any = 1;

            fprintf(stderr, "[evict-dedup] %" PRIu64 "->%" PRIu64 " (%" PRIu64 " blks, base=%" PRIu64 ")\n",
                    base_lba, evict_dst, phys_blocks, base_lba);

            /* Record relocations for ALL blocks of this dedup data block */
            for(uint64_t j = 0; j < phys_blocks; j++)
            {
                if(state->reloc_count >= state->reloc_cap)
                {
                    uint64_t nc = (state->reloc_cap == 0) ? 4096 : state->reloc_cap * 2;
                    uint64_t *ro = realloc(state->reloc_old, nc * sizeof(uint64_t));
                    uint64_t *rn = realloc(state->reloc_new, nc * sizeof(uint64_t));
                    if(ro && rn) { state->reloc_old = ro; state->reloc_new = rn; state->reloc_cap = nc; }
                }
                if(state->reloc_count < state->reloc_cap)
                {
                    state->reloc_old[state->reloc_count] = base_lba + j;
                    state->reloc_new[state->reloc_count] = evict_dst + j;
                    state->reloc_count++;
                }
            }

            /* Advance past the evicted region */
            uint64_t end_in_range = base_lba + phys_blocks;
            i = (end_in_range > pos) ? (end_in_range - pos) : i + 1;
        }

        /* Track that bitmap is dirty — caller will flush in batches */
        if(evicted_any)
        {
            state->data_blocks_moved++; /* reuse as dirty flag */
        }

        return pos;
    }

    if(positions_scanned > 10000)
        fprintf(stderr, "[find_free_range] SLOW: scanned %" PRIu64
                " positions for count=%" PRIu64 " from=%" PRIu64 " — NOT FOUND\n",
                positions_scanned, count, from);
    return 0;
}

/**
 * Move a single extent to a new contiguous location and update the
 * extent_run in-place (caller must write the modified node back).
 *
 * Returns 0 on success.
 */
static int move_extent(struct compact_state *state, struct extent_run *ext,
                       uint64_t new_start)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;

    if(ext->start_block == new_start) return OBMAFS3_OK; /* already there */

    atomic_store(&state->current_src_lba, ext->start_block);
    atomic_store(&state->current_dst_lba, new_start);

    /* Copy whole extent as one bulk I/O */
    int rc = copy_blocks(state, ext->start_block, new_start,
                         ext->block_count, bsz);
    if(rc != OBMAFS3_OK) return rc;

    /* Update bitmap in memory (caller flushes in batch) */
    obmafs3_bitmap_set(ctx, new_start, ext->block_count);
    obmafs3_bitmap_clear(ctx, ext->start_block, ext->block_count);

    /* Update block_types */
    uint8_t *bt = state->analysis->block_types;
    for(uint64_t i = 0; i < ext->block_count; i++)
    {
        bt[new_start + i] = BT_USED;
        bt[ext->start_block + i] = BT_FREE;
    }

    /* Update the extent in-place */
    ext->start_block = new_start;

    state->data_blocks_moved += ext->block_count;
    atomic_fetch_add(&state->done_steps, ext->block_count);

    return OBMAFS3_OK;
}

/* Overflow extent collection record for two-pass compaction */
struct ovf_collect {
    uint64_t node_lba;
    uint16_t record_idx;
    uint64_t start_block;
    uint64_t block_count;
    uint64_t logical_count;
};

static int cmp_ovf_by_start(const void *a, const void *b)
{
    const struct ovf_collect *oa = (const struct ovf_collect *)a;
    const struct ovf_collect *ob = (const struct ovf_collect *)b;
    return (oa->start_block > ob->start_block) - (oa->start_block < ob->start_block);
}

/**
 * Walk the inode tree and move each inode's extents as contiguous
 * units to pack data toward the start of the disk.
 *
 * Key property: entire extents are moved atomically — the data
 * inside (including compressed SME entries) is byte-identical at
 * the new location.  Only extent_run.start_block needs updating.
 */
static int compact_data_blocks(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;

    if(ctx->sb.inode_lba == 0) return OBMAFS3_OK;

    struct btree_header ihdr;
    int rc = obmafs3_btree_header_read(ctx, ctx->sb.inode_lba, &ihdr);
    if(rc != OBMAFS3_OK) return rc;
    if(ihdr.root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *node_buf = calloc(1, bsz);
    if(!node_buf) return OBMAFS3_ERR_NOMEM;

    state->write_cursor = 1; /* Start after superblock */

    /* Level-order BFS with sorted LBAs for sequential I/O */
    uint64_t *cur_level = malloc(256 * sizeof(uint64_t));
    uint64_t  cur_count = 0, cur_cap = 256;
    uint64_t *nxt_level = malloc(256 * sizeof(uint64_t));
    uint64_t  nxt_count = 0, nxt_cap = 256;
    if(!cur_level || !nxt_level)
    {
        free(cur_level); free(nxt_level); free(node_buf);
        return OBMAFS3_ERR_NOMEM;
    }

    cur_level[cur_count++] = ihdr.root_node_lba;
    uint64_t leaves_since_sync = 0;
    #define SYNC_EVERY_N_LEAVES 256
    #define PREFETCH_BATCH 64

    fprintf(stderr, "[data-phase] START root_lba=%" PRIu64 "\n", ihdr.root_node_lba);
    struct timespec phase_start;
    clock_gettime(CLOCK_MONOTONIC, &phase_start);

    while(cur_count > 0)
    {
        if(CANCELLED(state)) break;

        fprintf(stderr, "[data-phase] level: %" PRIu64 " nodes\n", cur_count);
        struct timespec level_start;
        clock_gettime(CLOCK_MONOTONIC, &level_start);

        /* Sort for sequential I/O */
        if(cur_count > 1)
            qsort(cur_level, (size_t)cur_count, sizeof(uint64_t), cmp_u64);

        nxt_count = 0;
        uint64_t prefetched = 0;

        for(uint64_t ci = 0; ci < cur_count; ci++)
        {
            if(CANCELLED(state)) break;

            /* Prefetch in batches */
            if(ci >= prefetched)
            {
                uint64_t end = ci + PREFETCH_BATCH;
                if(end > cur_count) end = cur_count;
                for(uint64_t p = ci; p < end; p++)
                    posix_fadvise(ctx->fd, (off_t)(cur_level[p] * bsz),
                                  (off_t)bsz, POSIX_FADV_WILLNEED);
                prefetched = end;
            }

            uint64_t lba = cur_level[ci];
            if(lba == 0) continue;

            rc = obmafs3_block_read(ctx, lba, node_buf, bsz);
            if(rc != OBMAFS3_OK) continue;

            struct btree_node_header nh;
            memcpy(&nh, node_buf, sizeof(nh));
            if(nh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

            if(nh.level > 0)
            {
                const uint8_t *records = node_buf + sizeof(struct btree_node_header);
                for(uint16_t i = 0; i < nh.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, records + (size_t)i * sizeof(ie), sizeof(ie));
                    if(ie.child_lba == 0) continue;
                    if(nxt_count >= nxt_cap) { nxt_cap *= 2; uint64_t *t = realloc(nxt_level, nxt_cap * sizeof(uint64_t)); if(!t) break; nxt_level = t; }
                    nxt_level[nxt_count++] = ie.child_lba;
                }
                continue;
            }

        /* Leaf node: process each inode's inline extents */
        int node_modified = 0;
        uint8_t *records = node_buf + sizeof(struct btree_node_header);

        static uint64_t total_extents = 0, moved_extents = 0, skipped_extents = 0;
        static uint64_t eviction_calls = 0, eviction_blocks = 0;
        static uint64_t leaf_count = 0;

        leaf_count++;
        if((leaf_count % 100) == 0)
        {
            fprintf(stderr, "[data-phase] leaf=%"PRIu64" extents: total=%"PRIu64
                    " moved=%"PRIu64" skipped=%"PRIu64
                    " evictions=%"PRIu64"/%"PRIu64"blks cursor=%"PRIu64"\n",
                    leaf_count, total_extents, moved_extents, skipped_extents,
                    eviction_calls, eviction_blocks, state->write_cursor);
        }

        for(uint16_t i = 0; i < nh.node_keys; i++)
        {
            struct inode_record *irec = (struct inode_record *)
                (records + (size_t)i * sizeof(struct inode_record));

            for(uint8_t e = 0; e < 8; e++)
            {
                if(irec->extents[e].start_block == 0 ||
                   irec->extents[e].block_count == 0)
                    continue;

                total_extents++;

                /* Skip if already at or before the cursor (in the compacted region) */
                if(irec->extents[e].start_block <= state->write_cursor)
                {
                    uint64_t end = irec->extents[e].start_block +
                                   irec->extents[e].block_count;
                    if(end > state->write_cursor)
                        state->write_cursor = end;
                    skipped_extents++;
                    continue;
                }

                /* Find a contiguous free range for this extent */
                uint64_t dst = find_free_range(state, state->write_cursor,
                                               irec->extents[e].block_count,
                                               irec->extents[e].start_block);
                if(dst == 0)
                {
                    skipped_extents++;
                    continue; /* No space */
                }

                /* Already in place — just advance cursor */
                if(dst == irec->extents[e].start_block)
                {
                    state->write_cursor = dst + irec->extents[e].block_count;
                    skipped_extents++;
                    continue;
                }

                rc = move_extent(state, &irec->extents[e], dst);
                if(rc != OBMAFS3_OK)
                {
                    skipped_extents++;
                    continue;
                }

                moved_extents++;
                node_modified = 1;
                state->write_cursor = dst + irec->extents[e].block_count;
            }
        }

        /* Write back the modified leaf node with updated start_blocks,
         * flush the bitmap, and sync — batched per N leaf nodes. */
        if(node_modified)
        {
            struct btree_node_header *nhp = (struct btree_node_header *)node_buf;
            memset(nhp->checksum, 0, sizeof(nhp->checksum));
            { size_t cs_len = sizeof(struct btree_node_header) + nhp->keys_length; obmafs3_checksum_block(node_buf, cs_len, nhp->checksum); };
            obmafs3_block_write(ctx, lba, node_buf, bsz);
            leaves_since_sync++;

            if(leaves_since_sync >= SYNC_EVERY_N_LEAVES)
            {
                obmafs3_bitmap_write(ctx);
                sync_fd(state);
                leaves_since_sync = 0;
            }
        }
        }

        struct timespec level_end;
        clock_gettime(CLOCK_MONOTONIC, &level_end);
        double level_secs = (double)(level_end.tv_sec - level_start.tv_sec) +
                            (double)(level_end.tv_nsec - level_start.tv_nsec) / 1e9;
        fprintf(stderr, "[data-phase] level done: %.1fs, next_count=%" PRIu64 "\n",
                level_secs, nxt_count);

        /* Swap levels */
        uint64_t *tmp_ptr = cur_level;
        cur_level = nxt_level;
        nxt_level = tmp_ptr;
        cur_count = nxt_count;
        uint64_t tmp_cap = cur_cap;
        cur_cap = nxt_cap;
        nxt_cap = tmp_cap;
    }

    /* Final flush for any remaining unsync'd leaves */
    if(leaves_since_sync > 0)
    {
        obmafs3_bitmap_write(ctx);
        sync_fd(state);
    }

    {
        struct timespec phase_end;
        clock_gettime(CLOCK_MONOTONIC, &phase_end);
        double total_secs = (double)(phase_end.tv_sec - phase_start.tv_sec) +
                            (double)(phase_end.tv_nsec - phase_start.tv_nsec) / 1e9;
        fprintf(stderr, "[data-phase] DONE: %.1fs total, cursor=%" PRIu64 "\n",
                total_secs, state->write_cursor);
    }

    #undef SYNC_EVERY_N_LEAVES
    #undef PREFETCH_BATCH

    free(cur_level);
    free(nxt_level);
    free(node_buf);

    /* Also process overflow extents — two-pass approach:
     * Pass 1: BFS the overflow tree, collect ALL overflow extents
     *         with their node LBA and record index.
     * Pass 2: Sort by start_block, process in LBA order so the
     *         write cursor advances monotonically. */
    if(ctx->sb.overflow_lba != 0)
    {
        fprintf(stderr, "[data-phase] Processing overflow tree at lba=%" PRIu64 "\n",
                ctx->sb.overflow_lba);
        struct timespec ovf_start;
        clock_gettime(CLOCK_MONOTONIC, &ovf_start);

        struct btree_header ohdr;
        rc = obmafs3_btree_header_read(ctx, ctx->sb.overflow_lba, &ohdr);
        if(rc == OBMAFS3_OK && ohdr.root_node_lba != 0)
        {
            /* --- Pass 1: Collect all overflow extents --- */

            uint64_t oc_cap = 4096, oc_count = 0;
            struct ovf_collect *oc = malloc(oc_cap * sizeof(*oc));
            node_buf = calloc(1, bsz);

            if(oc && node_buf)
            {
                /* Level-order BFS with sorted LBAs + prefetch */
                uint64_t *olev = malloc(256 * sizeof(uint64_t));
                uint64_t *onlev = malloc(256 * sizeof(uint64_t));
                uint64_t olev_n = 0, olev_cap = 256;
                uint64_t onlev_n = 0, onlev_cap = 256;

                if(olev && onlev)
                {
                    olev[olev_n++] = ohdr.root_node_lba;

                    while(olev_n > 0)
                    {
                        if(olev_n > 1)
                            qsort(olev, (size_t)olev_n, sizeof(uint64_t), cmp_u64);

                        onlev_n = 0;
                        uint64_t opf = 0;

                        for(uint64_t oi = 0; oi < olev_n; oi++)
                        {
                            if(oi >= opf)
                            {
                                uint64_t oe2 = oi + 64;
                                if(oe2 > olev_n) oe2 = olev_n;
                                for(uint64_t op = oi; op < oe2; op++)
                                    posix_fadvise(ctx->fd, (off_t)(olev[op] * bsz),
                                                  (off_t)bsz, POSIX_FADV_WILLNEED);
                                opf = oe2;
                            }

                            uint64_t olba = olev[oi];
                            rc = obmafs3_block_read(ctx, olba, node_buf, bsz);
                            if(rc != OBMAFS3_OK) continue;

                            struct btree_node_header onh;
                            memcpy(&onh, node_buf, sizeof(onh));
                            if(onh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

                            if(onh.level > 0)
                            {
                                const uint8_t *recs = node_buf + sizeof(struct btree_node_header);
                                for(uint16_t ri = 0; ri < onh.node_keys; ri++)
                                {
                                    struct overflow_index_entry oie;
                                    memcpy(&oie, recs + (size_t)ri * sizeof(oie), sizeof(oie));
                                    if(oie.child_lba == 0) continue;
                                    if(onlev_n >= onlev_cap) { onlev_cap *= 2; uint64_t *t2 = realloc(onlev, onlev_cap * sizeof(uint64_t)); if(!t2) break; onlev = t2; }
                                    onlev[onlev_n++] = oie.child_lba;
                                }
                                continue;
                            }

                            /* Leaf: collect each overflow_extent */
                            const uint8_t *recs = node_buf + sizeof(struct btree_node_header);
                            for(uint16_t ri = 0; ri < onh.node_keys; ri++)
                            {
                                struct overflow_extent oe2;
                                memcpy(&oe2, recs + (size_t)ri * sizeof(oe2), sizeof(oe2));
                                if(oe2.start_block == 0 || oe2.block_count == 0) continue;

                                if(oc_count >= oc_cap)
                                {
                                    oc_cap *= 2;
                                    struct ovf_collect *t2 = realloc(oc, oc_cap * sizeof(*oc));
                                    if(!t2) break;
                                    oc = t2;
                                }
                                oc[oc_count].node_lba = olba;
                                oc[oc_count].record_idx = ri;
                                oc[oc_count].start_block = oe2.start_block;
                                oc[oc_count].block_count = oe2.block_count;
                                oc[oc_count].logical_count = oe2.logical_count;
                                oc_count++;
                            }
                        }

                        uint64_t *otmp = olev; olev = onlev; onlev = otmp;
                        olev_n = onlev_n;
                        uint64_t otcap = olev_cap; olev_cap = onlev_cap; onlev_cap = otcap;
                    }
                }
                free(olev);
                free(onlev);

                fprintf(stderr, "[overflow] collected %" PRIu64 " extents\n", oc_count);

                /* --- Pass 2: Sort by start_block --- */
                if(oc_count > 1)
                    qsort(oc, (size_t)oc_count, sizeof(*oc), cmp_ovf_by_start);

                /* --- Pass 3: Process in LBA order --- */
                uint64_t ovf_moved = 0, ovf_skipped = 0;
                /* Track which leaf nodes were modified: node_lba → dirty flag.
                 * We need to rewrite modified leaves. Use a simple array. */
                uint64_t *dirty_nodes = NULL;
                uint64_t  dirty_count = 0, dirty_cap = 0;

                for(uint64_t ei = 0; ei < oc_count; ei++)
                {
                    if(CANCELLED(state)) break;

                    struct ovf_collect *entry = &oc[ei];

                    struct timespec t_ext_start;
                    clock_gettime(CLOCK_MONOTONIC, &t_ext_start);

                    /* Skip if already in compacted region */
                    if(entry->start_block <= state->write_cursor)
                    {
                        uint64_t end = entry->start_block + entry->block_count;
                        if(end > state->write_cursor)
                            state->write_cursor = end;
                        ovf_skipped++;
                        continue;
                    }

                    /* Try to find space before this extent */
                    struct timespec t_find_s;
                    clock_gettime(CLOCK_MONOTONIC, &t_find_s);

                    uint64_t dst = find_free_range(state, state->write_cursor,
                                                   entry->block_count,
                                                   entry->start_block);

                    struct timespec t_find_e;
                    clock_gettime(CLOCK_MONOTONIC, &t_find_e);
                    double find_ms = ((double)(t_find_e.tv_sec - t_find_s.tv_sec) * 1000.0) +
                                     ((double)(t_find_e.tv_nsec - t_find_s.tv_nsec) / 1e6);

                    if(dst == 0 || dst == entry->start_block)
                    {
                        /* Can't improve — skip */
                        state->write_cursor = entry->start_block + entry->block_count;
                        ovf_skipped++;

                        struct timespec t_ext_end;
                        clock_gettime(CLOCK_MONOTONIC, &t_ext_end);
                        double ext_ms = ((double)(t_ext_end.tv_sec - t_ext_start.tv_sec) * 1000.0) +
                                        ((double)(t_ext_end.tv_nsec - t_ext_start.tv_nsec) / 1e6);
                        if(ext_ms > 10.0)
                            fprintf(stderr, "[ovf %"PRIu64"/%"PRIu64"] SKIP lba=%"PRIu64" (%"PRIu64" blks) find=%.0fms total=%.0fms\n",
                                    ei, oc_count, entry->start_block, entry->block_count, find_ms, ext_ms);
                        continue;
                    }

                    struct timespec t_copy_s;
                    clock_gettime(CLOCK_MONOTONIC, &t_copy_s);

                    struct extent_run tmp_ext;
                    tmp_ext.start_block   = entry->start_block;
                    tmp_ext.block_count   = entry->block_count;
                    tmp_ext.logical_blocks = entry->logical_count;

                    rc = move_extent(state, &tmp_ext, dst);

                    struct timespec t_copy_e;
                    clock_gettime(CLOCK_MONOTONIC, &t_copy_e);
                    double copy_ms = ((double)(t_copy_e.tv_sec - t_copy_s.tv_sec) * 1000.0) +
                                     ((double)(t_copy_e.tv_nsec - t_copy_s.tv_nsec) / 1e6);

                    if(rc != OBMAFS3_OK)
                    {
                        ovf_skipped++;
                        fprintf(stderr, "[ovf %"PRIu64"/%"PRIu64"] FAIL %"PRIu64"->%"PRIu64" (%"PRIu64" blks) find=%.0fms copy=%.0fms rc=%d\n",
                                ei, oc_count, entry->start_block, dst, entry->block_count, find_ms, copy_ms, rc);
                        continue;
                    }

                    /* Update our collected entry */
                    uint64_t old_start = entry->start_block;
                    entry->start_block = dst;
                    ovf_moved++;
                    state->write_cursor = dst + entry->block_count;

                    struct timespec t_ext_end;
                    clock_gettime(CLOCK_MONOTONIC, &t_ext_end);
                    double ext_ms = ((double)(t_ext_end.tv_sec - t_ext_start.tv_sec) * 1000.0) +
                                    ((double)(t_ext_end.tv_nsec - t_ext_start.tv_nsec) / 1e6);

                    fprintf(stderr, "[ovf %"PRIu64"/%"PRIu64"] MOVE %"PRIu64"->%"PRIu64" (%"PRIu64" blks) find=%.0fms copy=%.0fms total=%.0fms\n",
                            ei, oc_count, old_start, dst, entry->block_count, find_ms, copy_ms, ext_ms);

                    /* Mark this leaf node as dirty */
                    int found_dirty = 0;
                    for(uint64_t di = 0; di < dirty_count; di++)
                    {
                        if(dirty_nodes[di] == entry->node_lba)
                        { found_dirty = 1; break; }
                    }
                    if(!found_dirty)
                    {
                        if(dirty_count >= dirty_cap)
                        {
                            dirty_cap = (dirty_cap == 0) ? 256 : dirty_cap * 2;
                            uint64_t *t2 = realloc(dirty_nodes, dirty_cap * sizeof(uint64_t));
                            if(t2) dirty_nodes = t2;
                        }
                        if(dirty_count < dirty_cap)
                            dirty_nodes[dirty_count++] = entry->node_lba;
                    }

                    /* Periodic batch sync every 512 moved extents */
                    if(ovf_moved > 0 && (ovf_moved % 512) == 0)
                    {
                        obmafs3_bitmap_write(ctx);
                        sync_fd(state);
                        fprintf(stderr, "[overflow] batch sync at %"PRIu64"/%"PRIu64" moved=%"PRIu64"\n",
                                ei, oc_count, ovf_moved);
                    }
                }

                /* Final sync before writing back dirty nodes */
                obmafs3_bitmap_write(ctx);
                sync_fd(state);

                fprintf(stderr, "[overflow] moved=%" PRIu64 " skipped=%" PRIu64
                        " dirty_nodes=%" PRIu64 "\n",
                        ovf_moved, ovf_skipped, dirty_count);

                /* --- Pass 4: Rewrite dirty leaf nodes --- */
                fprintf(stderr, "[pass4] dirty_count=%" PRIu64 " oc_count=%" PRIu64 "\n",
                        dirty_count, oc_count);
                for(uint64_t di = 0; di < dirty_count; di++)
                {
                    uint64_t dlba = dirty_nodes[di];
                    rc = obmafs3_block_read(ctx, dlba, node_buf, bsz);
                    if(rc != OBMAFS3_OK) continue;

                    struct btree_node_header dnh;
                    memcpy(&dnh, node_buf, sizeof(dnh));
                    if(dnh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

                    /* Apply all collected updates for this node */
                    uint8_t *recs = node_buf + sizeof(struct btree_node_header);
                    int any_mod = 0;
                    for(uint64_t ei = 0; ei < oc_count; ei++)
                    {
                        if(oc[ei].node_lba != dlba) continue;

                        struct overflow_extent *oe2 = (struct overflow_extent *)
                            (recs + (size_t)oc[ei].record_idx * sizeof(struct overflow_extent));

                        if(oe2->start_block != oc[ei].start_block)
                        {
                            fprintf(stderr, "[pass4] node=%" PRIu64 " rec=%u: %"PRIu64"->%"PRIu64"\n",
                                    dlba, oc[ei].record_idx, oe2->start_block, oc[ei].start_block);
                            oe2->start_block = oc[ei].start_block;
                            any_mod = 1;
                        }
                    }

                    if(any_mod)
                    {
                        struct btree_node_header *nhp = (struct btree_node_header *)node_buf;
                        memset(nhp->checksum, 0, sizeof(nhp->checksum));
                        { size_t cs_len = sizeof(struct btree_node_header) + nhp->keys_length; obmafs3_checksum_block(node_buf, cs_len, nhp->checksum); }
                        obmafs3_block_write(ctx, dlba, node_buf, bsz);
                    }
                }

                /* Final flush */
                if(dirty_count > 0)
                {
                    obmafs3_bitmap_write(ctx);
                    sync_fd(state);
                }

                /* Post-Pass4 verify: check first 5 moved extents on disk */
                {
                    uint8_t *vbuf = calloc(1, bsz);
                    uint8_t *dbuf = calloc(1, bsz);
                    if(vbuf && dbuf)
                    {
                        uint64_t dumped = 0;
                        for(uint64_t ei = 0; ei < oc_count && dumped < 5; ei++)
                        {
                            if(oc[ei].start_block == 0) continue;
                            /* Read the overflow tree record */
                            rc = obmafs3_block_read(ctx, oc[ei].node_lba, vbuf, bsz);
                            if(rc != OBMAFS3_OK) continue;
                            struct overflow_extent *oe_v = (struct overflow_extent *)
                                (vbuf + sizeof(struct btree_node_header) +
                                 (size_t)oc[ei].record_idx * sizeof(struct overflow_extent));

                            /* Read first block at the extent's start_block */
                            rc = obmafs3_block_read(ctx, oe_v->start_block, dbuf, bsz);
                            struct block_header *bh = (struct block_header *)dbuf;
                            dumped++;
                        }
                    }
                    free(vbuf);
                    free(dbuf);
                }

                free(dirty_nodes);
            }
            free(oc);
            free(node_buf);

            struct timespec ovf_end;
            clock_gettime(CLOCK_MONOTONIC, &ovf_end);
            double ovf_secs = (double)(ovf_end.tv_sec - ovf_start.tv_sec) +
                              (double)(ovf_end.tv_nsec - ovf_start.tv_nsec) / 1e9;
            fprintf(stderr, "[data-phase] Overflow tree done: %.1fs\n", ovf_secs);
        }
    }

    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Phase 2: Move dedup data blocks after data blocks                  */
/* ------------------------------------------------------------------ */

/**
 * Walk the analysis block_types array to find all BT_DEDUP blocks.
 * For each dedup data block (which may span multiple standard blocks),
 * copy it to the next free position after the data blocks.
 * If a non-dedup block (tree node, etc.) occupies any part of the
 * target range, it is evicted to a free spot near the end of the
 * disk first, ensuring no free-space holes between dedup blocks.
 *
 * Atomic sequence per dedup block:
 *   1. Evict any obstacles in the target range
 *   2. Copy dedup block(s) to new location
 *   3. fdatasync
 *   4. Update bitmap (set new, clear old)
 *   5. Flush bitmap
 */
static int compact_dedup_blocks(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
    uint8_t *block_types = state->analysis->block_types;
    uint64_t dedup_block_size = ctx->sb.dedup_block_size;
    uint64_t std_per_dedup = dedup_block_size / ctx->sb.block_size;

    if(std_per_dedup < 1) std_per_dedup = 1;

    uint64_t scan = state->write_cursor;

    #define DEDUP_BATCH_SIZE 65536
    uint64_t *pending_old = malloc(DEDUP_BATCH_SIZE * sizeof(uint64_t));
    uint64_t *pending_new = malloc(DEDUP_BATCH_SIZE * sizeof(uint64_t));
    uint64_t *pending_cnt = malloc(DEDUP_BATCH_SIZE * sizeof(uint64_t));
    uint64_t  pending_n = 0;

    if(!pending_old || !pending_new || !pending_cnt)
    {
        free(pending_old); free(pending_new); free(pending_cnt);
        return OBMAFS3_ERR_NOMEM;
    }

    for(;;)
    {
        /* Check for cancellation — flush pending and exit safely */
        if(CANCELLED(state))
        {
            if(pending_n > 0)
            {
                sync_fd(state);
                for(uint64_t p = 0; p < pending_n; p++)
                {
                    obmafs3_bitmap_set(ctx, pending_new[p], pending_cnt[p]);
                    obmafs3_bitmap_clear(ctx, pending_old[p], pending_cnt[p]);
                }
                obmafs3_bitmap_write(ctx);
                sync_fd(state);
            }
            free(pending_old); free(pending_new); free(pending_cnt);
            return OBMAFS3_OK;
        }

        while(state->write_cursor < total_blocks &&
              (block_types[state->write_cursor] == BT_USED ||
               block_types[state->write_cursor] == BT_META))
            state->write_cursor++;

        if(state->write_cursor < total_blocks &&
           block_types[state->write_cursor] == BT_DEDUP)
        {
            state->write_cursor++;
            continue;
        }

        if(scan <= state->write_cursor)
            scan = state->write_cursor + 1;

        while(scan < total_blocks && block_types[scan] != BT_DEDUP)
            scan++;

        if(scan >= total_blocks)
        {
            /* Flush remaining pending bitmap updates */
            if(pending_n > 0)
            {
                sync_fd(state);
                for(uint64_t p = 0; p < pending_n; p++)
                {
                    obmafs3_bitmap_set(ctx, pending_new[p], pending_cnt[p]);
                    obmafs3_bitmap_clear(ctx, pending_old[p], pending_cnt[p]);
                }
                obmafs3_bitmap_write(ctx);
                sync_fd(state);
            }
            break;
        }

        /* Determine actual physical size of the dedup data block at scan
         * by reading its block_header. */
        uint64_t run_len;
        {
            struct block_header dbhdr;
            ssize_t hrd = pread(ctx->fd, &dbhdr, sizeof(dbhdr),
                                (off_t)(scan * bsz));
            if(hrd >= (ssize_t)sizeof(dbhdr) && dbhdr.magic == OBMAFS3_BLOCK_MAGIC)
            {
                uint64_t payload = (dbhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                                       ? dbhdr.compressed_size
                                       : dbhdr.original_size;
                run_len = (sizeof(dbhdr) + payload + bsz - 1) / bsz;
                if(run_len > std_per_dedup)
                    run_len = std_per_dedup;
            }
            else
            {
                /* Can't read header — use contiguous BT_DEDUP run */
                run_len = 1;
                while(scan + run_len < total_blocks &&
                      block_types[scan + run_len] == BT_DEDUP &&
                      run_len < std_per_dedup)
                    run_len++;
            }
        }

        uint64_t dst = state->write_cursor;

        /* Evict obstacles — flush pending first for safety */
        int need_evict = 0;
        for(uint64_t i = 0; i < run_len; i++)
        {
            uint64_t pos = dst + i;
            if(pos < total_blocks && obmafs3_bitmap_is_set(ctx, pos) &&
               block_types[pos] != BT_FREE)
            {
                need_evict = 1;
                break;
            }
        }

        if(need_evict)
        {
            /* Flush pending batch before evicting */
            if(pending_n > 0)
            {
                sync_fd(state);
                for(uint64_t p = 0; p < pending_n; p++)
                {
                    obmafs3_bitmap_set(ctx, pending_new[p], pending_cnt[p]);
                    obmafs3_bitmap_clear(ctx, pending_old[p], pending_cnt[p]);
                }
                obmafs3_bitmap_write(ctx);
                sync_fd(state);
                pending_n = 0;
            }

            /* Evict obstacles as complete dedup data blocks, reading
             * block_header to get actual physical size. */
            uint64_t ei = 0;
            while(ei < run_len)
            {
                uint64_t epos = dst + ei;
                if(epos >= total_blocks || !obmafs3_bitmap_is_set(ctx, epos) ||
                   block_types[epos] == BT_FREE)
                {
                    ei++;
                    continue;
                }

                /* Find base of this dedup data block */
                uint64_t ebase = epos;
                while(ebase > 0 && block_types[ebase - 1] == BT_DEDUP)
                    ebase--;

                /* Read header to get actual size */
                struct block_header ebhdr;
                uint64_t obs_len;
                ssize_t ehrd = pread(ctx->fd, &ebhdr, sizeof(ebhdr),
                                     (off_t)(ebase * bsz));
                if(ehrd >= (ssize_t)sizeof(ebhdr) && ebhdr.magic == OBMAFS3_BLOCK_MAGIC)
                {
                    uint64_t epayload = (ebhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                                            ? ebhdr.compressed_size
                                            : ebhdr.original_size;
                    obs_len = (sizeof(ebhdr) + epayload + bsz - 1) / bsz;
                }
                else
                {
                    obs_len = 1;
                    while(ebase + obs_len < total_blocks &&
                          block_types[ebase + obs_len] == BT_DEDUP)
                        obs_len++;
                }

                /* Evict the ENTIRE dedup data block */
                uint64_t evict_dst = find_free_range_at_end(state, obs_len,
                                                            dst + run_len);

                if(evict_dst == 0)
                {
                    state->write_cursor = ebase + obs_len;
                    break;
                }

                int rc = copy_blocks(state, ebase, evict_dst, obs_len, bsz);
                if(rc != OBMAFS3_OK) break;

                obmafs3_bitmap_set(ctx, evict_dst, obs_len);
                obmafs3_bitmap_clear(ctx, ebase, obs_len);

                for(uint64_t j = 0; j < obs_len; j++)
                {
                    block_types[evict_dst + j] = BT_DEDUP;
                    if(ebase + j < total_blocks)
                        block_types[ebase + j] = BT_FREE;
                }

                for(uint64_t j = 0; j < obs_len; j++)
                {
                    if(state->reloc_count >= state->reloc_cap)
                    {
                        uint64_t nc = (state->reloc_cap == 0) ? 4096 : state->reloc_cap * 2;
                        uint64_t *ro = realloc(state->reloc_old, nc * sizeof(uint64_t));
                        uint64_t *rn = realloc(state->reloc_new, nc * sizeof(uint64_t));
                        if(ro && rn) { state->reloc_old = ro; state->reloc_new = rn; state->reloc_cap = nc; }
                    }
                    if(state->reloc_count < state->reloc_cap)
                    {
                        state->reloc_old[state->reloc_count] = ebase + j;
                        state->reloc_new[state->reloc_count] = evict_dst + j;
                        state->reloc_count++;
                    }
                }

                /* Advance past the evicted block */
                uint64_t eend = ebase + obs_len;
                ei = (eend > dst) ? (eend - dst) : ei + 1;
            }

            /* One bitmap flush + sync for all obstacle evictions */
            obmafs3_bitmap_write(ctx);
            sync_fd(state);
        }

        /* Move the dedup run */
        atomic_store(&state->current_src_lba, scan);
        atomic_store(&state->current_dst_lba, dst);

        int rc = copy_blocks(state, scan, dst, run_len, bsz);
        if(rc != OBMAFS3_OK) { free(pending_old); free(pending_new); free(pending_cnt); return rc; }

        for(uint64_t i = 0; i < run_len; i++)
        {
            block_types[dst + i]  = BT_DEDUP;
            block_types[scan + i] = BT_FREE;
        }

        /* Record each relocated block individually so that
         * dedup_entry.block_lba lookups work for any block within
         * the run, not just the first one. */
        if(scan != dst)
        {
            for(uint64_t ri = 0; ri < run_len; ri++)
            {
                if(state->reloc_count >= state->reloc_cap)
                {
                    uint64_t new_cap = (state->reloc_cap == 0) ? 4096 : state->reloc_cap * 2;
                    uint64_t *ro = realloc(state->reloc_old, new_cap * sizeof(uint64_t));
                    uint64_t *rn = realloc(state->reloc_new, new_cap * sizeof(uint64_t));
                    if(ro && rn)
                    {
                        state->reloc_old = ro;
                        state->reloc_new = rn;
                        state->reloc_cap = new_cap;
                    }
                }
                if(state->reloc_count < state->reloc_cap)
                {
                    state->reloc_old[state->reloc_count] = scan + ri;
                    state->reloc_new[state->reloc_count] = dst + ri;
                    state->reloc_count++;
                }
            }
        }

        /* Queue bitmap update */
        pending_old[pending_n] = scan;
        pending_new[pending_n] = dst;
        pending_cnt[pending_n] = run_len;
        pending_n++;

        state->write_cursor = dst + run_len;
        scan += run_len;
        state->dedup_blocks_moved += run_len;
        atomic_fetch_add(&state->done_steps, run_len);

        /* Flush batch when full */
        if(pending_n >= DEDUP_BATCH_SIZE)
        {
            sync_fd(state);
            for(uint64_t p = 0; p < pending_n; p++)
            {
                obmafs3_bitmap_set(ctx, pending_new[p], pending_cnt[p]);
                obmafs3_bitmap_clear(ctx, pending_old[p], pending_cnt[p]);
            }
            obmafs3_bitmap_write(ctx);
            sync_fd(state);
            pending_n = 0;
        }
    }

    free(pending_old);
    free(pending_new);
    free(pending_cnt);

    #undef DEDUP_BATCH_SIZE

    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Phase 3: Relocate B+Trees contiguously at end of disk              */
/* ------------------------------------------------------------------ */

/**
 * Collect all node LBAs for a single tree via BFS.
 */
static int collect_tree_nodes(struct compact_state *state, struct tree_reloc *tr)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;

    struct btree_header hdr;
    int rc = obmafs3_btree_header_read(ctx, tr->hdr_lba, &hdr);
    if(rc != OBMAFS3_OK || hdr.root_node_lba == 0)
    {
        tr->node_count = 0;
        tr->node_lbas = NULL;
        return OBMAFS3_OK;
    }

    uint64_t cap = 256;
    uint64_t count = 0;
    tr->node_lbas = malloc(cap * sizeof(uint64_t));
    if(!tr->node_lbas) return OBMAFS3_ERR_NOMEM;

    /* BFS */
    uint64_t *queue = malloc(cap * sizeof(uint64_t));
    uint64_t qh = 0, qt = 0, qcap = cap;
    if(!queue) { free(tr->node_lbas); return OBMAFS3_ERR_NOMEM; }

    queue[qt++] = hdr.root_node_lba;

    uint8_t *buf = calloc(1, bsz);
    if(!buf) { free(queue); free(tr->node_lbas); return OBMAFS3_ERR_NOMEM; }

    while(qh < qt)
    {
        uint64_t lba = queue[qh++];
        if(lba == 0) continue;

        /* Record this node */
        if(count >= cap)
        {
            cap *= 2;
            uint64_t *tmp = realloc(tr->node_lbas, cap * sizeof(uint64_t));
            if(!tmp) { free(buf); free(queue); return OBMAFS3_ERR_NOMEM; }
            tr->node_lbas = tmp;
        }
        tr->node_lbas[count++] = lba;

        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) continue;

        struct btree_node_header nh;
        memcpy(&nh, buf, sizeof(nh));
        if(nh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

        if(nh.level > 0 && tr->idx_entry_size > 0)
        {
            for(uint16_t i = 0; i < nh.node_keys; i++)
            {
                uint64_t child;
                memcpy(&child,
                       buf + sizeof(struct btree_node_header) +
                           (size_t)i * tr->idx_entry_size + tr->child_lba_off,
                       sizeof(uint64_t));
                if(child == 0) continue;

                if(qt >= qcap)
                {
                    qcap *= 2;
                    uint64_t *tmp2 = realloc(queue, qcap * sizeof(uint64_t));
                    if(!tmp2) break;
                    queue = tmp2;
                }
                queue[qt++] = child;
            }
        }
    }

    /* Also walk the free-node chain */
    uint64_t flba = hdr.free_node_lba;
    while(flba != 0 && count < cap + 1024)
    {
        if(count >= cap)
        {
            cap *= 2;
            uint64_t *tmp = realloc(tr->node_lbas, cap * sizeof(uint64_t));
            if(!tmp) break;
            tr->node_lbas = tmp;
        }
        tr->node_lbas[count++] = flba;

        rc = obmafs3_block_read(ctx, flba, buf, bsz);
        if(rc != OBMAFS3_OK) break;

        uint64_t next;
        memcpy(&next, buf, sizeof(uint64_t));
        flba = next;
    }

    free(buf);
    free(queue);
    tr->node_count = count;
    return OBMAFS3_OK;
}

/**
 * Relocate a single B+Tree: copy all nodes to a contiguous range,
 * update internal child_lba pointers, update the tree header,
 * and update the superblock.
 */
static int relocate_tree(struct compact_state *state, struct tree_reloc *tr,
                         uint64_t dst_start)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;

    if(tr->node_count == 0) return OBMAFS3_OK;

    uint64_t *old_lbas = tr->node_lbas;
    uint64_t  n        = tr->node_count;
    int       rc;

    /* Step 1: Read ALL nodes into memory first to avoid the overlap problem.
     * If old nodes fall inside [dst_start, dst_start+n), copying them one
     * at a time would overwrite unread source nodes. */
    uint8_t **node_data = calloc((size_t)n, sizeof(uint8_t *));
    if(!node_data) return OBMAFS3_ERR_NOMEM;

    for(uint64_t i = 0; i < n; i++)
    {
        node_data[i] = malloc(bsz);
        if(!node_data[i])
        {
            for(uint64_t k = 0; k < i; k++) free(node_data[k]);
            free(node_data);
            return OBMAFS3_ERR_NOMEM;
        }

        rc = obmafs3_block_read(ctx, old_lbas[i], node_data[i], bsz);
        if(rc != OBMAFS3_OK)
        {
            for(uint64_t k = 0; k <= i; k++) free(node_data[k]);
            free(node_data);
            return rc;
        }
    }

    /* Build a hash map of old_lba → index for O(1) lookups during
     * pointer rewriting, instead of O(n) linear scans. */
    /* Use a simple open-addressing hash table. */
    uint64_t map_cap = 1;
    while(map_cap < n * 2) map_cap <<= 1;
    uint64_t map_mask = map_cap - 1;

    struct { uint64_t old_lba; uint64_t new_lba; int used; } *map =
        calloc((size_t)map_cap, sizeof(*map));
    if(!map)
    {
        for(uint64_t i = 0; i < n; i++) free(node_data[i]);
        free(node_data);
        return OBMAFS3_ERR_NOMEM;
    }

    for(uint64_t i = 0; i < n; i++)
    {
        uint64_t idx = (old_lbas[i] * 0x9E3779B97F4A7C15ULL) & map_mask;
        while(map[idx].used)
            idx = (idx + 1) & map_mask;
        map[idx].old_lba = old_lbas[i];
        map[idx].new_lba = dst_start + i;
        map[idx].used = 1;
    }

    /* Helper: lookup old_lba → new_lba, returns old_lba if not found */
    #define MAP_LOOKUP(old) ({                                          \
        uint64_t _old = (old);                                          \
        uint64_t _new = _old;                                            \
        uint64_t _idx = (_old * 0x9E3779B97F4A7C15ULL) & map_mask;      \
        while(map[_idx].used) {                                          \
            if(map[_idx].old_lba == _old) { _new = map[_idx].new_lba; break; } \
            _idx = (_idx + 1) & map_mask;                                \
        }                                                                \
        _new;                                                            \
    })

    /* Step 2: Rewrite all pointers in the in-memory node buffers */
    for(uint64_t i = 0; i < n; i++)
    {
        uint8_t *buf = node_data[i];
        struct btree_node_header nh;
        memcpy(&nh, buf, sizeof(nh));

        if(nh.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            /* This is a free-chain node — the first 8 bytes are the
             * next-free pointer, not the magic.  Map it. */
            uint64_t next_old;
            memcpy(&next_old, buf, sizeof(uint64_t));
            if(next_old != 0)
            {
                uint64_t next_new = MAP_LOOKUP(next_old);
                if(next_new != next_old)
                    memcpy(buf, &next_new, sizeof(uint64_t));
            }
            continue;
        }

        int modified = 0;

        /* Update left_link */
        if(nh.left_link != 0)
        {
            uint64_t new_link = MAP_LOOKUP(nh.left_link);
            if(new_link != nh.left_link) { nh.left_link = new_link; modified = 1; }
        }

        /* Update right_link */
        if(nh.right_link != 0)
        {
            uint64_t new_link = MAP_LOOKUP(nh.right_link);
            if(new_link != nh.right_link) { nh.right_link = new_link; modified = 1; }
        }

        /* Update child_lba pointers in index nodes */
        if(nh.level > 0 && tr->idx_entry_size > 0)
        {
            for(uint16_t k = 0; k < nh.node_keys; k++)
            {
                size_t off = sizeof(struct btree_node_header) +
                             (size_t)k * tr->idx_entry_size + tr->child_lba_off;
                uint64_t child;
                memcpy(&child, buf + off, sizeof(uint64_t));
                if(child == 0) continue;

                uint64_t new_child = MAP_LOOKUP(child);
                if(new_child != child)
                {
                    memcpy(buf + off, &new_child, sizeof(uint64_t));
                    modified = 1;
                }
            }
        }

        if(modified)
        {
            /* Write back updated header into the buffer */
            memcpy(buf, &nh, sizeof(nh));

            /* Recompute node checksum */
            struct btree_node_header *nhp = (struct btree_node_header *)buf;
            memset(nhp->checksum, 0, sizeof(nhp->checksum));
            { size_t cs_len = sizeof(struct btree_node_header) + nhp->keys_length; obmafs3_checksum_block(buf, cs_len, nhp->checksum); }
        }
    }

    /* Step 3: Write all nodes to their new positions */
    for(uint64_t i = 0; i < n; i++)
    {
        uint64_t dst = dst_start + i;

        atomic_store(&state->current_src_lba, old_lbas[i]);
        atomic_store(&state->current_dst_lba, dst);

        rc = obmafs3_block_write(ctx, dst, node_data[i], bsz);
        if(rc != OBMAFS3_OK)
        {
            for(uint64_t k = 0; k < n; k++) free(node_data[k]);
            free(node_data);
            free(map);
            return rc;
        }

        state->tree_nodes_moved++;
        atomic_fetch_add(&state->done_steps, 1);

        free(node_data[i]);
        node_data[i] = NULL;
    }
    free(node_data);

    /* Step 4: Update tree header */
    struct btree_header thdr;
    rc = obmafs3_btree_header_read(ctx, tr->hdr_lba, &thdr);
    if(rc != OBMAFS3_OK) { free(map); return rc; }

    if(thdr.root_node_lba != 0)
        thdr.root_node_lba = MAP_LOOKUP(thdr.root_node_lba);
    if(thdr.free_node_lba != 0)
        thdr.free_node_lba = MAP_LOOKUP(thdr.free_node_lba);

    /* Record all node relocations in state->tree_reloc_old/new
     * so update_sme_refs can fix dedup_subchannel_lba in CD SMEs. */
    for(uint64_t i = 0; i < n; i++)
    {
        if(old_lbas[i] == dst_start + i) continue; /* not moved */

        if(state->tree_reloc_count >= state->tree_reloc_cap)
        {
            uint64_t nc = (state->tree_reloc_cap == 0) ? 4096 : state->tree_reloc_cap * 2;
            uint64_t *ro = realloc(state->tree_reloc_old, nc * sizeof(uint64_t));
            uint64_t *rn = realloc(state->tree_reloc_new, nc * sizeof(uint64_t));
            if(ro && rn)
            {
                state->tree_reloc_old = ro;
                state->tree_reloc_new = rn;
                state->tree_reloc_cap = nc;
            }
        }
        if(state->tree_reloc_count < state->tree_reloc_cap)
        {
            state->tree_reloc_old[state->tree_reloc_count] = old_lbas[i];
            state->tree_reloc_new[state->tree_reloc_count] = dst_start + i;
            state->tree_reloc_count++;
        }
    }

    #undef MAP_LOOKUP
    free(map);

    /* Write updated header (auto-computes checksum) */
    rc = obmafs3_btree_header_write(ctx, tr->hdr_lba, &thdr);
    if(rc != OBMAFS3_OK) return rc;

    sync_fd(state);

    /* Step 5: Update bitmap: set new range, clear old individual LBAs */
    obmafs3_bitmap_set(ctx, dst_start, n);
    for(uint64_t i = 0; i < n; i++)
    {
        uint64_t old = old_lbas[i];
        if(old < dst_start || old >= dst_start + n)
            obmafs3_bitmap_clear(ctx, old, 1);

        if(state->analysis->block_types)
        {
            if(old < dst_start || old >= dst_start + n)
                state->analysis->block_types[old] = BT_FREE;
            state->analysis->block_types[dst_start + i] = BT_TREE;
        }
    }

    sync_fd(state);

    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Dedup reference update after block relocation                      */
/* ------------------------------------------------------------------ */

/**
 * Build a hash map from the relocation arrays for O(1) lookup.
 */
struct reloc_map
{
    uint64_t *old_lbas;
    uint64_t *new_lbas;
    int      *used;
    uint64_t  capacity;
    uint64_t  mask;
};

static struct reloc_map *reloc_map_create(const uint64_t *old_arr,
                                          const uint64_t *new_arr,
                                          uint64_t count)
{
    struct reloc_map *m = calloc(1, sizeof(*m));
    if(!m) return NULL;

    uint64_t cap = 64;
    while(cap < count * 2) cap <<= 1;

    m->old_lbas = calloc((size_t)cap, sizeof(uint64_t));
    m->new_lbas = calloc((size_t)cap, sizeof(uint64_t));
    m->used     = calloc((size_t)cap, sizeof(int));
    if(!m->old_lbas || !m->new_lbas || !m->used)
    {
        free(m->old_lbas); free(m->new_lbas); free(m->used); free(m);
        return NULL;
    }
    m->capacity = cap;
    m->mask = cap - 1;

    for(uint64_t i = 0; i < count; i++)
    {
        uint64_t idx = (old_arr[i] * 0x9E3779B97F4A7C15ULL) & m->mask;
        while(m->used[idx])
            idx = (idx + 1) & m->mask;
        m->old_lbas[idx] = old_arr[i];
        m->new_lbas[idx] = new_arr[i];
        m->used[idx] = 1;
    }
    return m;
}

/** Lookup old_lba in the relocation map. Returns new_lba or 0 if not found. */
static uint64_t reloc_map_get(const struct reloc_map *m, uint64_t old_lba)
{
    if(!m || old_lba == 0) return 0;
    uint64_t idx = (old_lba * 0x9E3779B97F4A7C15ULL) & m->mask;
    while(m->used[idx])
    {
        if(m->old_lbas[idx] == old_lba)
            return m->new_lbas[idx];
        idx = (idx + 1) & m->mask;
    }
    return 0; /* not relocated */
}

static void reloc_map_destroy(struct reloc_map *m)
{
    if(!m) return;
    free(m->old_lbas);
    free(m->new_lbas);
    free(m->used);
    free(m);
}

/**
 * Walk all dedup tree leaves and update dedup_entry.block_lba for
 * any entries whose block_lba was relocated.
 */
static int update_dedup_tree_refs(struct compact_state *state,
                                  const struct reloc_map *rmap)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;

    if(ctx->sb.dedup_lba == 0) return OBMAFS3_OK;

    uint8_t *tlbuf = calloc(1, bsz);
    if(!tlbuf) return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, tlbuf, bsz);
    if(rc != OBMAFS3_OK) { free(tlbuf); return rc; }

    struct tree_list_header *tlh = (struct tree_list_header *)tlbuf;
    if(tlh->magic != OBMAFS3_TREELIST_MAGIC) { free(tlbuf); return OBMAFS3_OK; }

    struct tree_list_entry *entries =
        (struct tree_list_entry *)(tlbuf + sizeof(struct tree_list_header));

    uint8_t *node_buf = calloc(1, bsz);
    if(!node_buf) { free(tlbuf); return OBMAFS3_ERR_NOMEM; }

    for(uint64_t t = 0; t < tlh->tree_count && t < 64; t++)
    {
        if(entries[t].tree_lba == 0) continue;

        struct btree_header dhdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &dhdr);
        if(rc != OBMAFS3_OK) continue;
        if(dhdr.root_node_lba == 0) continue;

        /* Also update last_block_lba in the header */
        if(dhdr.last_block_lba != 0)
        {
            uint64_t new_last = reloc_map_get(rmap, dhdr.last_block_lba);
            if(new_last != 0)
            {
                dhdr.last_block_lba = new_last;
                obmafs3_btree_header_write(ctx, entries[t].tree_lba, &dhdr);
            }
        }

        /* BFS to find leaf nodes */
        uint64_t *queue = malloc(256 * sizeof(uint64_t));
        uint64_t  qh = 0, qt = 0, qcap = 256;
        if(!queue) continue;

        queue[qt++] = dhdr.root_node_lba;

        while(qh < qt)
        {
            uint64_t lba = queue[qh++];
            if(lba == 0) continue;

            rc = obmafs3_block_read(ctx, lba, node_buf, bsz);
            if(rc != OBMAFS3_OK) continue;

            struct btree_node_header nh;
            memcpy(&nh, node_buf, sizeof(nh));
            if(nh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

            if(nh.level > 0)
            {
                const uint8_t *records = node_buf + sizeof(struct btree_node_header);
                for(uint16_t i = 0; i < nh.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, records + (size_t)i * sizeof(ie), sizeof(ie));
                    if(ie.child_lba == 0) continue;
                    if(qt >= qcap)
                    {
                        qcap *= 2;
                        uint64_t *tmp = realloc(queue, qcap * sizeof(uint64_t));
                        if(!tmp) break;
                        queue = tmp;
                    }
                    queue[qt++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf node — update dedup_entry.block_lba */
            int modified = 0;
            uint8_t *records = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nh.node_keys; i++)
            {
                struct dedup_entry *de = (struct dedup_entry *)
                    (records + (size_t)i * sizeof(struct dedup_entry));

                if(de->block_lba == 0) continue;

                uint64_t new_lba = reloc_map_get(rmap, de->block_lba);
                if(new_lba != 0 && new_lba != de->block_lba)
                {
                    de->block_lba = new_lba;
                    modified = 1;
                }
            }

            if(modified)
            {
                /* Recompute node checksum */
                struct btree_node_header *nhp = (struct btree_node_header *)node_buf;
                memset(nhp->checksum, 0, sizeof(nhp->checksum));
                { size_t cs_len = sizeof(struct btree_node_header) + nhp->keys_length; obmafs3_checksum_block(node_buf, cs_len, nhp->checksum); };

                obmafs3_block_write(ctx, lba, node_buf, bsz);
            }
        }

        free(queue);
    }

    free(node_buf);
    free(tlbuf);
    return OBMAFS3_OK;
}

/**
 * Walk all inodes and update extent_run.start_block for any data
 * blocks that were relocated.  Also updates overflow extents.
 */
static int update_inode_extent_refs(struct compact_state *state,
                                    const struct reloc_map *dmap)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;

    if(ctx->sb.inode_lba == 0) return OBMAFS3_OK;

    struct btree_header ihdr;
    int rc = obmafs3_btree_header_read(ctx, ctx->sb.inode_lba, &ihdr);
    if(rc != OBMAFS3_OK) return rc;
    if(ihdr.root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *node_buf = calloc(1, bsz);
    if(!node_buf) return OBMAFS3_ERR_NOMEM;

    /* BFS the inode tree */
    uint64_t *queue = malloc(256 * sizeof(uint64_t));
    uint64_t  qh = 0, qt = 0, qcap = 256;
    if(!queue) { free(node_buf); return OBMAFS3_ERR_NOMEM; }

    queue[qt++] = ihdr.root_node_lba;

    while(qh < qt)
    {
        if(CANCELLED(state)) break;

        uint64_t lba = queue[qh++];
        if(lba == 0) continue;

        rc = obmafs3_block_read(ctx, lba, node_buf, bsz);
        if(rc != OBMAFS3_OK) continue;

        struct btree_node_header nh;
        memcpy(&nh, node_buf, sizeof(nh));
        if(nh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

        if(nh.level > 0)
        {
            const uint8_t *records = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nh.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, records + (size_t)i * sizeof(ie), sizeof(ie));
                if(ie.child_lba == 0) continue;
                if(qt >= qcap) { qcap *= 2; uint64_t *t = realloc(queue, qcap * sizeof(uint64_t)); if(!t) break; queue = t; }
                queue[qt++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf — update inode_record inline extents */
        int node_modified = 0;
        uint8_t *records = node_buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < nh.node_keys; i++)
        {
            struct inode_record *irec = (struct inode_record *)
                (records + (size_t)i * sizeof(struct inode_record));

            for(uint8_t e = 0; e < 8; e++)
            {
                if(irec->extents[e].start_block == 0) continue;

                /* Check each block in the extent individually since
                 * compaction can split or reorder blocks */
                uint64_t new_start = reloc_map_get(dmap, irec->extents[e].start_block);
                if(new_start != 0 && new_start != irec->extents[e].start_block)
                {
                    irec->extents[e].start_block = new_start;
                    node_modified = 1;
                }
            }
        }

        if(node_modified)
        {
            struct btree_node_header *nhp = (struct btree_node_header *)node_buf;
            memset(nhp->checksum, 0, sizeof(nhp->checksum));
            { size_t cs_len = sizeof(struct btree_node_header) + nhp->keys_length; obmafs3_checksum_block(node_buf, cs_len, nhp->checksum); };
            obmafs3_block_write(ctx, lba, node_buf, bsz);
        }
    }

    free(queue);
    free(node_buf);

    /* Also update overflow extents */
    if(ctx->sb.overflow_lba != 0)
    {
        struct btree_header ohdr;
        rc = obmafs3_btree_header_read(ctx, ctx->sb.overflow_lba, &ohdr);
        if(rc == OBMAFS3_OK && ohdr.root_node_lba != 0)
        {
            node_buf = calloc(1, bsz);
            queue = malloc(256 * sizeof(uint64_t));
            qh = 0; qt = 0; qcap = 256;
            if(node_buf && queue)
            {
                queue[qt++] = ohdr.root_node_lba;
                while(qh < qt)
                {
                    if(CANCELLED(state)) break;
                    uint64_t olba = queue[qh++];
                    if(olba == 0) continue;

                    rc = obmafs3_block_read(ctx, olba, node_buf, bsz);
                    if(rc != OBMAFS3_OK) continue;

                    struct btree_node_header onh;
                    memcpy(&onh, node_buf, sizeof(onh));
                    if(onh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

                    if(onh.level > 0)
                    {
                        const uint8_t *recs = node_buf + sizeof(struct btree_node_header);
                        for(uint16_t i = 0; i < onh.node_keys; i++)
                        {
                            struct overflow_index_entry oie;
                            memcpy(&oie, recs + (size_t)i * sizeof(oie), sizeof(oie));
                            if(oie.child_lba == 0) continue;
                            if(qt >= qcap) { qcap *= 2; uint64_t *t = realloc(queue, qcap * sizeof(uint64_t)); if(!t) break; queue = t; }
                            queue[qt++] = oie.child_lba;
                        }
                        continue;
                    }

                    /* Leaf — update overflow_extent.start_block */
                    int omod = 0;
                    uint8_t *recs = node_buf + sizeof(struct btree_node_header);
                    for(uint16_t i = 0; i < onh.node_keys; i++)
                    {
                        struct overflow_extent *oe = (struct overflow_extent *)
                            (recs + (size_t)i * sizeof(struct overflow_extent));
                        if(oe->start_block == 0) continue;

                        uint64_t new_sb = reloc_map_get(dmap, oe->start_block);
                        if(new_sb != 0 && new_sb != oe->start_block)
                        {
                            oe->start_block = new_sb;
                            omod = 1;
                        }
                    }
                    if(omod)
                    {
                        struct btree_node_header *onhp = (struct btree_node_header *)node_buf;
                        memset(onhp->checksum, 0, sizeof(onhp->checksum));
                        { size_t cs_len = sizeof(struct btree_node_header) + onhp->keys_length; obmafs3_checksum_block(node_buf, cs_len, onhp->checksum); };
                        obmafs3_block_write(ctx, olba, node_buf, bsz);
                    }
                }
            }
            free(node_buf);
            free(queue);
        }
    }

    return OBMAFS3_OK;
}

/**
 * Read the entire logical file data for an inode into a contiguous
 * buffer by reading all extents (inline + overflow), decompressing
 * compressed extents, and concatenating the results.
 *
 * @param data_out  On success, *data_out is set to a malloc'd buffer
 *                  containing the decompressed file data.
 * @param data_len  On success, total bytes in *data_out.
 * @return 0 on success, negative on error.
 */
static int read_inode_data(struct compact_state *state,
                           const struct inode_record *irec,
                           uint8_t **data_out, uint64_t *data_len_out)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;
    int rc;

    /* Collect ALL extent runs: 8 inline + overflow */
    struct extent_run *all_extents = NULL;
    uint64_t ext_count = 0, ext_cap = 0;

    /* Inline extents */
    for(uint8_t e = 0; e < 8; e++)
    {
        if(irec->extents[e].start_block == 0) break;
        if(ext_count >= ext_cap)
        {
            ext_cap = (ext_cap == 0) ? 64 : ext_cap * 2;
            struct extent_run *t = realloc(all_extents, ext_cap * sizeof(*t));
            if(!t) { free(all_extents); return OBMAFS3_ERR_NOMEM; }
            all_extents = t;
        }
        all_extents[ext_count++] = irec->extents[e];
    }

    /* Overflow extents */
    if(ctx->sb.overflow_lba != 0)
    {
        struct btree_header ohdr;
        rc = obmafs3_btree_header_read(ctx, ctx->sb.overflow_lba, &ohdr);
        if(rc == OBMAFS3_OK && ohdr.root_node_lba != 0)
        {
            uint8_t *onode = calloc(1, bsz);
            uint64_t *oq = malloc(256 * sizeof(uint64_t));
            uint64_t oqh = 0, oqt = 0, oqcap = 256;
            if(onode && oq)
            {
                oq[oqt++] = ohdr.root_node_lba;
                while(oqh < oqt)
                {
                    uint64_t olba = oq[oqh++];
                    if(olba == 0) continue;
                    rc = obmafs3_block_read(ctx, olba, onode, bsz);
                    if(rc != OBMAFS3_OK) continue;
                    struct btree_node_header onh;
                    memcpy(&onh, onode, sizeof(onh));
                    if(onh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;
                    if(onh.level > 0)
                    {
                        const uint8_t *orecs = onode + sizeof(struct btree_node_header);
                        for(uint16_t oi = 0; oi < onh.node_keys; oi++)
                        {
                            struct overflow_index_entry oie;
                            memcpy(&oie, orecs + (size_t)oi * sizeof(oie), sizeof(oie));
                            if(oie.child_lba == 0) continue;
                            if(oqt >= oqcap) { oqcap *= 2; uint64_t *t = realloc(oq, oqcap * sizeof(uint64_t)); if(!t) break; oq = t; }
                            oq[oqt++] = oie.child_lba;
                        }
                        continue;
                    }
                    const uint8_t *orecs = onode + sizeof(struct btree_node_header);
                    for(uint16_t oi = 0; oi < onh.node_keys; oi++)
                    {
                        struct overflow_extent oe;
                        memcpy(&oe, orecs + (size_t)oi * sizeof(oe), sizeof(oe));
                        if(oe.inode_id != irec->inode_id) continue;
                        if(oe.start_block == 0) continue;
                        if(ext_count >= ext_cap)
                        {
                            ext_cap = (ext_cap == 0) ? 64 : ext_cap * 2;
                            struct extent_run *t = realloc(all_extents, ext_cap * sizeof(*t));
                            if(!t) break;
                            all_extents = t;
                        }
                        all_extents[ext_count].start_block   = oe.start_block;
                        all_extents[ext_count].block_count   = oe.block_count;
                        all_extents[ext_count].logical_blocks = oe.logical_count;
                        ext_count++;
                    }
                }
            }
            free(onode);
            free(oq);
        }
    }

    /* Compute total logical size */
    uint64_t total_logical = 0;
    for(uint64_t i = 0; i < ext_count; i++)
        total_logical += all_extents[i].logical_blocks * bsz;

    if(total_logical == 0 || total_logical > irec->file_size + bsz * 16)
    {
        free(all_extents);
        return OBMAFS3_ERR_IO;
    }

    uint8_t *data = malloc((size_t)total_logical);
    if(!data) { free(all_extents); return OBMAFS3_ERR_NOMEM; }

    /* Read each extent into the contiguous buffer */
    uint64_t offset = 0;
    for(uint64_t i = 0; i < ext_count; i++)
    {
        struct extent_run *ext = &all_extents[i];
        size_t logical_bytes = (size_t)(ext->logical_blocks * bsz);

        if(ext->logical_blocks == ext->block_count)
        {
            /* Uncompressed: read raw blocks */
            for(uint64_t b = 0; b < ext->block_count; b++)
            {
                rc = obmafs3_block_read(ctx, ext->start_block + b,
                                        data + offset + b * bsz, bsz);
                if(rc != OBMAFS3_OK) { free(data); free(all_extents); return rc; }
            }
        }
        else
        {
            /* Compressed: read phys blocks, decompress */
            size_t phys_bytes = (size_t)ext->block_count * bsz;
            uint8_t *phys = malloc(phys_bytes);
            if(!phys) { free(data); free(all_extents); return OBMAFS3_ERR_NOMEM; }

            for(uint64_t b = 0; b < ext->block_count; b++)
            {
                rc = obmafs3_block_read(ctx, ext->start_block + b,
                                        phys + b * bsz, bsz);
                if(rc != OBMAFS3_OK) { free(phys); free(data); free(all_extents); return rc; }
            }

            struct block_header bhdr;
            memcpy(&bhdr, phys, sizeof(bhdr));
            if(bhdr.magic != OBMAFS3_BLOCK_MAGIC)
            { free(phys); free(data); free(all_extents); return OBMAFS3_ERR_IO; }

            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                size_t r = ZSTD_decompress(data + offset, logical_bytes,
                                           phys + sizeof(bhdr),
                                           (size_t)bhdr.compressed_size);
                if(ZSTD_isError(r))
                { free(phys); free(data); free(all_extents); return OBMAFS3_ERR_IO; }
            }
            else
            {
                size_t copy_len = (size_t)bhdr.original_size;
                if(copy_len > logical_bytes) copy_len = logical_bytes;
                memcpy(data + offset, phys + sizeof(bhdr), copy_len);
            }
            free(phys);
        }
        offset += logical_bytes;
    }

    free(all_extents);
    *data_out = data;
    *data_len_out = total_logical;
    return OBMAFS3_OK;
}

/**
 * Write back the logical file data for an inode by compressing and
 * writing each extent.
 */
static int write_inode_data(struct compact_state *state,
                            const struct inode_record *irec,
                            const uint8_t *data, uint64_t data_len)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;
    int rc;

    /* Collect ALL extent runs: 8 inline + overflow */
    struct extent_run *all_extents = NULL;
    uint64_t ext_count = 0, ext_cap = 0;

    for(uint8_t e = 0; e < 8; e++)
    {
        if(irec->extents[e].start_block == 0) break;
        if(ext_count >= ext_cap)
        {
            ext_cap = (ext_cap == 0) ? 64 : ext_cap * 2;
            struct extent_run *t = realloc(all_extents, ext_cap * sizeof(*t));
            if(!t) { free(all_extents); return OBMAFS3_ERR_NOMEM; }
            all_extents = t;
        }
        all_extents[ext_count++] = irec->extents[e];
    }

    if(ctx->sb.overflow_lba != 0)
    {
        struct btree_header ohdr;
        rc = obmafs3_btree_header_read(ctx, ctx->sb.overflow_lba, &ohdr);
        if(rc == OBMAFS3_OK && ohdr.root_node_lba != 0)
        {
            uint8_t *onode = calloc(1, bsz);
            uint64_t *oq = malloc(256 * sizeof(uint64_t));
            uint64_t oqh = 0, oqt = 0, oqcap = 256;
            if(onode && oq)
            {
                oq[oqt++] = ohdr.root_node_lba;
                while(oqh < oqt)
                {
                    uint64_t olba = oq[oqh++];
                    if(olba == 0) continue;
                    rc = obmafs3_block_read(ctx, olba, onode, bsz);
                    if(rc != OBMAFS3_OK) continue;
                    struct btree_node_header onh;
                    memcpy(&onh, onode, sizeof(onh));
                    if(onh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;
                    if(onh.level > 0)
                    {
                        const uint8_t *orecs = onode + sizeof(struct btree_node_header);
                        for(uint16_t oi = 0; oi < onh.node_keys; oi++)
                        {
                            struct overflow_index_entry oie;
                            memcpy(&oie, orecs + (size_t)oi * sizeof(oie), sizeof(oie));
                            if(oie.child_lba == 0) continue;
                            if(oqt >= oqcap) { oqcap *= 2; uint64_t *t = realloc(oq, oqcap * sizeof(uint64_t)); if(!t) break; oq = t; }
                            oq[oqt++] = oie.child_lba;
                        }
                        continue;
                    }
                    const uint8_t *orecs = onode + sizeof(struct btree_node_header);
                    for(uint16_t oi = 0; oi < onh.node_keys; oi++)
                    {
                        struct overflow_extent oe;
                        memcpy(&oe, orecs + (size_t)oi * sizeof(oe), sizeof(oe));
                        if(oe.inode_id != irec->inode_id) continue;
                        if(oe.start_block == 0) continue;
                        if(ext_count >= ext_cap)
                        {
                            ext_cap = (ext_cap == 0) ? 64 : ext_cap * 2;
                            struct extent_run *t = realloc(all_extents, ext_cap * sizeof(*t));
                            if(!t) break;
                            all_extents = t;
                        }
                        all_extents[ext_count].start_block   = oe.start_block;
                        all_extents[ext_count].block_count   = oe.block_count;
                        all_extents[ext_count].logical_blocks = oe.logical_count;
                        ext_count++;
                    }
                }
            }
            free(onode);
            free(oq);
        }
    }

    /* Write each extent back */
    uint64_t offset = 0;
    for(uint64_t i = 0; i < ext_count; i++)
    {
        struct extent_run *ext = &all_extents[i];
        size_t logical_bytes = (size_t)(ext->logical_blocks * bsz);

        if(ext->logical_blocks == ext->block_count)
        {
            /* Uncompressed: write raw blocks */
            for(uint64_t b = 0; b < ext->block_count && offset + b * bsz < data_len; b++)
                obmafs3_block_write(ctx, ext->start_block + b,
                                    data + offset + b * bsz, bsz);
        }
        else
        {
            /* Compressed: recompress and write */
            size_t phys_bytes = (size_t)ext->block_count * bsz;
            size_t src_len    = logical_bytes;
            if(offset + src_len > data_len) src_len = (size_t)(data_len - offset);

            size_t comp_bound = ZSTD_compressBound(src_len);
            uint8_t *comp_buf = malloc(comp_bound);
            uint8_t *phys_buf = calloc(1, phys_bytes);

            if(comp_buf && phys_buf)
            {
                size_t comp_size = ZSTD_compress(comp_buf, comp_bound,
                                                 data + offset, src_len,
                                                 ctx->zstd_level);
                size_t max_payload = phys_bytes - sizeof(struct block_header);

                if(!ZSTD_isError(comp_size) && comp_size <= max_payload)
                {
                    struct block_header bh;
                    memset(&bh, 0, sizeof(bh));
                    bh.magic = OBMAFS3_BLOCK_MAGIC;
                    bh.flags = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                    bh.original_size = src_len;
                    bh.compressed_size = comp_size;

                    memcpy(phys_buf, &bh, sizeof(bh));
                    memcpy(phys_buf + sizeof(bh), comp_buf, comp_size);

                    /* Checksum over payload only */
                    struct block_header *hp = (struct block_header *)phys_buf;
                    memset(hp->checksum, 0, sizeof(hp->checksum));
                    obmafs3_checksum_block(phys_buf + sizeof(bh), comp_size,
                                           hp->checksum);

                    for(uint64_t b = 0; b < ext->block_count; b++)
                        obmafs3_block_write(ctx, ext->start_block + b,
                                            phys_buf + b * bsz, bsz);
                }
                else if(src_len <= max_payload)
                {
                    struct block_header bh;
                    memset(&bh, 0, sizeof(bh));
                    bh.magic = OBMAFS3_BLOCK_MAGIC;
                    bh.flags = 0;
                    bh.original_size = src_len;
                    bh.compressed_size = src_len;

                    memcpy(phys_buf, &bh, sizeof(bh));
                    memcpy(phys_buf + sizeof(bh), data + offset, src_len);

                    struct block_header *hp = (struct block_header *)phys_buf;
                    memset(hp->checksum, 0, sizeof(hp->checksum));
                    obmafs3_checksum_block(phys_buf + sizeof(bh), src_len,
                                           hp->checksum);

                    for(uint64_t b = 0; b < ext->block_count; b++)
                        obmafs3_block_write(ctx, ext->start_block + b,
                                            phys_buf + b * bsz, bsz);
                }
            }
            free(comp_buf);
            free(phys_buf);
        }
        offset += logical_bytes;
    }

    free(all_extents);
    return OBMAFS3_OK;
}

/**
 * Walk all inode data and update sector_map_entry.dedup_sector_lba
 * and cd_sector_map_entry.dedup_sector_lba for relocated dedup blocks.
 *
 * Loads the ENTIRE sector map for each media-image inode as one
 * contiguous buffer, updates all SME entries at once, then writes
 * the whole thing back.  No per-extent or per-block alignment issues.
 */
static int update_sme_refs(struct compact_state *state,
                           const struct reloc_map *rmap,
                           const struct reloc_map *tree_rmap)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;

    if(ctx->sb.inode_lba == 0) return OBMAFS3_OK;

    struct btree_header ihdr;
    int rc = obmafs3_btree_header_read(ctx, ctx->sb.inode_lba, &ihdr);
    if(rc != OBMAFS3_OK) return rc;
    if(ihdr.root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *node_buf = calloc(1, bsz);
    if(!node_buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *queue = malloc(256 * sizeof(uint64_t));
    uint64_t  qh = 0, qt = 0, qcap = 256;
    if(!queue) { free(node_buf); return OBMAFS3_ERR_NOMEM; }

    queue[qt++] = ihdr.root_node_lba;

    while(qh < qt)
    {
        if(CANCELLED(state)) break;

        uint64_t lba = queue[qh++];
        if(lba == 0) continue;

        rc = obmafs3_block_read(ctx, lba, node_buf, bsz);
        if(rc != OBMAFS3_OK) continue;

        struct btree_node_header nh;
        memcpy(&nh, node_buf, sizeof(nh));
        if(nh.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

        if(nh.level > 0)
        {
            const uint8_t *records = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nh.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, records + (size_t)i * sizeof(ie), sizeof(ie));
                if(ie.child_lba == 0) continue;
                if(qt >= qcap) { qcap *= 2; uint64_t *t = realloc(queue, qcap * sizeof(uint64_t)); if(!t) break; queue = t; }
                queue[qt++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf — each record is an inode_record. */
        const uint8_t *records = node_buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < nh.node_keys; i++)
        {
            struct inode_record irec;
            memcpy(&irec, records + (size_t)i * sizeof(struct inode_record), sizeof(irec));

            if(irec.file_size == 0) continue;
            if(irec.extents[0].start_block == 0) continue;

            /* Load the entire inode's logical data */
            uint8_t *file_data = NULL;
            uint64_t file_len = 0;
            rc = read_inode_data(state, &irec, &file_data, &file_len);
            if(rc != OBMAFS3_OK || !file_data) continue;

            /* Check sector map magic */
            if(file_len < sizeof(struct sector_map_header))
            { free(file_data); continue; }

            struct sector_map_header smh;
            memcpy(&smh, file_data, sizeof(smh));
            if(smh.magic != OBMAFS3_SECTOR_MAP_MAGIC)
            { free(file_data); continue; }

            /* Update all SME entries in one pass */
            int modified = 0;
            uint8_t *p = file_data + sizeof(struct sector_map_header);
            size_t remaining = (size_t)(file_len - sizeof(struct sector_map_header));

            if(smh.type == 0)
            {
                size_t esz = sizeof(struct sector_map_entry);
                while(remaining >= esz)
                {
                    struct sector_map_entry *sme = (struct sector_map_entry *)p;
                    if(sme->dedup_sector_lba != 0)
                    {
                        uint64_t n = reloc_map_get(rmap, sme->dedup_sector_lba);
                        if(n != 0 && n != sme->dedup_sector_lba)
                        { sme->dedup_sector_lba = n; modified = 1; }
                    }
                    p += esz; remaining -= esz;
                }
            }
            else
            {
                size_t esz = sizeof(struct cd_sector_map_entry);
                while(remaining >= esz)
                {
                    struct cd_sector_map_entry *cdsme = (struct cd_sector_map_entry *)p;
                    if(cdsme->dedup_sector_lba != 0)
                    {
                        uint64_t n = reloc_map_get(rmap, cdsme->dedup_sector_lba);
                        if(n != 0 && n != cdsme->dedup_sector_lba)
                        { cdsme->dedup_sector_lba = n; modified = 1; }
                    }
                    if(tree_rmap && cdsme->dedup_subchannel_lba != 0)
                    {
                        uint64_t n = reloc_map_get(tree_rmap, cdsme->dedup_subchannel_lba);
                        if(n != 0 && n != cdsme->dedup_subchannel_lba)
                        { cdsme->dedup_subchannel_lba = n; modified = 1; }
                    }
                    p += esz; remaining -= esz;
                }
            }

            /* Write back if modified */
            if(modified)
                write_inode_data(state, &irec, file_data, file_len);

            free(file_data);
        }
    }

    free(queue);
    free(node_buf);
    return OBMAFS3_OK;
}

static int compact_trees(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;

    /* Collect all trees to relocate */
    struct tree_reloc trees[20];
    int tc = 0;

    #define ADD_TREE(name_str, lba_val, ie_type, clba_field)          \
        do {                                                          \
            if((lba_val) != 0 && tc < 20)                             \
            {                                                         \
                trees[tc].name = (name_str);                          \
                trees[tc].hdr_lba = (lba_val);                        \
                trees[tc].idx_entry_size = sizeof(ie_type);            \
                trees[tc].child_lba_off = offsetof(ie_type, child_lba);\
                trees[tc].node_lbas = NULL;                           \
                trees[tc].node_count = 0;                             \
                tc++;                                                 \
            }                                                         \
        } while(0)

    ADD_TREE("Catalog",      ctx->sb.catalog_lba,       struct catalog_index_entry,      child_lba);
    ADD_TREE("Inode",        ctx->sb.inode_lba,         struct btree_index_entry,        child_lba);
    ADD_TREE("Overflow",     ctx->sb.overflow_lba,      struct overflow_index_entry,     child_lba);
    ADD_TREE("Metadata",     ctx->sb.metadata_lba,      struct metadata_index_entry,     child_lba);
    ADD_TREE("MetadataIdx",  ctx->sb.metadata_idx_lba,  struct metadata_idx_index_entry, child_lba);
    ADD_TREE("MediaTag",     ctx->sb.media_tag_lba,     struct media_tag_index_entry,    child_lba);
    ADD_TREE("CdPrefix",     ctx->sb.cd_prefix_lba,     struct btree_index_entry,        child_lba);
    ADD_TREE("CdSuffix",     ctx->sb.cd_suffix_lba,     struct btree_index_entry,        child_lba);
    ADD_TREE("CdSubchannel", ctx->sb.cd_subchannel_lba, struct btree_index_entry,        child_lba);
    ADD_TREE("Refcount",     ctx->sb.refcount_lba,      struct btree_index_entry,        child_lba);

    /* Add dedup trees from the tree list */
    if(ctx->sb.dedup_lba != 0)
    {
        size_t bsz = (size_t)ctx->sb.block_size;
        uint8_t *tlbuf = calloc(1, bsz);
        if(tlbuf)
        {
            int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, tlbuf, bsz);
            if(rc == OBMAFS3_OK)
            {
                struct tree_list_header *tlh = (struct tree_list_header *)tlbuf;
                if(tlh->magic == OBMAFS3_TREELIST_MAGIC)
                {
                    struct tree_list_entry *entries =
                        (struct tree_list_entry *)(tlbuf + sizeof(struct tree_list_header));
                    for(uint64_t t = 0; t < tlh->tree_count && t < 64 && tc < 20; t++)
                    {
                        if(entries[t].tree_lba == 0) continue;
                        char *name = malloc(32);
                        if(name) snprintf(name, 32, "Dedup %u", (unsigned)entries[t].sector_size);
                        trees[tc].name = name ? name : "Dedup";
                        trees[tc].hdr_lba = entries[t].tree_lba;
                        trees[tc].idx_entry_size = sizeof(struct btree_index_entry);
                        trees[tc].child_lba_off = offsetof(struct btree_index_entry, child_lba);
                        trees[tc].node_lbas = NULL;
                        trees[tc].node_count = 0;
                        tc++;
                    }
                }
            }
            free(tlbuf);
        }
    }

    #undef ADD_TREE

    /* Collect nodes for all trees */
    uint64_t total_nodes = 0;
    for(int i = 0; i < tc; i++)
    {
        int rc = collect_tree_nodes(state, &trees[i]);
        if(rc != OBMAFS3_OK) goto cleanup;
        total_nodes += trees[i].node_count;
    }

    /* Calculate total space needed: nodes + clump gaps between trees */
    uint64_t total_needed = total_nodes + (uint64_t)(tc > 0 ? tc - 1 : 0) * COMPACT_TREE_CLUMP;

    /* Place trees at the end of the disk (before the backup superblock) */
    uint64_t trees_start = total_blocks - 1 - total_needed;

    /* Relocate each tree */
    uint64_t cursor = trees_start;
    for(int i = 0; i < tc; i++)
    {
        if(trees[i].node_count == 0) continue;

        int rc = relocate_tree(state, &trees[i], cursor);
        if(rc != OBMAFS3_OK) goto cleanup;

        cursor += trees[i].node_count + COMPACT_TREE_CLUMP;
    }

    /* Flush bitmap after all trees are relocated */
    obmafs3_bitmap_write(ctx);
    sync_fd(state);

cleanup:
    for(int i = 0; i < tc; i++)
        free(trees[i].node_lbas);

    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Main compaction entry point                                        */
/* ------------------------------------------------------------------ */

int defrag_compact_run(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
    uint8_t *bt = state->analysis->block_types;

    clock_gettime(CLOCK_MONOTONIC, &state->start_time);

    /* Count total steps for progress */
    uint64_t total_steps = 0;
    for(uint64_t i = 0; i < total_blocks; i++)
    {
        if(bt[i] == BT_USED || bt[i] == BT_DEDUP || bt[i] == BT_TREE)
            total_steps++;
    }
    atomic_store(&state->total_steps, total_steps);
    atomic_store(&state->done_steps, 0);

    /* ---- Phase 0: Prepare ---- */
    atomic_store(&state->phase, COMPACT_PHASE_PREPARE);

    /* Ensure bitmap is loaded */
    int rc = obmafs3_bitmap_read(ctx);
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        return rc;
    }

    /* Initialize tree relocation map for subchannel LBA fixup */
    state->tree_reloc_old   = NULL;
    state->tree_reloc_new   = NULL;
    state->tree_reloc_count = 0;
    state->tree_reloc_cap   = 0;

    /* ---- Phase 1: Relocate trees to end of disk first ----
     * This must happen BEFORE data/dedup compaction so that tree nodes
     * are out of the way.  relocate_tree() correctly updates all
     * internal child_lba, sibling, and free-chain pointers. */
    fprintf(stderr, "[compact] === PHASE 1: TREES ===\n");
    struct timespec ts_trees_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_trees_start);
    atomic_store(&state->phase, COMPACT_PHASE_TREES);
    rc = compact_trees(state);
    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        double secs = (double)(ts_now.tv_sec - ts_trees_start.tv_sec) +
                      (double)(ts_now.tv_nsec - ts_trees_start.tv_nsec) / 1e9;
        fprintf(stderr, "[compact] TREES done: %.1fs rc=%d\n", secs, rc);
    }
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        return rc;
    }
    sync_fd(state);

    if(CANCELLED(state)) goto safe_stop;

    /* Initialize dedup relocation map */
    state->reloc_old   = NULL;
    state->reloc_new   = NULL;
    state->reloc_count = 0;
    state->reloc_cap   = 0;
    state->data_reloc_old   = NULL;
    state->data_reloc_new   = NULL;
    state->data_reloc_count = 0;
    state->data_reloc_cap   = 0;

    /* ---- Phase 2: Inode-aware data block compaction ---- */
    fprintf(stderr, "[compact] === PHASE 2: DATA ===\n");
    struct timespec ts_data_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_data_start);
    atomic_store(&state->phase, COMPACT_PHASE_DATA);
    rc = compact_data_blocks(state);
    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        double secs = (double)(ts_now.tv_sec - ts_data_start.tv_sec) +
                      (double)(ts_now.tv_nsec - ts_data_start.tv_nsec) / 1e9;
        fprintf(stderr, "[compact] DATA done: %.1fs rc=%d\n", secs, rc);
    }
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        goto safe_stop;
    }
    sync_fd(state);

    if(CANCELLED(state)) goto safe_stop;

    /* ---- Phase 3: Move dedup blocks after data ---- */
    fprintf(stderr, "[compact] === PHASE 3: DEDUP ===\n");
    struct timespec ts_dedup_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_dedup_start);
    atomic_store(&state->phase, COMPACT_PHASE_DEDUP);
    rc = compact_dedup_blocks(state);
    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        double secs = (double)(ts_now.tv_sec - ts_dedup_start.tv_sec) +
                      (double)(ts_now.tv_nsec - ts_dedup_start.tv_nsec) / 1e9;
        fprintf(stderr, "[compact] DEDUP done: %.1fs rc=%d reloc_count=%" PRIu64 "\n",
                secs, rc, state->reloc_count);
    }
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        goto safe_stop;
    }
    sync_fd(state);

    sync_fd(state);

    /* ---- Phase 4: Update references ---- */
    fprintf(stderr, "[compact] === PHASE 4: REFS ===\n");
    struct timespec ts_refs_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_refs_start);
safe_stop:
    atomic_store(&state->phase, COMPACT_PHASE_REFS);
    /* Inode extent start_blocks are updated in-place during the data
     * phase (move_extent updates the leaf record directly), so no
     * separate fixup pass is needed. */
    free(state->data_reloc_old);
    free(state->data_reloc_new);
    state->data_reloc_old = NULL;
    state->data_reloc_new = NULL;

    /* Free tree reloc arrays (used by subchannel LBA update) will be
     * freed after update_sme_refs. Keep them alive until then. */

    /* Always update dedup references if any blocks were relocated,
     * even on early cancellation — otherwise the tree/SMEs will
     * reference old (now-freed) positions. */
    if(state->reloc_count > 0)
    {
        fprintf(stderr, "[defrag] reloc_count=%" PRIu64 ", resolving chains...\n",
                state->reloc_count);
        /* Resolve chained relocations: if a block was evicted A→B by
         * the data phase and then moved B→C by the dedup phase, we
         * need A→C (not A→B). */
        struct reloc_map *chain_map = reloc_map_create(state->reloc_old,
                                                       state->reloc_new,
                                                       state->reloc_count);
        if(chain_map)
        {
            for(uint64_t i = 0; i < state->reloc_count; i++)
            {
                uint64_t dest = state->reloc_new[i];
                for(int depth = 0; depth < 10; depth++) /* max chain depth */
                {
                    uint64_t next = reloc_map_get(chain_map, dest);
                    if(next == 0 || next == dest) break;
                    dest = next;
                }
                state->reloc_new[i] = dest;
            }
            reloc_map_destroy(chain_map);
        }

        /* Now build the final map with resolved destinations */
        struct reloc_map *rmap = reloc_map_create(state->reloc_old,
                                                  state->reloc_new,
                                                  state->reloc_count);
        if(rmap)
        {
            update_dedup_tree_refs(state, rmap);
            sync_fd(state);

            /* Build tree reloc map for subchannel LBA fixup in CD SMEs */
            struct reloc_map *tree_rmap = NULL;
            if(state->tree_reloc_count > 0)
                tree_rmap = reloc_map_create(state->tree_reloc_old,
                                             state->tree_reloc_new,
                                             state->tree_reloc_count);

            update_sme_refs(state, rmap, tree_rmap);
            sync_fd(state);

            reloc_map_destroy(rmap);
            if(tree_rmap) reloc_map_destroy(tree_rmap);
        }
    }
    free(state->reloc_old);
    free(state->reloc_new);
    state->reloc_old = NULL;
    state->reloc_new = NULL;

    free(state->tree_reloc_old);
    free(state->tree_reloc_new);
    state->tree_reloc_old = NULL;
    state->tree_reloc_new = NULL;

    atomic_store(&state->phase, COMPACT_PHASE_BITMAP);
    obmafs3_bitmap_write(ctx);
    obmafs3_sb_write(ctx->fd, &ctx->sb);
    sync_fd(state);

    /* ---- Done ---- */
    atomic_store(&state->phase, COMPACT_PHASE_DONE);
    atomic_store(&state->finished, 1);
    return OBMAFS3_OK;
}
