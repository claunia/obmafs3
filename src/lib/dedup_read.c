// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_read.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Media image read path, CD image read path, and CD sector map cache flush/free.
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
/*  Persistent dedup data-block cache (opaque, heap-allocated)         */
/* ------------------------------------------------------------------ */

/** Cached dedup data block that persists across FUSE read calls. */
struct media_dedup_block_cache
{
    uint8_t *dedup_buf;        ///< Raw on-disk dedup block (dedup_block_size bytes)
    uint8_t *decomp_buf;      ///< Decompressed payload (lazy, dedup_block_size bytes)
    uint64_t cached_lba;      ///< LBA of the block currently in dedup_buf (0 = none)
    int      compressed;      ///< Non-zero when decomp_buf holds decompressed data
    uint64_t block_size;      ///< Expected dedup_block_size (for validation)
};

/* ------------------------------------------------------------------ */
/*  Batch sorted lookup helpers                                        */
/* ------------------------------------------------------------------ */

/** Pair used to sort sector hashes for B+Tree locality. */
struct hash_lookup_pair
{
    uint64_t hash;
    uint64_t sme_idx;
};

/** qsort comparator: sort by hash ascending. */
static int hash_pair_cmp(const void *a, const void *b)
{
    const struct hash_lookup_pair *pa = (const struct hash_lookup_pair *)a;
    const struct hash_lookup_pair *pb = (const struct hash_lookup_pair *)b;
    if(pa->hash < pb->hash) return -1;
    if(pa->hash > pb->hash) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Parallel leaf-read worker for the prefetch phase                   */
/* ------------------------------------------------------------------ */

/** Work descriptor for one parallel leaf-read thread. */
struct leaf_read_work
{
    int                      fd;      ///< file descriptor (pread is thread-safe)
    const uint64_t          *lbas;    ///< sorted, deduplicated leaf LBAs
    uint8_t                 *bufs;    ///< output buffer: bsz * (end - start) bytes
    uint64_t                 start;   ///< first index (inclusive)
    uint64_t                 end;     ///< last index (exclusive)
    size_t                   bsz;
};

/**
 * Thread entry: read a slice of leaves with raw pread() — no mutex,
 * no cache interaction.  Each thread writes into its own pre-allocated
 * region of bufs[], so there is zero synchronization during I/O.
 */
static void *leaf_read_worker(void *arg)
{
    struct leaf_read_work *w = (struct leaf_read_work *)arg;

    for(uint64_t i = w->start; i < w->end; i++)
    {
        pread(w->fd,
              w->bufs + (i - w->start) * w->bsz,
              w->bsz,
              (off_t)(w->lbas[i] * w->bsz));
    }
    return NULL;
}

/** Max threads for parallel leaf reads. */
#define LEAF_READ_THREADS 32

/* ------------------------------------------------------------------ */
/*  Media image read path                                              */
/* ------------------------------------------------------------------ */

/**
 * Read data from a media image file.
 *
 * The inode's data blocks store sector_map_entry records that map
 * each logical sector to a hash.  The hash is used to look up the
 * actual sector data in the dedup tree.
 *
 * @param ctx         Filesystem context.
 * @param inode       Inode of the media image file.
 * @param offset      Byte offset in the original file.
 * @param buf         Output buffer.
 * @param size        Number of bytes to read.
 * @param sector_size Size of each sector in the image.
 * @return OBMAFS3_OK on success, error code otherwise.
 */
int obmafs3_read_media_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                                  size_t size, uint16_t sector_size, void *leaf_cache,
                                  void *dedup_cache)
{
    if(offset >= inode->file_size) return OBMAFS3_OK;

    if(offset + size > inode->file_size) size = (size_t)(inode->file_size - offset);

    if(size == 0) return OBMAFS3_OK;

    struct timespec t_start, t_get_tree, t_sme_read, t_sort, t_dedup_lookup, t_data_read;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Get the dedup tree for this sector size */
    struct btree_header dedup_hdr;
    uint64_t            dedup_hdr_lba;
    int                 rc = obmafs3_dedup_get_tree(ctx, sector_size, &dedup_hdr, &dedup_hdr_lba);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "[read_media_image] dedup_get_tree FAILED rc=%d ss=%u\n", rc, sector_size);
        return rc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_get_tree);

    /* ---- Batch-read all needed sector_map_entries in one call ----
     *
     * A single 4 KiB block holds 4096/18 = 227 entries.  The old code
     * re-read the same block for every sector — thousands of pread
     * syscalls for a typical 1 MiB FUSE read.  Pre-reading the whole
     * range collapses that to a handful of block reads. */
    int64_t  first_sector = (int64_t)(offset / sector_size);
    int64_t  last_sector  = (int64_t)((offset + size - 1) / sector_size);
    uint64_t sme_count    = (uint64_t)(last_sector - first_sector + 1);

    struct sector_map_entry *sme_batch = malloc((size_t)(sme_count * sizeof(struct sector_map_entry)));
    if(!sme_batch) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Temporary inode copy for reading sector map (need to adjust file_size) */
    struct inode_record map_inode;
    memcpy(&map_inode, inode, sizeof(map_inode));
    map_inode.file_size = inode->sector_map_size * sizeof(struct sector_map_entry);

    uint64_t sme_offset = (uint64_t)first_sector * sizeof(struct sector_map_entry);
    rc                  = obmafs3_read_file_data(ctx, &map_inode, sme_offset, sme_batch,
                                                 (size_t)(sme_count * sizeof(struct sector_map_entry)));
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr,
                "[read_media_image] batch read_file_data(sme) FAILED rc=%d "
                "first_sector=%" PRId64 " count=%" PRIu64 " sme_offset=%" PRIu64 " map_file_size=%" PRIu64
                " sector_map_size=%" PRIu64 " inode=%" PRIu64 "\n",
                rc, first_sector, sme_count, sme_offset, map_inode.file_size, inode->sector_map_size, inode->inode_id);
        free(sme_batch);
        return rc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_sme_read);

    /* ---- Dedup data-block cache ----
     * When an external cache is provided (persistent across FUSE read
     * calls), reuse its buffers and cached LBA; otherwise allocate
     * per-call buffers that are freed at the end of this function. */
    struct media_dedup_block_cache *dbc = (struct media_dedup_block_cache *)dedup_cache;
    int                             owns_dedup_bufs;

    uint8_t *dedup_buf;
    uint8_t *decomp_buf;
    uint64_t cached_dedup_lba;
    int      cached_compressed;

    if(dbc)
    {
        dedup_buf        = dbc->dedup_buf;
        decomp_buf       = dbc->decomp_buf;
        cached_dedup_lba = dbc->cached_lba;
        cached_compressed = dbc->compressed;
        owns_dedup_bufs  = 0;
    }
    else
    {
        dedup_buf = malloc((size_t)ctx->sb.dedup_block_size);
        if(!dedup_buf)
        {
            free(sme_batch);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }
        decomp_buf       = NULL;
        cached_dedup_lba = 0;
        cached_compressed = 0;
        owns_dedup_bufs  = 1;
    }

    /* Leaf-level lookup cache: avoids full tree traversal when
     * consecutive sector hashes land in the same B+Tree leaf.
     * When an external cache is provided (persistent across FUSE read
     * calls), use it; otherwise fall back to a stack-local cache. */
    struct dedup_leaf_cache  local_leaf_cache = DEDUP_LEAF_CACHE_INIT;
    struct dedup_leaf_cache *lc;
    int                      owns_leaf_cache;
    if(leaf_cache)
    {
        lc              = (struct dedup_leaf_cache *)leaf_cache;
        owns_leaf_cache = 0;
    }
    else
    {
        lc              = &local_leaf_cache;
        owns_leaf_cache = 1;
    }

    uint8_t *out        = (uint8_t *)buf;
    size_t   bytes_read = 0;

    /* ---- Phase 1: Batch sorted dedup lookups ----
     *
     * Collect all sector hashes, sort by hash value, then look up each
     * unique hash once in sorted order.  Benefits:
     *  (a) Consecutive sorted hashes tend to reside in the same B+Tree
     *      leaf, so the leaf cache hit rate jumps dramatically.
     *  (b) Leaf reads proceed in roughly sequential LBA order (the
     *      B+Tree is hash-sorted → leaf LBAs track hash order), which
     *      lets the kernel readahead/prefetch work effectively on HDD.
     *  (c) Duplicate hashes (e.g. zero sectors) are looked up only
     *      once instead of once per sector.
     */
    struct hash_lookup_pair *pairs = malloc((size_t)(sme_count * sizeof(struct hash_lookup_pair)));
    struct dedup_entry      *de_results = malloc((size_t)(sme_count * sizeof(struct dedup_entry)));
    if(!pairs || !de_results)
    {
        free(pairs);
        free(de_results);
        if(owns_leaf_cache) free(lc->leaf_buf);
        free(sme_batch);
        if(owns_dedup_bufs) { free(decomp_buf); free(dedup_buf); }
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    for(uint64_t i = 0; i < sme_count; i++)
    {
        pairs[i].hash    = sme_batch[i].hash;
        pairs[i].sme_idx = i;
    }

    qsort(pairs, (size_t)sme_count, sizeof(struct hash_lookup_pair), hash_pair_cmp);

    clock_gettime(CLOCK_MONOTONIC, &t_sort);

    /* ---- Phase 1a: Prefetch B+Tree index nodes AND leaves ----
     *
     * The dedup tree is keyed by hash, so our sorted hashes map to
     * random index/leaf nodes scattered across disk.  Single-hash
     * traversals cause hundreds of random 4 KiB pread() calls (~15 ms
     * each on HDD → seconds).
     *
     * Instead we do a *batch* level-by-level descent:
     *  1. Collect all unique hashes that aren't in the global lookup cache.
     *  2. Call dedup_batch_find_leaves() which descends the B+Tree one
     *     level at a time, bulk-reading every level with a single pread()
     *     and inserting nodes into the node cache.
     *  3. Sort the resulting leaf LBAs, bulk-read them with one more
     *     pread(), and insert into the cache.
     *
     * Total I/O: one pread() per tree level + one pread() for all
     * leaves.  For a typical 3-level tree this is ~4 sequential reads
     * instead of ~512 random seeks. */
    uint64_t prefetch_leaves = 0;
    {
        struct dedup_node_cache   *nc  = (struct dedup_node_cache *)ctx->dedup_node_cache;
        struct dedup_lookup_cache *dlc = (struct dedup_lookup_cache *)ctx->dedup_lookup_cache;

        /* Collect unique hashes that need tree lookup. */
        uint64_t *need_hashes = malloc((size_t)(sme_count * sizeof(uint64_t)));
        uint64_t  need_count  = 0;

        if(need_hashes)
        {
            for(uint64_t i = 0; i < sme_count; i++)
            {
                if(i > 0 && pairs[i].hash == pairs[i - 1].hash)
                    continue;
                struct dedup_entry dummy;
                if(dlc && dedup_lc_get(dlc, pairs[i].hash, dedup_hdr_lba, &dummy))
                    continue;
                need_hashes[need_count++] = pairs[i].hash;
            }
        }

        if(need_hashes && need_count > 0 && dedup_hdr.root_node_lba != 0)
        {
            /* Step 1: batch-descend index levels → get leaf LBAs. */
            uint64_t *leaf_lbas = malloc((size_t)(need_count * sizeof(uint64_t)));
            if(leaf_lbas)
            {
                struct timespec t_bd_start, t_bd_end, t_lr_end;
                clock_gettime(CLOCK_MONOTONIC, &t_bd_start);

                dedup_batch_find_leaves(ctx, &dedup_hdr, need_hashes,
                                        need_count, leaf_lbas, nc);

                clock_gettime(CLOCK_MONOTONIC, &t_bd_end);

                /* Step 2: sort leaf LBAs, deduplicate, bulk-read. */
                qsort(leaf_lbas, (size_t)need_count, sizeof(uint64_t), lba_cmp);

                uint64_t unique_leaves = 0;
                for(uint64_t i = 0; i < need_count; i++)
                    if(leaf_lbas[i] != 0 &&
                       (i == 0 || leaf_lbas[i] != leaf_lbas[i - 1]))
                        leaf_lbas[unique_leaves++] = leaf_lbas[i];

                if(nc && unique_leaves > 0)
                {
                    size_t   bsz        = (size_t)ctx->sb.block_size;
                    uint64_t lba_lo     = leaf_lbas[0];
                    uint64_t lba_hi     = leaf_lbas[unique_leaves - 1];
                    size_t   range_bytes = (size_t)((lba_hi - lba_lo + 1) * bsz);

                    if(range_bytes <= 64u * 1024 * 1024)
                    {
                        uint8_t *bulk = malloc(range_bytes);
                        if(bulk)
                        {
                            ssize_t got = pread(ctx->fd, bulk, range_bytes,
                                                (off_t)(lba_lo * bsz));
                            if(got > 0)
                            {
                                for(uint64_t i = 0; i < unique_leaves; i++)
                                {
                                    size_t off = (size_t)((leaf_lbas[i] - lba_lo) * bsz);
                                    if(off + bsz <= (size_t)got)
                                        dedup_cache_insert(nc, leaf_lbas[i],
                                                           bulk + off);
                                }
                            }
                            free(bulk);
                        }
                        else
                        {
                            /* malloc failed — fall back to threaded reads. */
                            goto threaded_leaf_read;
                        }
                    }
                    else
                    {
                    threaded_leaf_read:;
                        /* Leaves span too much disk (or malloc failed).
                         *
                         * Launch LEAF_READ_THREADS threads, each doing
                         * raw pread() into a pre-allocated buffer with
                         * ZERO mutex or cache interaction.  This gives
                         * the kernel N truly concurrent I/O requests,
                         * allowing the I/O scheduler to elevator-sort
                         * them into near-sequential sweeps.
                         *
                         * After all threads finish, we insert every
                         * block into the node cache single-threaded
                         * (no contention). */

                        /* Pre-allocate one buffer for all leaves. */
                        uint8_t *all_bufs = malloc((size_t)(unique_leaves * bsz));
                        if(!all_bufs)
                        {
                            /* Last resort: single-threaded sequential. */
                            uint8_t *tbuf = obmafs3_get_thread_bufs(ctx)->node_buf;
                            for(uint64_t i = 0; i < unique_leaves; i++)
                                dedup_cache_read(nc, ctx, leaf_lbas[i], tbuf, bsz);
                        }
                        else
                        {
                            /* Issue WILLNEED for all leaves so the
                             * kernel can start prefetching while we
                             * spawn threads. */
                            for(uint64_t i = 0; i < unique_leaves; i++)
                                posix_fadvise(ctx->fd,
                                              (off_t)(leaf_lbas[i] * bsz),
                                              (off_t)bsz, POSIX_FADV_WILLNEED);

                            int n_threads = LEAF_READ_THREADS;
                            if((uint64_t)n_threads > unique_leaves)
                                n_threads = (int)unique_leaves;

                            pthread_t             tids[LEAF_READ_THREADS];
                            struct leaf_read_work  work[LEAF_READ_THREADS];
                            uint64_t per = unique_leaves / (uint64_t)n_threads;
                            uint64_t rem = unique_leaves % (uint64_t)n_threads;
                            uint64_t pos = 0;

                            for(int t = 0; t < n_threads; t++)
                            {
                                work[t].fd    = ctx->fd;
                                work[t].lbas  = leaf_lbas;
                                work[t].bsz   = bsz;
                                work[t].start = pos;
                                pos += per + ((uint64_t)t < rem ? 1 : 0);
                                work[t].end   = pos;
                                work[t].bufs  = all_bufs + work[t].start * bsz;
                                pthread_create(&tids[t], NULL,
                                               leaf_read_worker, &work[t]);
                            }
                            for(int t = 0; t < n_threads; t++)
                                pthread_join(tids[t], NULL);

                            /* Single-threaded batch insert — no mutex
                             * contention, no disk I/O. */
                            for(uint64_t i = 0; i < unique_leaves; i++)
                                dedup_cache_insert(nc, leaf_lbas[i],
                                                   all_bufs + i * bsz);

                            free(all_bufs);
                        }
                    }
                }
                prefetch_leaves = unique_leaves;

                clock_gettime(CLOCK_MONOTONIC, &t_lr_end);
                {
                    double bd_ms = (t_bd_end.tv_sec - t_bd_start.tv_sec) * 1000.0
                                 + (t_bd_end.tv_nsec - t_bd_start.tv_nsec) / 1e6;
                    double lr_ms = (t_lr_end.tv_sec - t_bd_end.tv_sec) * 1000.0
                                 + (t_lr_end.tv_nsec - t_bd_end.tv_nsec) / 1e6;
                    fprintf(stderr, "[prefetch-split] batch_descent=%.1fms  leaf_read=%.1fms  "
                            "need=%llu  unique_leaves=%llu  range=%.1fKiB\n",
                            bd_ms, lr_ms,
                            (unsigned long long)need_count,
                            (unsigned long long)unique_leaves,
                            (nc && unique_leaves > 0)
                              ? ((double)((leaf_lbas[unique_leaves-1] - leaf_lbas[0] + 1) * (uint64_t)ctx->sb.block_size) / 1024.0)
                              : 0.0);
                }
                free(leaf_lbas);
            }
        }
        free(need_hashes);
    }

    struct timespec t_prefetch;
    clock_gettime(CLOCK_MONOTONIC, &t_prefetch);

    uint64_t dedup_unique = 0, dedup_dup = 0, dedup_lc_hits = 0;

    /* Look up each unique hash once in sorted order. */
    for(uint64_t i = 0; i < sme_count; i++)
    {
        /* Deduplicate: reuse the previous result for identical hashes. */
        if(i > 0 && pairs[i].hash == pairs[i - 1].hash)
        {
            de_results[pairs[i].sme_idx] = de_results[pairs[i - 1].sme_idx];
            dedup_dup++;
            continue;
        }

        dedup_unique++;

        struct dedup_entry de;
        rc = dedup_lookup_cached(ctx, &dedup_hdr, dedup_hdr_lba, pairs[i].hash, &de, lc);
        if(rc != OBMAFS3_OK)
        {
            fprintf(stderr,
                    "[read_media_image] dedup_lookup FAILED rc=%d hash=%" PRIu64 " sme_idx=%" PRIu64 " inode=%" PRIu64
                    "\n",
                    rc, pairs[i].hash, pairs[i].sme_idx, inode->inode_id);
            free(pairs);
            free(de_results);
            if(owns_leaf_cache) free(lc->leaf_buf);
            free(sme_batch);
            if(owns_dedup_bufs) { free(decomp_buf); free(dedup_buf); }
            return rc;
        }
        de_results[pairs[i].sme_idx] = de;
    }

    free(pairs);

    clock_gettime(CLOCK_MONOTONIC, &t_dedup_lookup);

    uint64_t data_block_reads = 0, data_decomps = 0;

    /* ---- Phase 2: Read sector data in original order ---- */
    while(bytes_read < size)
    {
        uint64_t read_pos         = offset + bytes_read;
        int64_t  sector_num       = (int64_t)(read_pos / sector_size);
        size_t   offset_in_sector = (size_t)(read_pos % sector_size);

        /* How many bytes remain in this sector */
        size_t remaining_in_sector = (size_t)sector_size - offset_in_sector;
        size_t remaining_in_read   = size - bytes_read;
        size_t chunk               = remaining_in_read < remaining_in_sector ? remaining_in_read : remaining_in_sector;

        /* Index into the pre-fetched batch */
        uint64_t sme_idx = (uint64_t)(sector_num - first_sector);

        /* Use the pre-computed dedup entry */
        struct dedup_entry de = de_results[sme_idx];

        /* Read the dedup data block if not already cached */
        if(de.block_lba != cached_dedup_lba)
        {
            /* Read first standard block to get the header */
            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK)
            {
                fprintf(stderr,
                        "[read_media_image] block_read(dedup hdr) FAILED rc=%d lba=%" PRIu64 " sector=%" PRId64 "\n",
                        rc, de.block_lba, sector_num);
                free(de_results);
                if(owns_leaf_cache) free(lc->leaf_buf);
                free(sme_batch);
                if(owns_dedup_bufs) { free(decomp_buf); free(dedup_buf); }
                return rc;
            }

            /* Check if the block is compressed */
            struct block_header bhdr;
            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

            if(bhdr.magic != OBMAFS3_BLOCK_MAGIC)
            {
                fprintf(stderr,
                        "[read_media_image] BAD BLOCK MAGIC at lba=%" PRIu64 " hash=%" PRIu64
                        " block_offset=%" PRIu64 " sector=%" PRId64
                        " magic=0x%" PRIX64 " (expected 0x%" PRIX64 ")\n",
                        de.block_lba, de.hash, de.block_offset, (int64_t)(offset + bytes_read) / (int64_t)sector_size,
                        bhdr.magic, (uint64_t)OBMAFS3_BLOCK_MAGIC);
                free(de_results);
                if(owns_leaf_cache) free(lc->leaf_buf);
                free(sme_batch);
                if(owns_dedup_bufs) { free(decomp_buf); free(dedup_buf); }
                DBG_RETURN(OBMAFS3_ERR_IO, "bad block magic in dedup data block");
            }

            /* Determine actual on-disk payload size and read remaining */
            uint64_t payload_size;
            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                payload_size = bhdr.compressed_size;
            else
                payload_size = bhdr.original_size;

            uint64_t total_on_disk = sizeof(bhdr) + payload_size;
            uint64_t bs            = ctx->sb.block_size;
            uint64_t needed_std    = (total_on_disk + bs - 1) / bs;

            /* Read remaining standard blocks beyond the first */
            if(needed_std > 1)
            {
                rc = obmafs3_block_read(ctx, de.block_lba + 1, dedup_buf + bs, (size_t)((needed_std - 1) * bs));
                if(rc != OBMAFS3_OK)
                {
                    free(de_results);
                    if(owns_leaf_cache) free(lc->leaf_buf);
                    free(sme_batch);
                    if(owns_dedup_bufs) { free(decomp_buf); free(dedup_buf); }
                    return rc;
                }
            }

            cached_dedup_lba = de.block_lba;
            data_block_reads++;

            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                if(!decomp_buf)
                {
                    decomp_buf = malloc((size_t)ctx->sb.dedup_block_size);
                    if(!decomp_buf)
                    {
                        free(de_results);
                        if(owns_leaf_cache) free(lc->leaf_buf);
                        free(sme_batch);
                        if(owns_dedup_bufs) free(dedup_buf);
                        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                    }
                    /* Store back into persistent cache so it survives */
                    if(dbc) dbc->decomp_buf = decomp_buf;
                }
                rc = obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, dedup_buf + sizeof(bhdr),
                                        (size_t)bhdr.compressed_size, decomp_buf, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK)
                {
                    fprintf(stderr,
                            "[read_media_image] DECOMPRESS FAILED lba=%" PRIu64 " hash=%" PRIu64
                            " block_offset=%" PRIu64 " sector=%" PRId64
                            " compressed_size=%" PRIu64 " original_size=%" PRIu64
                            " needed_std=%" PRIu64 " flags=0x%02x\n",
                            de.block_lba, de.hash, de.block_offset,
                            (int64_t)(offset + bytes_read) / (int64_t)sector_size,
                            bhdr.compressed_size, bhdr.original_size, needed_std, bhdr.flags);
                    free(de_results);
                    if(owns_leaf_cache) free(lc->leaf_buf);
                    free(sme_batch);
                    if(owns_dedup_bufs) { free(decomp_buf); free(dedup_buf); }
                    return rc;
                }
                cached_compressed = 1;
                data_decomps++;
            }
            else
            {
                cached_compressed = 0;
            }

            /* Speculatively prefetch the next different dedup block.
             * Use the pre-computed dedup entries — no tree traversal needed. */
            for(uint64_t next = sme_idx + 1; next < sme_count; next++)
            {
                if(de_results[next].block_lba != de.block_lba)
                {
                    off_t off = (off_t)(de_results[next].block_lba * ctx->sb.block_size);
                    off_t len = (off_t)ctx->sb.dedup_block_size;
                    posix_fadvise(ctx->fd, off, len, POSIX_FADV_WILLNEED);
                    break;
                }
            }
        }

        /* Copy sector data from the dedup block at the stored offset.
         * block_offset includes the header prefix; for compressed blocks
         * decomp_buf holds only the payload so subtract the header. */
        if(cached_compressed)
        {
            size_t decomp_off = de.block_offset - sizeof(struct block_header);
            memcpy(out + bytes_read, decomp_buf + decomp_off + offset_in_sector, chunk);
        }
        else
        {
            memcpy(out + bytes_read, dedup_buf + de.block_offset + offset_in_sector, chunk);
        }
        bytes_read += chunk;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_data_read);

    free(de_results);
    if(owns_leaf_cache) free(lc->leaf_buf);
    free(sme_batch);

    {
        struct dedup_node_cache  *nc  = (struct dedup_node_cache *)ctx->dedup_node_cache;
        struct dedup_lookup_cache *dlc = (struct dedup_lookup_cache *)ctx->dedup_lookup_cache;
        uint32_t nc_count = nc ? nc->count : 0;
        uint32_t nc_cap   = nc ? nc->capacity : 0;
        uint64_t dlc_count = dlc ? dlc->count : 0;
        uint64_t dlc_cap   = dlc ? dlc->capacity : 0;
        fprintf(stderr,
                "[read-timing] read %zu bytes @ %" PRIu64 " ss=%u: "
                "get_tree=%.1fms  sme_read=%.1fms(%" PRIu64 " sectors)  "
                "sort=%.1fms  prefetch=%.1fms(%" PRIu64 " leaves)  "
                "dedup_lookup=%.1fms(uniq=%" PRIu64 " dup=%" PRIu64 ")  "
                "data_read=%.1fms(blks=%" PRIu64 " decomps=%" PRIu64 ")  "
                "TOTAL=%.1fms  nc=%u/%u dlc=%" PRIu64 "/%" PRIu64 "\n",
                size, offset, sector_size,
                timespec_diff_ms(&t_start, &t_get_tree),
                timespec_diff_ms(&t_get_tree, &t_sme_read), sme_count,
                timespec_diff_ms(&t_sme_read, &t_sort),
                timespec_diff_ms(&t_sort, &t_prefetch), prefetch_leaves,
                timespec_diff_ms(&t_prefetch, &t_dedup_lookup), dedup_unique, dedup_dup,
                timespec_diff_ms(&t_dedup_lookup, &t_data_read), data_block_reads, data_decomps,
                timespec_diff_ms(&t_start, &t_data_read),
                nc_count, nc_cap, dlc_count, dlc_cap);
    }

    /* Write back cached state so the next call can reuse the block */
    if(dbc)
    {
        dbc->cached_lba  = cached_dedup_lba;
        dbc->compressed  = cached_compressed;
    }
    else
    {
        free(decomp_buf);
        free(dedup_buf);
    }

    return OBMAFS3_OK;
}

