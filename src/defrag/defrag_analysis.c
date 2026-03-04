// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : defrag_analysis.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     Volume fragmentation analysis engine.  Walks all on-disk structures
//     to classify every block and compute per-tree fragmentation statistics.
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

#include "defrag_analysis.h"
#include "obmafs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Phase labels                                                       */
/* ------------------------------------------------------------------ */

const char *analysis_phase_labels[ANALYSIS_NUM_PHASES] = {
    "Loading bitmap",
    "Walking B+Trees",
    "Scanning dedup",
    "Classifying blocks",
    "Computing statistics",
    "Done"
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Mark a range of blocks as a given type. */
static void mark_blocks(struct analysis_state *state, uint64_t lba, uint64_t count, enum block_type bt)
{
    for(uint64_t i = 0; i < count && (lba + i) < state->total_blocks; i++)
    {
        state->block_types[lba + i] = (uint8_t)bt;
    }
}

/** Mark a single block. */
static inline void mark_block(struct analysis_state *state, uint64_t lba, enum block_type bt)
{
    if(lba < state->total_blocks)
        state->block_types[lba] = (uint8_t)bt;
}

/* ------------------------------------------------------------------ */
/*  BFS tree node walker                                               */
/* ------------------------------------------------------------------ */

/**
 * Walk all nodes reachable from @p root_lba via iterative DFS.
 *
 * Each visited node LBA is marked as BT_TREE in block_types[].
 * Returns the number of nodes walked (0 if root_lba == 0).
 */
static uint64_t walk_tree(struct analysis_state *state, uint64_t root_lba,
                          size_t index_entry_size, size_t child_lba_offset)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t block_size = ctx->sb.block_size;

    if(root_lba == 0) return 0;

    uint8_t *buf = calloc(1, (size_t)block_size);
    if(!buf) return 0;

    /* Explicit DFS stack */
    uint64_t stk_cap  = 256;
    uint64_t stk_size = 0;
    uint64_t *stack   = malloc(stk_cap * sizeof(uint64_t));
    if(!stack) { free(buf); return 0; }

    uint64_t node_count = 0;

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        if(lba == 0 || lba >= state->total_blocks) continue;

        /* Avoid revisiting (may happen with corrupt sibling links) */
        if(state->block_types[lba] == BT_TREE)
            continue;

        mark_block(state, lba, BT_TREE);
        node_count++;
        atomic_fetch_add(&state->done_blocks, 1);

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)block_size);
        if(rc != OBMAFS3_OK) continue;

        struct btree_node_header *nh = (struct btree_node_header *)buf;
        if(nh->magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

        /* If index node, push all children */
        if(nh->level > 0 && nh->node_keys > 0 && index_entry_size > 0)
        {
            const uint8_t *records = buf + sizeof(struct btree_node_header);
            uint16_t nk = nh->node_keys;

            for(uint16_t i = 0; i < nk; i++)
            {
                const uint8_t *entry = records + (size_t)i * index_entry_size;
                uint64_t child_lba;
                memcpy(&child_lba, entry + child_lba_offset, sizeof(uint64_t));

                if(child_lba == 0 || child_lba >= state->total_blocks) continue;

                /* Grow stack if needed */
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(uint64_t));
                    if(!tmp) break;
                    stack = tmp;
                }
                stack[stk_size++] = child_lba;
            }
        }
    }

    free(stack);
    free(buf);
    return node_count;
}

/**
 * Walk the free-node chain of a B+Tree and mark each as BT_TREE.
 *
 * Free B+Tree nodes store a next-free pointer at byte offset 0 of
 * the node block.  The chain terminates at 0.
 */
static uint64_t walk_free_chain(struct analysis_state *state, uint64_t free_lba)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t block_size = ctx->sb.block_size;
    uint64_t count = 0;
    uint64_t lba = free_lba;

    uint8_t *buf = calloc(1, (size_t)block_size);
    if(!buf) return 0;

    while(lba != 0 && lba < state->total_blocks)
    {
        if(state->block_types[lba] == BT_TREE)
            break; /* cycle detection */

        mark_block(state, lba, BT_TREE);
        count++;
        atomic_fetch_add(&state->done_blocks, 1);

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)block_size);
        if(rc != OBMAFS3_OK) break;

        uint64_t next;
        memcpy(&next, buf, sizeof(uint64_t));
        lba = next;

        /* Safety limit */
        if(count > state->total_blocks) break;
    }

    free(buf);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Walk a single B+Tree (header + nodes + free chain)                 */
