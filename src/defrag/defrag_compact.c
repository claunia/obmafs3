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
#include <unistd.h>

#define CANCELLED(s) (atomic_load(&(s)->cancel_requested))

/* ------------------------------------------------------------------ */
/*  Phase labels                                                       */
/* ------------------------------------------------------------------ */

const char *compact_phase_labels[COMPACT_NUM_PHASES] = {
    "Preparing",
    "Moving data blocks",
    "Moving dedup blocks",
    "Relocating trees",
    "Flushing metadata",
    "Done"
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

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
 * Bulk copy a contiguous range of blocks in one large I/O.
 * Uses a single pread + pwrite for the entire range, which is
 * dramatically faster than per-block calls on both SSDs and HDDs.
 * Issues posix_fadvise prefetch before each read.
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
        /* Fallback: copy in chunks that fit in the buffer */
        size_t chunk_blocks = g_io_cap / block_size;
        if(chunk_blocks < 1) chunk_blocks = 1;

        for(uint64_t off = 0; off < count; off += chunk_blocks)
        {
            uint64_t n = count - off;
            if(n > chunk_blocks) n = chunk_blocks;
            size_t bytes = (size_t)n * block_size;

            ssize_t rd = pread(ctx->fd, g_io_buf, bytes,
                               (off_t)((src + off) * block_size));
            if(rd < (ssize_t)bytes) return OBMAFS3_ERR_IO;

            ssize_t wr = pwrite(ctx->fd, g_io_buf, bytes,
                                (off_t)((dst + off) * block_size));
            if(wr < (ssize_t)bytes) return OBMAFS3_ERR_IO;
        }
        return OBMAFS3_OK;
    }

    ssize_t rd = pread(ctx->fd, g_io_buf, total_bytes,
                       (off_t)(src * block_size));
    if(rd < (ssize_t)total_bytes) return OBMAFS3_ERR_IO;

    ssize_t wr = pwrite(ctx->fd, g_io_buf, total_bytes,
                        (off_t)(dst * block_size));
    if(wr < (ssize_t)total_bytes) return OBMAFS3_ERR_IO;

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

/** Find a free LBA near the end of the disk for eviction targets. */
static uint64_t find_free_at_end(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t total = ctx->sb.total_bytes / ctx->sb.block_size;

    /* Search backwards from the end (before backup superblock) */
    for(uint64_t lba = total - 2; lba > 0; lba--)
    {
        if(!obmafs3_bitmap_is_set(ctx, lba))
            return lba;
    }
    return 0; /* should not happen on a non-full disk */
}

/**
 * Advance the write cursor to the next position suitable for a data block.
 * If the position is occupied by a non-data, non-meta block (i.e. a tree
 * node or dedup block), evict it to a free spot near the end of the disk
 * first, making room for the data block.
 */
static uint64_t next_data_slot(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *block_types = state->analysis->block_types;
    uint64_t total = ctx->sb.total_bytes / ctx->sb.block_size;

    while(state->write_cursor < total)
    {
        uint64_t pos = state->write_cursor;

        /* If the position is free in the bitmap, we can use it */
        if(!obmafs3_bitmap_is_set(ctx, pos))
            return pos;

        /* If it's already a data block (BT_USED) or metadata, it's in
         * place — advance past it. */
        if(block_types[pos] == BT_USED || block_types[pos] == BT_META)
        {
            state->write_cursor++;
            continue;
        }

        /* It's a tree node or dedup block sitting where we want to put
         * data.  Evict it to a free spot near the end of the disk. */
        uint64_t evict_dst = find_free_at_end(state);
        if(evict_dst == 0)
        {
            state->write_cursor++;
            continue;
        }

        atomic_store(&state->current_src_lba, pos);
        atomic_store(&state->current_dst_lba, evict_dst);

        /* Copy the block to its eviction destination */
        int rc = copy_block(state, pos, evict_dst, bsz);
        if(rc != OBMAFS3_OK) { state->write_cursor++; continue; }

        sync_fd(state);

        rc = bitmap_move(state, pos, evict_dst, 1);
        if(rc != OBMAFS3_OK) { state->write_cursor++; continue; }

        block_types[evict_dst] = block_types[pos];
        block_types[pos]       = BT_FREE;

        sync_fd(state);

        return pos;
    }
    return state->write_cursor;
}

/* ------------------------------------------------------------------ */
/*  Phase 1: Move non-dedup data blocks to disk start                  */
/* ------------------------------------------------------------------ */