/**
 * Free a persistent dedup leaf cache previously passed to
 * @c obmafs3_read_media_image_data as the @c leaf_cache parameter.
 *
 * Safe to call with NULL.
 *
 * @param lc  Opaque leaf cache pointer (or NULL).
 */
void obmafs3_free_media_leaf_cache(void *lc)
{
    if(!lc) return;
    struct dedup_leaf_cache *cache = (struct dedup_leaf_cache *)lc;
    free(cache->leaf_buf);
    free(cache);
}

/**
 * Allocate an opaque persistent dedup leaf cache for use with
 * @c obmafs3_read_media_image_data.  The caller must free it with
 * @c obmafs3_free_media_leaf_cache when done.
 *
 * @return Opaque leaf cache pointer, or NULL on allocation failure.
 */
void *obmafs3_alloc_media_leaf_cache(void)
{
    struct dedup_leaf_cache *lc = calloc(1, sizeof(*lc));
    return lc;
}

/**
 * Allocate a persistent dedup data-block cache for use with
 * @c obmafs3_read_media_image_data.  Pre-allocates the raw and
 * decompression buffers so they survive across FUSE read calls.
 *
 * The caller must free the cache with @c obmafs3_free_media_dedup_cache
 * when the file handle is released.
 *
 * @param ctx  Filesystem context (used for @c dedup_block_size).
 * @return Opaque cache pointer, or NULL on allocation failure.
 */