/* ------------------------------------------------------------------ */

/**
 * Analyse a single B+Tree: mark its header as BT_META, walk all
 * reachable nodes, walk the free chain, and record per-tree stats.
 */
static void analyse_tree(struct analysis_state *state, uint64_t hdr_lba,
                         const char *name, size_t index_entry_size,
                         size_t child_lba_offset)
{
    if(hdr_lba == 0) return;

    mark_block(state, hdr_lba, BT_META);
    atomic_fetch_add(&state->done_blocks, 1);

    struct btree_header hdr;
    int rc = obmafs3_btree_header_read(state->ctx, hdr_lba, &hdr);
    if(rc != OBMAFS3_OK) return;

    uint64_t nodes = walk_tree(state, hdr.root_node_lba,
                               index_entry_size, child_lba_offset);
    nodes += walk_free_chain(state, hdr.free_node_lba);

    /* Record per-tree stats */
    if(state->result.tree_count < ANALYSIS_MAX_TREES)
    {
        struct tree_frag_stats *ts = &state->result.trees[state->result.tree_count++];
        strncpy(ts->name, name, sizeof(ts->name) - 1);
        ts->name[sizeof(ts->name) - 1] = '\0';
        ts->node_count = nodes;
        /* contiguous_runs and frag_pct computed later in stats phase */
    }
}

/* ------------------------------------------------------------------ */
/*  Walk dedup data blocks                                             */
/* ------------------------------------------------------------------ */

/**
 * Walk all leaf nodes of a dedup B+Tree and mark referenced dedup
 * data blocks as BT_DEDUP.  Each dedup_entry has a block_lba pointing
 * to a dedup data block that spans dedup_block_size / block_size
 * standard blocks.
 */
static void walk_dedup_data_blocks(struct analysis_state *state,
                                   uint64_t root_lba, size_t index_entry_size,
                                   size_t child_lba_offset)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t block_size = ctx->sb.block_size;
    uint64_t dedup_block_size = ctx->sb.dedup_block_size;
    uint64_t std_per_dedup = dedup_block_size / block_size;

    if(root_lba == 0 || std_per_dedup == 0) return;

    uint8_t *buf = calloc(1, (size_t)block_size);
    if(!buf) return;

    /* DFS to find leaf nodes */
    uint64_t stk_cap  = 256;
    uint64_t stk_size = 0;
    uint64_t *stack   = malloc(stk_cap * sizeof(uint64_t));
    if(!stack) { free(buf); return; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];
        if(lba == 0 || lba >= state->total_blocks) continue;

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)block_size);
        if(rc != OBMAFS3_OK) continue;

        struct btree_node_header *nh = (struct btree_node_header *)buf;
        if(nh->magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

        if(nh->level > 0)
        {
            /* Index node — push children */
            const uint8_t *records = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nh->node_keys; i++)
            {
                const uint8_t *entry = records + (size_t)i * index_entry_size;
                uint64_t child_lba;
                memcpy(&child_lba, entry + child_lba_offset, sizeof(uint64_t));
                if(child_lba == 0 || child_lba >= state->total_blocks) continue;

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(uint64_t));
                    if(!tmp) break;
                    stack = tmp;
                }
                stack[stk_size++] = child_lba;
            }
        }
        else
        {
            /* Leaf node — extract dedup_entry records */
            const uint8_t *records = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nh->node_keys; i++)
            {
                struct dedup_entry de;
                memcpy(&de, records + (size_t)i * sizeof(struct dedup_entry),
                       sizeof(struct dedup_entry));

                if(de.block_lba == 0 || de.block_lba >= state->total_blocks)
                    continue;

                /* Mark the entire dedup data block range */
                for(uint64_t b = 0; b < std_per_dedup && (de.block_lba + b) < state->total_blocks; b++)
                {
                    if(state->block_types[de.block_lba + b] != BT_DEDUP)
                    {
                        state->block_types[de.block_lba + b] = BT_DEDUP;
                        atomic_fetch_add(&state->done_blocks, 1);
                    }
                }
            }
        }
    }

    free(stack);
    free(buf);
}