/**
 * Walk the inode tree, and for each inode move its data extent blocks
 * to the beginning of the disk.  Updates extent_run references in the
 * inode record (and overflow records) atomically.
 *
 * Atomic sequence per extent:
 *   1. Copy block(s) to new location
 *   2. Update extent_run.start_lba in the inode/overflow leaf
 *   3. Update refcount tree (delete old key, insert new key)
 *   4. fdatasync
 *   5. Update bitmap (set new, clear old)
 *   6. Flush bitmap
 */
static int compact_data_blocks(struct compact_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    size_t bsz = (size_t)ctx->sb.block_size;
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
    uint8_t *block_types = state->analysis->block_types;

    /* Start right after the primary superblock at LBA 0.
     * Metadata blocks (bitmap, keyset, pending) can live anywhere
     * on disk — they'll be skipped via BT_META in the loop. */
    state->write_cursor = 1;

    /*
     * Fill gaps: walk the write cursor forward, skipping positions
     * that already have BT_USED or BT_META blocks.  Find the next
     * out-of-place BT_USED block and move it into the gap.
     * Trees are already relocated to the end, so no eviction needed.
     */
    uint64_t scan = state->write_cursor;

    /* Batch size: how many blocks to move before syncing.
     * Larger = faster but more data at risk on crash.
     * 65536 blocks × 4K = 256 MB of data per sync. */
    #define DATA_BATCH_SIZE 65536

    /* Pending bitmap updates: old LBAs to clear, new LBAs to set */
    uint64_t *pending_old = malloc(DATA_BATCH_SIZE * sizeof(uint64_t));
    uint64_t *pending_new = malloc(DATA_BATCH_SIZE * sizeof(uint64_t));
    uint64_t  pending_count = 0;

    if(!pending_old || !pending_new)
    {
        free(pending_old);
        free(pending_new);
        return OBMAFS3_ERR_NOMEM;
    }

    for(;;)
    {
        /* Check for cancellation — flush pending and exit safely */
        if(CANCELLED(state))
        {
            if(pending_count > 0)
            {
                sync_fd(state);
                for(uint64_t p = 0; p < pending_count; p++)
                {
                    obmafs3_bitmap_set(ctx, pending_new[p], 1);
                    obmafs3_bitmap_clear(ctx, pending_old[p], 1);
                }
                obmafs3_bitmap_write(ctx);
                sync_fd(state);
            }
            free(pending_old);
            free(pending_new);
            return OBMAFS3_OK;
        }

        /* Advance write_cursor past metadata blocks (immovable). */
        while(state->write_cursor < total_blocks &&
              block_types[state->write_cursor] == BT_META)
            state->write_cursor++;

        /* If write_cursor already points to a BT_USED block, it's in
         * the right place — just advance past it. */
        if(state->write_cursor < total_blocks &&
           block_types[state->write_cursor] == BT_USED)
        {
            state->write_cursor++;
            continue;
        }

        /* write_cursor is now at a gap (BT_FREE, BT_DEDUP, or BT_TREE).
         * Find the next BT_USED block ANYWHERE beyond write_cursor
         * to fill this gap. */
        if(scan <= state->write_cursor)
            scan = state->write_cursor + 1;

        while(scan < total_blocks && block_types[scan] != BT_USED)
            scan++;

        /* No more data blocks to move? Flush pending and done. */
        if(scan >= total_blocks)
        {
            if(pending_count > 0)
            {
                sync_fd(state);
                for(uint64_t p = 0; p < pending_count; p++)
                {
                    obmafs3_bitmap_set(ctx, pending_new[p], 1);
                    obmafs3_bitmap_clear(ctx, pending_old[p], 1);
                }
                obmafs3_bitmap_write(ctx);
                sync_fd(state);
            }
            break;
        }

        uint64_t dst = state->write_cursor;
        uint64_t src = scan;

        /* If the destination is occupied by a dedup block, evict it.
         * Evictions are always flushed immediately for safety. */
        if(obmafs3_bitmap_is_set(ctx, dst) && block_types[dst] != BT_FREE)
        {
            if(block_types[dst] == BT_DEDUP)
            {
                /* Flush any pending batch first */
                if(pending_count > 0)
                {
                    sync_fd(state);
                    for(uint64_t p = 0; p < pending_count; p++)
                    {
                        obmafs3_bitmap_set(ctx, pending_new[p], 1);
                        obmafs3_bitmap_clear(ctx, pending_old[p], 1);
                    }
                    obmafs3_bitmap_write(ctx);
                    sync_fd(state);
                    pending_count = 0;
                }

                uint64_t evict_dst = find_free_at_end(state);
                if(evict_dst == 0) { state->write_cursor++; continue; }

                atomic_store(&state->current_src_lba, dst);
                atomic_store(&state->current_dst_lba, evict_dst);

                int rc = copy_block(state, dst, evict_dst, bsz);
                if(rc != OBMAFS3_OK) { free(pending_old); free(pending_new); return rc; }
                sync_fd(state);

                rc = bitmap_move(state, dst, evict_dst, 1);
                if(rc != OBMAFS3_OK) { free(pending_old); free(pending_new); return rc; }

                block_types[evict_dst] = BT_DEDUP;
                block_types[dst] = BT_FREE;
                sync_fd(state);
            }
            else
            {
                state->write_cursor++;
                continue;
            }
        }

        /* Now dst is free — move the data block from src to dst */
        atomic_store(&state->current_src_lba, src);
        atomic_store(&state->current_dst_lba, dst);

        int rc = copy_block(state, src, dst, bsz);
        if(rc != OBMAFS3_OK) { free(pending_old); free(pending_new); return rc; }

        /* Update refcount tree if needed */
        uint32_t ref = 0;
        obmafs3_refcount_get(ctx, src, &ref);
        if(ref > 1)
        {
            obmafs3_refcount_set(ctx, dst, ref);
            obmafs3_refcount_set(ctx, src, 0);
        }

        /* Update block_types immediately for the live map */
        block_types[dst] = BT_USED;
        block_types[src] = BT_FREE;

        /* Queue bitmap update */
        pending_old[pending_count] = src;
        pending_new[pending_count] = dst;
        pending_count++;

        state->write_cursor = dst + 1;
        scan = src + 1;
        state->data_blocks_moved++;
        atomic_fetch_add(&state->done_steps, 1);

        /* Flush batch when full */
        if(pending_count >= DATA_BATCH_SIZE)
        {
            sync_fd(state);
            for(uint64_t p = 0; p < pending_count; p++)
            {
                obmafs3_bitmap_set(ctx, pending_new[p], 1);
                obmafs3_bitmap_clear(ctx, pending_old[p], 1);
            }
            obmafs3_bitmap_write(ctx);
            sync_fd(state);
            pending_count = 0;
        }
    }

    free(pending_old);
    free(pending_new);

    #undef DATA_BATCH_SIZE

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

    #define DEDUP_BATCH_SIZE 4096
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

        uint64_t run_len = 1;
        while(scan + run_len < total_blocks &&
              block_types[scan + run_len] == BT_DEDUP &&
              run_len < std_per_dedup)
            run_len++;

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

            for(uint64_t i = 0; i < run_len; i++)
            {
                uint64_t pos = dst + i;
                if(pos >= total_blocks) break;
                if(!obmafs3_bitmap_is_set(ctx, pos)) continue;
                if(block_types[pos] == BT_FREE) continue;

                uint64_t evict_dst = find_free_at_end(state);
                if(evict_dst == 0) continue;

                atomic_store(&state->current_src_lba, pos);
                atomic_store(&state->current_dst_lba, evict_dst);

                int rc = copy_block(state, pos, evict_dst, bsz);
                if(rc != OBMAFS3_OK) continue;
                sync_fd(state);

                rc = bitmap_move(state, pos, evict_dst, 1);
                if(rc != OBMAFS3_OK) continue;

                block_types[evict_dst] = block_types[pos];
                block_types[pos]       = BT_FREE;
                sync_fd(state);
            }
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
            obmafs3_checksum_block(buf, bsz, nhp->checksum);
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

    /* ---- Phase 1: Relocate trees to end of disk first ----
     * This must happen BEFORE data/dedup compaction so that tree nodes
     * are out of the way.  relocate_tree() correctly updates all
     * internal child_lba, sibling, and free-chain pointers. */
    atomic_store(&state->phase, COMPACT_PHASE_TREES);
    rc = compact_trees(state);
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        return rc;
    }
    sync_fd(state);

    if(CANCELLED(state)) goto safe_stop;

    /* ---- Phase 2: Move data blocks to start of disk ---- */
    atomic_store(&state->phase, COMPACT_PHASE_DATA);
    rc = compact_data_blocks(state);
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        return rc;
    }
    sync_fd(state);

    if(CANCELLED(state)) goto safe_stop;

    /* ---- Phase 3: Move dedup blocks after data ---- */
    atomic_store(&state->phase, COMPACT_PHASE_DEDUP);
    rc = compact_dedup_blocks(state);
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        return rc;
    }
    sync_fd(state);

    /* ---- Phase 4: Final flush ---- */
safe_stop:
    atomic_store(&state->phase, COMPACT_PHASE_BITMAP);
    obmafs3_bitmap_write(ctx);
    obmafs3_sb_write(ctx->fd, &ctx->sb);
    sync_fd(state);

    /* ---- Done ---- */
    atomic_store(&state->phase, COMPACT_PHASE_DONE);
    atomic_store(&state->finished, 1);
    return OBMAFS3_OK;
}