void *obmafs3_alloc_media_dedup_cache(struct obmafs3_ctx *ctx)
{
    struct media_dedup_block_cache *c = calloc(1, sizeof(*c));
    if(!c) return NULL;

    c->dedup_buf = malloc((size_t)ctx->sb.dedup_block_size);
    if(!c->dedup_buf)
    {
        free(c);
        return NULL;
    }

    /* decomp_buf is allocated lazily on first compressed block */
    c->cached_lba  = 0;
    c->compressed  = 0;
    c->block_size  = ctx->sb.dedup_block_size;
    return c;
}

/**
 * Free a persistent dedup data-block cache.
 * Safe to call with NULL.
 *
 * @param cache  Opaque cache pointer (or NULL).
 */
void obmafs3_free_media_dedup_cache(void *cache)
{
    if(!cache) return;
    struct media_dedup_block_cache *c = (struct media_dedup_block_cache *)cache;
    free(c->decomp_buf);
    free(c->dedup_buf);
    free(c);
}

/* ------------------------------------------------------------------ */
/*  CD compact disc image read path                                    */
/* ------------------------------------------------------------------ */

/**
 * Read data from a compact disc image, reconstructing full 2352-byte
 * raw sectors from dedup, prefix/suffix B+Trees, and the CD sector map.
 *
 * This is the CD equivalent of @c obmafs3_read_media_image_data.
 * All needed @c cd_sector_map_entry records are batch-read in a single
 * call to @c obmafs3_read_file_data, eliminating per-sector I/O for
 * the map.  Each sector is then reconstructed from:
 *
 *  - Audio mode: full 2352 bytes from the 2352-byte dedup tree.
 *  - Data modes: prefix (generated or from prefix tree) + subheader
 *    (if applicable) + user data (from appropriately-sized dedup tree)
 *    + suffix (generated or from suffix tree).
 *
 * The virtual file size is @c sector_count * @c CD_RAW_SECTOR_SIZE.
 * Sectors that do not have a corresponding entry in the sector map
 * (gaps between tracks) are returned as zero-filled 2352-byte buffers.
 * Reading beyond @c sector_count is not allowed (caller must clamp).
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode record describing the CD image file.
 * @param offset Byte offset into the virtual 2352-byte-per-sector image.
 * @param buf    Output buffer.
 * @param size   Number of bytes to read.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_read_cd_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                               size_t size)
{
    /* Virtual file size: sector_count * 2352 */
    uint64_t virtual_size = inode->sector_count * CD_RAW_SECTOR_SIZE;
    if(offset >= virtual_size) return OBMAFS3_OK;
    if(offset + size > virtual_size) size = (size_t)(virtual_size - offset);
    if(size == 0) return OBMAFS3_OK;

    /* ---- Read ALL cd_sector_map_entries (sparse — indexed by sector field) ---- */
    uint64_t total_entries = inode->sector_map_size;

    struct cd_sector_map_entry *sme_all = NULL;
    if(total_entries > 0)
    {
        sme_all = malloc((size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(!sme_all) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        struct inode_record map_inode;
        memcpy(&map_inode, inode, sizeof(map_inode));
        map_inode.file_size = total_entries * sizeof(struct cd_sector_map_entry);

        int rc = obmafs3_read_file_data(ctx, &map_inode, 0, sme_all,
                                        (size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(rc != OBMAFS3_OK)
        {
            free(sme_all);
            return rc;
        }
    }

    /* ECC context for suffix reconstruction — lazy-allocated on first use */
    void *ecc_ctx = NULL;

    /* Dedup block cache — shared across all sectors in this read */
    uint8_t *dedup_buf = malloc((size_t)ctx->sb.dedup_block_size);
    if(!dedup_buf)
    {
        free(sme_all);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }
    uint8_t *decomp_buf        = NULL;
    uint64_t cached_dedup_lba  = 0;
    int      cached_compressed = 0;

    /* Leaf-level lookup cache — amortises tree traversals across sectors */
    struct dedup_leaf_cache leaf_cache = DEDUP_LEAF_CACHE_INIT;

    /* Cached dedup tree header (changes when the sector's data_size changes) */
    struct btree_header cached_dedup_hdr;
    uint64_t            cached_dedup_hdr_lba = 0;
    uint16_t            cached_data_size = 0;

    int      rc         = OBMAFS3_OK;
    uint8_t *out        = (uint8_t *)buf;
    size_t   bytes_read = 0;

    while(bytes_read < size)
    {
        uint64_t read_pos         = offset + bytes_read;
        int64_t  sector_num       = (int64_t)(read_pos / CD_RAW_SECTOR_SIZE);
        size_t   offset_in_sector = (size_t)(read_pos % CD_RAW_SECTOR_SIZE);

        size_t remaining_in_sector = CD_RAW_SECTOR_SIZE - offset_in_sector;
        size_t remaining_in_read   = size - bytes_read;
        size_t chunk               = remaining_in_read < remaining_in_sector ? remaining_in_read : remaining_in_sector;

        /* Binary search the sector map for this LBA.  Entries are
         * sorted by sector number (written in track order). */
        struct cd_sector_map_entry *sme = NULL;
        if(sme_all && total_entries > 0)
        {
            int64_t lo = 0, hi = (int64_t)total_entries - 1;
            while(lo <= hi)
            {
                int64_t mid = lo + (hi - lo) / 2;
                if(sme_all[mid].sector == sector_num)
                {
                    sme = &sme_all[mid];
                    break;
                }
                else if(sme_all[mid].sector < sector_num)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
        }

        /* Sector not found in the map — gap between tracks.
         * Return a zero-filled 2352-byte buffer. */
        if(!sme)
        {
            uint8_t zero_sector[CD_RAW_SECTOR_SIZE];
            memset(zero_sector, 0, CD_RAW_SECTOR_SIZE);
            memcpy(out + bytes_read, zero_sector + offset_in_sector, chunk);
            bytes_read += chunk;
            continue;
        }

        /* Determine the data_size (= dedup sector size) for this sector */
        uint16_t data_size;
        switch((enum obmafs3_cd_sector_mode)sme->sector_mode)
        {
            case kCdSectorModeAudio:
                data_size = CD_RAW_SECTOR_SIZE;
                break;
            case kCdSectorMode1:
                data_size = CD_DATA_SIZE;
                break;
            case kCdSectorMode2:
                data_size = 2336;
                break;
            case kCdSectorMode2Form1:
                data_size = CD_DATA_SIZE;
                break;
            case kCdSectorMode2Form2:
                data_size = 2328;
                break;
            default:
                rc = OBMAFS3_ERR_INVAL;
                goto fail;
        }

        /* If the dedup tree changed, get the new header and invalidate
         * the leaf cache (different tree = different leaves). */
        if(data_size != cached_data_size)
        {
            uint64_t hdr_lba;
            rc = obmafs3_dedup_get_tree(ctx, data_size, &cached_dedup_hdr, &hdr_lba);
            if(rc != OBMAFS3_OK)
            {
                fprintf(stderr, "[read_cd_image] dedup_get_tree FAILED rc=%d ss=%u\n", rc, data_size);
                goto fail;
            }
            cached_data_size     = data_size;
            cached_dedup_hdr_lba = hdr_lba;
            /* Invalidate leaf cache — it belongs to the previous tree */
            free(leaf_cache.leaf_buf);
            leaf_cache = (struct dedup_leaf_cache)DEDUP_LEAF_CACHE_INIT;
        }

        /* ---- Look up the sector's hash in the dedup tree ---- */
        struct dedup_entry de;
        rc = dedup_lookup_cached(ctx, &cached_dedup_hdr, cached_dedup_hdr_lba, sme->hash, &de, &leaf_cache);
        if(rc != OBMAFS3_OK)
        {
            fprintf(stderr,
                    "[read_cd_image] dedup_lookup FAILED rc=%d hash=%" PRIu64 " sector=%" PRId64 " inode=%" PRIu64 "\n",
                    rc, sme->hash, sector_num, inode->inode_id);
            goto fail;
        }

        /* ---- Read the dedup data block if not already cached ---- */
        if(de.block_lba != cached_dedup_lba)
        {
            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) goto fail;

            struct block_header bhdr;
            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

            if(bhdr.magic != OBMAFS3_BLOCK_MAGIC)
            {
                fprintf(stderr,
                        "[read_cd_image] BAD BLOCK MAGIC at lba=%" PRIu64 " hash=%" PRIu64
                        " block_offset=%" PRIu64 " sector=%" PRId64
                        " magic=0x%" PRIX64 " (expected 0x%" PRIX64 ")\n",
                        de.block_lba, de.hash, de.block_offset, sector_num,
                        bhdr.magic, (uint64_t)OBMAFS3_BLOCK_MAGIC);
                rc = OBMAFS3_ERR_IO;
                goto fail;
            }

            uint64_t payload_size =
                (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? bhdr.compressed_size : bhdr.original_size;
            uint64_t total_on_disk = sizeof(bhdr) + payload_size;
            uint64_t bs            = ctx->sb.block_size;
            uint64_t needed_std    = (total_on_disk + bs - 1) / bs;

            if(needed_std > 1)
            {
                rc = obmafs3_block_read(ctx, de.block_lba + 1, dedup_buf + bs, (size_t)((needed_std - 1) * bs));
                if(rc != OBMAFS3_OK) goto fail;
            }

            cached_dedup_lba = de.block_lba;

            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                if(!decomp_buf)
                {
                    decomp_buf = malloc((size_t)ctx->sb.dedup_block_size);
                    if(!decomp_buf)
                    {
                        rc = OBMAFS3_ERR_NOMEM;
                        goto fail;
                    }
                }
                rc = obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, dedup_buf + sizeof(bhdr),
                                        (size_t)bhdr.compressed_size, decomp_buf, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK)
                {
                    fprintf(stderr,
                            "[read_cd_image] DECOMPRESS FAILED lba=%" PRIu64 " hash=%" PRIu64
                            " block_offset=%" PRIu64 " sector=%" PRId64
                            " compressed_size=%" PRIu64 " original_size=%" PRIu64
                            " needed_std=%" PRIu64 " flags=0x%02x\n",
                            de.block_lba, de.hash, de.block_offset, sector_num,
                            bhdr.compressed_size, bhdr.original_size, needed_std, bhdr.flags);
                    goto fail;
                }
                cached_compressed = 1;
            }
            else
            {
                cached_compressed = 0;
            }
        }

        /* ---- Reconstruct the full 2352-byte raw sector ---- */
        uint8_t sector_buf[CD_RAW_SECTOR_SIZE];
        memset(sector_buf, 0, CD_RAW_SECTOR_SIZE);

        /* Copy the data portion from the dedup block */
        const uint8_t *src_data;
        if(cached_compressed)
        {
            size_t decomp_off = de.block_offset - sizeof(struct block_header);
            src_data          = decomp_buf + decomp_off;
        }
        else
        {
            src_data = dedup_buf + de.block_offset;
        }

        if(sme->sector_mode == kCdSectorModeAudio)
        {
            /* Audio: full 2352 bytes IS the dedup data */
            memcpy(sector_buf, src_data, CD_RAW_SECTOR_SIZE);
        }
        else
        {
            int has_subheader = (sme->sector_mode == kCdSectorMode2Form1 || sme->sector_mode == kCdSectorMode2Form2);

            /* 1. Prefix (bytes 0-15) */
            if(sme->generated_prefix) { ecc_cd_reconstruct_prefix(sector_buf, sme->sector_mode, sector_num); }
            else
            {
                uint8_t pfx[CD_PREFIX_DATA_SIZE];
                rc = obmafs3_cd_prefix_get(ctx, sme->prefix_hash, pfx);
                if(rc != OBMAFS3_OK) goto fail;
                memcpy(sector_buf, pfx, CD_PREFIX_SIZE);
            }

            /* 2. Subheader (bytes 16-23) for Mode 2 variants */
            if(has_subheader)
            {
                memcpy(sector_buf + CD_PREFIX_SIZE, sme->subheader, 4);
                memcpy(sector_buf + CD_PREFIX_SIZE + 4, sme->subheader + 4, 4);
            }

            /* 3. Data portion */
            {
                int data_offset_in_sector = has_subheader ? CD_PREFIX_SIZE + 8 : CD_PREFIX_SIZE;
                memcpy(sector_buf + data_offset_in_sector, src_data, data_size);
            }

            /* 4. Suffix */
            if(sme->sector_mode == kCdSectorMode2) { /* Raw Mode 2 has no suffix */ }
            else if(sme->generated_suffix)
            {
                if(!ecc_ctx)
                {
                    ecc_ctx = ecc_cd_init();
                    if(!ecc_ctx)
                    {
                        rc = OBMAFS3_ERR_NOMEM;
                        goto fail;
                    }
                }
                ecc_cd_reconstruct(ecc_ctx, sector_buf, sme->sector_mode);
            }
            else
            {
                uint8_t sfx[CD_SUFFIX_DATA_SIZE];
                rc = obmafs3_cd_suffix_get(ctx, sme->suffix_hash, sfx);
                if(rc != OBMAFS3_OK) goto fail;
                memcpy(sector_buf + CD_RAW_SECTOR_SIZE - CD_SUFFIX_SIZE, sfx, CD_SUFFIX_SIZE);
            }
        }

        memcpy(out + bytes_read, sector_buf + offset_in_sector, chunk);
        bytes_read += chunk;
    }

    ecc_cd_free(ecc_ctx);
    free(leaf_cache.leaf_buf);
    free(decomp_buf);
    free(dedup_buf);
    free(sme_all);
    return OBMAFS3_OK;

fail:
    ecc_cd_free(ecc_ctx);
    free(leaf_cache.leaf_buf);
    free(decomp_buf);
    free(dedup_buf);
    free(sme_all);
    return rc;
}

/**
 * Read subchannel data from a kFileTypeSubchannelFile.
 *
 * The subchannel file has no data of its own.  Its @c sector_count field
 * stores the parent CD image's inode_id.  This function loads the parent
 * inode, reads its CD sector map, and for each requested 96-byte sector
 * offset, looks up the subchannel hash via binary search and fetches the
 * 96-byte subchannel data from the CD subchannel B+Tree.  Sectors
 * without subchannel data are zero-filled.
 *
 * @param ctx        Filesystem context.
 * @param sub_inode  Inode of the subchannel sidecar file.
 * @param offset     Byte offset into the virtual subchannel stream.
 * @param buf        Output buffer.
 * @param size       Number of bytes to read.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_read_subchannel_data(struct obmafs3_ctx *ctx, const struct inode_record *sub_inode, uint64_t offset,
                                 void *buf, size_t size)
{
    /* The parent CD image inode_id is stored in sector_count */
    uint64_t parent_inode_id = sub_inode->sector_count;

    struct inode_record parent_inode;
    int                 rc = obmafs3_inode_get(ctx, parent_inode_id, &parent_inode);
    if(rc != OBMAFS3_OK) return rc;

    /* Virtual size: parent's sector_count * 96 */
    uint64_t virtual_size = parent_inode.sector_count * CD_SUBCHANNEL_SIZE;
    if(offset >= virtual_size) return OBMAFS3_OK;
    if(offset + size > virtual_size) size = (size_t)(virtual_size - offset);
    if(size == 0) return OBMAFS3_OK;

    /* Read all cd_sector_map_entries from the parent */
    uint64_t                    total_entries = parent_inode.sector_map_size;
    struct cd_sector_map_entry *sme_all       = NULL;
    if(total_entries > 0)
    {
        sme_all = malloc((size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(!sme_all) return OBMAFS3_ERR_NOMEM;

        struct inode_record map_inode;
        memcpy(&map_inode, &parent_inode, sizeof(map_inode));
        map_inode.file_size = total_entries * sizeof(struct cd_sector_map_entry);

        rc = obmafs3_read_file_data(ctx, &map_inode, 0, sme_all,
                                    (size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(rc != OBMAFS3_OK)
        {
            free(sme_all);
            return rc;
        }
    }

    uint8_t *out        = (uint8_t *)buf;
    size_t   bytes_read = 0;

    while(bytes_read < size)
    {
        uint64_t read_pos         = offset + bytes_read;
        int64_t  sector_num       = (int64_t)(read_pos / CD_SUBCHANNEL_SIZE);
        size_t   offset_in_sector = (size_t)(read_pos % CD_SUBCHANNEL_SIZE);

        size_t remaining_in_sector = CD_SUBCHANNEL_SIZE - offset_in_sector;
        size_t remaining_in_read   = size - bytes_read;
        size_t chunk               = remaining_in_read < remaining_in_sector ? remaining_in_read : remaining_in_sector;

        /* Binary search for this sector number */
        uint64_t subchannel_hash = 0;
        if(sme_all && total_entries > 0)
        {
            int64_t lo = 0, hi = (int64_t)total_entries - 1;
            while(lo <= hi)
            {
                int64_t mid = lo + (hi - lo) / 2;
                if(sme_all[mid].sector == sector_num)
                {
                    subchannel_hash = sme_all[mid].subchannel_hash;
                    break;
                }
                else if(sme_all[mid].sector < sector_num)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
        }

        /* Fetch subchannel data or zero-fill */
        uint8_t sub_sector[CD_SUBCHANNEL_DATA_SIZE];
        if(subchannel_hash != 0)
        {
            rc = obmafs3_cd_subchannel_get(ctx, subchannel_hash, sub_sector);
            if(rc != OBMAFS3_OK) memset(sub_sector, 0, CD_SUBCHANNEL_DATA_SIZE);
        }
        else
        {
            memset(sub_sector, 0, CD_SUBCHANNEL_DATA_SIZE);
        }

        memcpy(out + bytes_read, sub_sector + offset_in_sector, chunk);
        bytes_read += chunk;
    }

    free(sme_all);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  CD sector map cache flush / free                                   */
/* ------------------------------------------------------------------ */

/**
 * Write a batch of CD sector map entries to the inode's data blocks.
 *
 * Appends @p count entries at the current @c sector_map_size offset
 * and advances the map size accordingly.
 *
 * @param ctx      Filesystem context.
 * @param inode    Inode record to update (modified in place).
 * @param entries  Array of CD sector map entries to write.
 * @param count    Number of entries.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int write_cd_sector_map_batch(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                     const struct cd_sector_map_entry *entries, uint64_t count)
{
    if(count == 0) return OBMAFS3_OK;

    size_t   entry_size  = sizeof(struct cd_sector_map_entry);
    uint64_t map_offset  = inode->sector_map_size * entry_size;
    size_t   total_bytes = (size_t)(count * entry_size);

    uint64_t saved_file_size = inode->file_size;
    inode->file_size         = map_offset;

    int rc = obmafs3_write_file_data(ctx, inode, map_offset, entries, total_bytes);

    inode->file_size = saved_file_size;

    if(rc == OBMAFS3_OK) inode->sector_map_size += count;

    return rc;
}

/**
 * Flush cached CD sector map entries to disk.
 *
 * Writes all accumulated @c cd_sector_map_entry records from @p cache
 * to disk and resets the cache count.
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode record to update.
 * @param cache  CD sector map cache to flush.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_flush_cd_sector_map_cache(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                      struct cd_sector_map_cache *cache)
{
    if(!cache || cache->count == 0) return OBMAFS3_OK;

    int rc = write_cd_sector_map_batch(ctx, inode, cache->entries, cache->count);
    if(rc == OBMAFS3_OK) { cache->count = 0; }
    return rc;
}

/**
 * Free all resources held by a CD sector map cache.
 *
 * @param cache  CD sector map cache to free.
 */
void obmafs3_free_cd_sector_map_cache(struct cd_sector_map_cache *cache)
{
    if(!cache) return;
    free(cache->entries);
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}