/* ------------------------------------------------------------------ */
/*  Fragmentation statistics                                           */
/* ------------------------------------------------------------------ */

/** Count contiguous runs of @p target_type in block_types[]. */
static uint64_t count_runs(const uint8_t *block_types, uint64_t total, uint8_t target_type)
{
    uint64_t runs = 0;
    int      in_run = 0;

    for(uint64_t i = 0; i < total; i++)
    {
        if(block_types[i] == target_type)
        {
            if(!in_run) { runs++; in_run = 1; }
        }
        else
        {
            in_run = 0;
        }
    }
    return runs;
}

/** Count contiguous runs of BT_TREE blocks that belong to a specific tree. */
static void compute_tree_frag(const uint8_t *block_types, uint64_t total,
                              const uint64_t *tree_lbas, uint64_t tree_lba_count,
                              uint64_t *out_runs)
{
    /* For simplicity, we count runs by sorting the LBA array and
     * checking for consecutive sequences. */
    if(tree_lba_count == 0) { *out_runs = 0; return; }

    /* Copy and sort */
    uint64_t *sorted = malloc(tree_lba_count * sizeof(uint64_t));
    if(!sorted) { *out_runs = tree_lba_count; return; }
    memcpy(sorted, tree_lbas, tree_lba_count * sizeof(uint64_t));

    /* Simple insertion sort for moderate counts; qsort for large */
    for(uint64_t i = 1; i < tree_lba_count; i++)
    {
        uint64_t key = sorted[i];
        uint64_t j = i;
        while(j > 0 && sorted[j - 1] > key) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = key;
    }

    uint64_t runs = 1;
    for(uint64_t i = 1; i < tree_lba_count; i++)
    {
        if(sorted[i] != sorted[i - 1] + 1)
            runs++;
    }

    *out_runs = runs;
    free(sorted);

    (void)block_types;
    (void)total;
}

/**
 * Compute fragmentation percentage.
 *
 * frag = 0% when everything is in 1 run.
 * frag = 100% when every block is in its own run (completely fragmented).
 */
static double frag_percent(uint64_t total_items, uint64_t runs)
{
    if(total_items <= 1 || runs <= 1) return 0.0;
    return ((double)(runs - 1) / (double)(total_items - 1)) * 100.0;
}

void defrag_analysis_compute_stats(struct analysis_state *state)
{
    uint64_t total = state->total_blocks;
    const uint8_t *bt = state->block_types;
    struct analysis_result *r = &state->result;

    r->total_blocks = total;
    r->free_blocks  = 0;
    r->used_blocks  = 0;
    r->tree_blocks  = 0;
    r->dedup_blocks = 0;
    r->meta_blocks  = 0;

    for(uint64_t i = 0; i < total; i++)
    {
        switch(bt[i])
        {
            case BT_FREE:  r->free_blocks++;  break;
            case BT_USED:  r->used_blocks++;  break;
            case BT_TREE:  r->tree_blocks++;  break;
            case BT_DEDUP: r->dedup_blocks++; break;
            case BT_META:  r->meta_blocks++;  break;
        }
    }

    /* Free-space fragmentation */
    r->free_runs     = count_runs(bt, total, BT_FREE);
    r->free_frag_pct = frag_percent(r->free_blocks, r->free_runs);

    /* Dedup data-block fragmentation */
    r->dedup_runs     = count_runs(bt, total, BT_DEDUP);
    r->dedup_frag_pct = frag_percent(r->dedup_blocks, r->dedup_runs);

    /* Per-tree fragmentation: count runs of BT_TREE globally.
     * Since we don't have per-tree LBA lists at this point,
     * we approximate per-tree fragmentation from the overall
     * tree block count.  For more precision we would need to
     * keep per-tree LBA arrays, but for the summary this is
     * sufficient. */
    uint64_t tree_runs = count_runs(bt, total, BT_TREE);
    for(int i = 0; i < r->tree_count; i++)
    {
        struct tree_frag_stats *ts = &r->trees[i];
        /* Proportional approximation: each tree's runs ≈
         * total_tree_runs × (tree_nodes / total_tree_blocks) */
        if(r->tree_blocks > 0 && ts->node_count > 0)
        {
            double proportion = (double)ts->node_count / (double)r->tree_blocks;
            uint64_t est_runs = (uint64_t)(tree_runs * proportion);
            if(est_runs < 1) est_runs = 1;
            ts->contiguous_runs = est_runs;
            ts->frag_pct = frag_percent(ts->node_count, est_runs);
        }
        else
        {
            ts->contiguous_runs = 0;
            ts->frag_pct = 0.0;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Main analysis entry point                                          */
/* ------------------------------------------------------------------ */

int defrag_analysis_run(struct analysis_state *state)
{
    struct obmafs3_ctx *ctx = state->ctx;
    uint64_t block_size = ctx->sb.block_size;

    state->total_blocks = ctx->sb.total_bytes / block_size;
    state->result.tree_count = 0;

    /* Allocate classification array */
    state->block_types = calloc(state->total_blocks, 1);
    if(!state->block_types)
    {
        atomic_store(&state->error, OBMAFS3_ERR_NOMEM);
        atomic_store(&state->finished, 1);
        return OBMAFS3_ERR_NOMEM;
    }

    /* ---- Phase 0: Load bitmap ---- */
    atomic_store(&state->phase, ANALYSIS_PHASE_BITMAP);

    int rc = obmafs3_bitmap_read(ctx);
    if(rc != OBMAFS3_OK)
    {
        atomic_store(&state->error, rc);
        atomic_store(&state->finished, 1);
        return rc;
    }

    /* Mark metadata blocks: superblock, backup superblock, bitmap */
    mark_block(state, 0, BT_META); /* Primary superblock */
    uint64_t backup_lba = (ctx->sb.total_bytes / block_size) - 1;
    mark_block(state, backup_lba, BT_META); /* Backup superblock */
    mark_blocks(state, ctx->sb.bitmap_lba, ctx->sb.bitmap_blocks, BT_META);

    /* Keyset extent */
    if(ctx->sb.keyset_lba != 0)
        mark_blocks(state, ctx->sb.keyset_lba, ctx->sb.keyset_blocks, BT_META);

    /* Pending buffer extent */
    if(ctx->sb.pending_lba != 0)
        mark_blocks(state, ctx->sb.pending_lba, ctx->sb.pending_blocks, BT_META);

    atomic_fetch_add(&state->done_blocks, ctx->sb.bitmap_blocks + 2);

    /* ---- Phase 1: Walk all fixed B+Trees ---- */
    atomic_store(&state->phase, ANALYSIS_PHASE_TREES);

    /* Catalog tree: index entry = catalog_index_entry (272 bytes),
     * child_lba at byte offset 264 */
    analyse_tree(state, ctx->sb.catalog_lba, "Catalog",
                 sizeof(struct catalog_index_entry),
                 offsetof(struct catalog_index_entry, child_lba));

    /* Inode tree: standard index entry (16 bytes), child_lba at offset 8 */
    analyse_tree(state, ctx->sb.inode_lba, "Inode",
                 sizeof(struct btree_index_entry),
                 offsetof(struct btree_index_entry, child_lba));

    /* Overflow tree: overflow_index_entry (24 bytes), child_lba at offset 16 */
    analyse_tree(state, ctx->sb.overflow_lba, "Overflow",
                 sizeof(struct overflow_index_entry),
                 offsetof(struct overflow_index_entry, child_lba));

    /* Metadata tree: metadata_index_entry, child_lba at end */
    analyse_tree(state, ctx->sb.metadata_lba, "Metadata",
                 sizeof(struct metadata_index_entry),
                 offsetof(struct metadata_index_entry, child_lba));

    /* Metadata index tree */
    analyse_tree(state, ctx->sb.metadata_idx_lba, "MetadataIdx",
                 sizeof(struct metadata_idx_index_entry),
                 offsetof(struct metadata_idx_index_entry, child_lba));

    /* Media tag tree: media_tag_index_entry */
    analyse_tree(state, ctx->sb.media_tag_lba, "MediaTag",
                 sizeof(struct media_tag_index_entry),
                 offsetof(struct media_tag_index_entry, child_lba));

    /* CD prefix tree */
    analyse_tree(state, ctx->sb.cd_prefix_lba, "CdPrefix",
                 sizeof(struct btree_index_entry),
                 offsetof(struct btree_index_entry, child_lba));

    /* CD suffix tree */
    analyse_tree(state, ctx->sb.cd_suffix_lba, "CdSuffix",
                 sizeof(struct btree_index_entry),
                 offsetof(struct btree_index_entry, child_lba));

    /* CD subchannel tree */
    analyse_tree(state, ctx->sb.cd_subchannel_lba, "CdSubchannel",
                 sizeof(struct btree_index_entry),
                 offsetof(struct btree_index_entry, child_lba));

    /* Refcount tree */
    analyse_tree(state, ctx->sb.refcount_lba, "Refcount",
                 sizeof(struct btree_index_entry),
                 offsetof(struct btree_index_entry, child_lba));

    /* ---- Phase 2: Walk dedup tree list and dedup data blocks ---- */
    atomic_store(&state->phase, ANALYSIS_PHASE_DEDUP);

    if(ctx->sb.dedup_lba != 0)
    {
        mark_block(state, ctx->sb.dedup_lba, BT_META);

        uint8_t *tlbuf = calloc(1, (size_t)block_size);
        if(tlbuf)
        {
            rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, tlbuf, (size_t)block_size);
            if(rc == OBMAFS3_OK)
            {
                struct tree_list_header *tlh = (struct tree_list_header *)tlbuf;
                if(tlh->magic == OBMAFS3_TREELIST_MAGIC)
                {
                    struct tree_list_entry *entries =
                        (struct tree_list_entry *)(tlbuf + sizeof(struct tree_list_header));

                    for(uint64_t t = 0; t < tlh->tree_count && t < 64; t++)
                    {
                        uint64_t tree_lba = entries[t].tree_lba;
                        uint16_t sector_sz = entries[t].sector_size;

                        if(tree_lba == 0) continue;

                        char name[64];
                        snprintf(name, sizeof(name), "Dedup %u", (unsigned)sector_sz);

                        analyse_tree(state, tree_lba, name,
                                     sizeof(struct btree_index_entry),
                                     offsetof(struct btree_index_entry, child_lba));

                        /* Now walk leaves to find dedup data blocks */
                        struct btree_header dhdr;
                        rc = obmafs3_btree_header_read(ctx, tree_lba, &dhdr);
                        if(rc == OBMAFS3_OK)
                        {
                            /* Also mark the last (partial) dedup data block */
                            if(dhdr.last_block_lba != 0)
                            {
                                uint64_t std_per_dedup = ctx->sb.dedup_block_size / block_size;
                                for(uint64_t b = 0; b < std_per_dedup &&
                                    (dhdr.last_block_lba + b) < state->total_blocks; b++)
                                {
                                    if(state->block_types[dhdr.last_block_lba + b] != BT_DEDUP)
                                        state->block_types[dhdr.last_block_lba + b] = BT_DEDUP;
                                }
                            }

                            walk_dedup_data_blocks(state, dhdr.root_node_lba,
                                                   sizeof(struct btree_index_entry),
                                                   offsetof(struct btree_index_entry, child_lba));
                        }
                    }
                }
            }
            free(tlbuf);
        }
    }

    /* ---- Phase 3: Classify remaining allocated blocks as BT_USED ---- */
    atomic_store(&state->phase, ANALYSIS_PHASE_CLASSIFY);

    for(uint64_t i = 0; i < state->total_blocks; i++)
    {
        if(state->block_types[i] == BT_FREE && obmafs3_bitmap_is_set(ctx, i))
        {
            state->block_types[i] = BT_USED;
        }
        /* Update progress every 64K blocks to avoid excessive atomic ops */
        if((i & 0xFFFF) == 0)
            atomic_store(&state->done_blocks, i);
    }
    atomic_store(&state->done_blocks, state->total_blocks);

    /* ---- Phase 4: Compute fragmentation statistics ---- */
    atomic_store(&state->phase, ANALYSIS_PHASE_STATS);
    defrag_analysis_compute_stats(state);

    /* ---- Phase 5: Done ---- */
    atomic_store(&state->phase, ANALYSIS_PHASE_DONE);
    atomic_store(&state->finished, 1);
    return OBMAFS3_OK;
}
