// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_metadata.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Metadata tree bidirectional consistency for obmafsck.
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

#include "fsck.h"

/* ------------------------------------------------------------------ */
/*  Metadata tree bidirectional consistency                             */
/* ------------------------------------------------------------------ */

/**
 * A metadata triple collected from a leaf node.
 * Used for bidirectional comparison between metadata and metadata index trees.
 */
struct meta_triple
{
    uint64_t inode_id;
    char     key[METADATA_KEY_MAX];
    char     value[METADATA_VALUE_MAX];
};

/** qsort comparator for meta_triple: (inode_id, key, value). */
static int cmp_meta_triple(const void *a, const void *b)
{
    const struct meta_triple *ta = (const struct meta_triple *)a;
    const struct meta_triple *tb = (const struct meta_triple *)b;
    if(ta->inode_id < tb->inode_id) return -1;
    if(ta->inode_id > tb->inode_id) return 1;
    int kc = strncmp(ta->key, tb->key, METADATA_KEY_MAX);
    if(kc != 0) return kc;
    return strncmp(ta->value, tb->value, METADATA_VALUE_MAX);
}

/**
 * Walk all metadata tree leaf nodes and collect every (inode_id, key, value) tuple.
 *
 * @param ctx       Filesystem context.
 * @param out       Output: heap-allocated array of meta_triple.
 * @param out_count Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_metadata_records(struct obmafs3_ctx *ctx,
                                    struct meta_triple **out, uint64_t *out_count)
{
    *out       = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->metadata_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_triple *recs = NULL;
    uint64_t count = 0, cap = 0;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(recs); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct metadata_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect metadata records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                struct meta_triple *tmp = realloc(recs, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                recs = tmp;
            }
            struct metadata_record mrec;
            memcpy(&mrec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(mrec), sizeof(mrec));
            recs[count].inode_id = mrec.inode_id;
            memcpy(recs[count].key, mrec.key, METADATA_KEY_MAX);
            memcpy(recs[count].value, mrec.value, METADATA_VALUE_MAX);
            count++;
        }
    }

    free(buf);
    free(stack);
    *out       = recs;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Walk all metadata index tree leaf nodes and collect every (key, value, inode_id) tuple.
 *
 * @param ctx       Filesystem context.
 * @param out       Output: heap-allocated array of meta_triple.
 * @param out_count Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_metadata_idx_records(struct obmafs3_ctx *ctx,
                                        struct meta_triple **out, uint64_t *out_count)
{
    *out       = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->metadata_idx_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_triple *recs = NULL;
    uint64_t count = 0, cap = 0;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(recs); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct metadata_idx_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect metadata index records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                struct meta_triple *tmp = realloc(recs, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                recs = tmp;
            }
            struct metadata_idx_record irec;
            memcpy(&irec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(irec), sizeof(irec));
            recs[count].inode_id = irec.inode_id;
            memcpy(recs[count].key, irec.key, METADATA_KEY_MAX);
            memcpy(recs[count].value, irec.value, METADATA_VALUE_MAX);
            count++;
        }
    }

    free(buf);
    free(stack);
    *out       = recs;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Binary search for a meta_triple in a sorted array.
 *
 * @param arr    Sorted array of meta_triple.
 * @param count  Number of elements.
 * @param t      Triple to search for.
 * @return Non-zero if found.
 */
static int meta_triple_sorted_contains(const struct meta_triple *arr, uint64_t count,
                                       const struct meta_triple *t)
{
    uint64_t lo = 0, hi = count;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = cmp_meta_triple(&arr[mid], t);
        if(c < 0)      lo = mid + 1;
        else if(c > 0)  hi = mid;
        else             return 1;
    }
    return 0;
}

/**
 * Check bidirectional consistency between the metadata tree and the
 * metadata index tree.
 *
 * Detects:
 * - Entries in the metadata tree with no corresponding record in the
 *   metadata index tree.  Fixed by re-inserting via metadata_put.
 * - Entries in the metadata index tree with no corresponding record in
 *   the metadata tree.  Fixed by re-inserting via metadata_put.
 *
 * @param ctx       Filesystem context.
 * @param auto_yes  If nonzero, always repair.
 * @param auto_no   If nonzero, never repair.
 * @param errors    In/out: incremented for each unfixed error.
 */
void check_metadata_bidirectional(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors)
{
    struct meta_triple *meta_recs = NULL, *idx_recs = NULL;
    uint64_t meta_count = 0, idx_count = 0;

    printf("\n  %sMetadata consistency%s\n", CLR_BOLD, CLR_RESET);

    int rc = collect_metadata_records(ctx, &meta_recs, &meta_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Metadata tree:", "could not walk (%d)", rc);
        (*errors)++;
        return;
    }

    rc = collect_metadata_idx_records(ctx, &idx_recs, &idx_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Metadata index:", "could not walk (%d)", rc);
        free(meta_recs);
        (*errors)++;
        return;
    }

    /* Sort both arrays by (inode_id, key, value) */
    if(meta_count > 0) qsort(meta_recs, (size_t)meta_count, sizeof(meta_recs[0]), cmp_meta_triple);
    if(idx_count > 0)  qsort(idx_recs,  (size_t)idx_count,  sizeof(idx_recs[0]),  cmp_meta_triple);

    /* ---- Phase 1: entries in metadata but not in index ---- */
    uint64_t missing_from_idx = 0, fixed_idx = 0;
    for(uint64_t i = 0; i < meta_count; i++)
    {
        if(!meta_triple_sorted_contains(idx_recs, idx_count, &meta_recs[i]))
            missing_from_idx++;
    }

    if(missing_from_idx > 0)
    {
        result_bad("Missing from index:", "%" PRIu64 " record(s)", missing_from_idx);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Re-insert missing entries into metadata index?"))
        {
            for(uint64_t i = 0; i < meta_count; i++)
            {
                if(!meta_triple_sorted_contains(idx_recs, idx_count, &meta_recs[i]))
                {
                    rc = obmafs3_metadata_put(ctx, meta_recs[i].inode_id,
                                              meta_recs[i].key, meta_recs[i].value);
                    if(rc == OBMAFS3_OK)
                        fixed_idx++;
                    else
                        printf("    Error: could not re-insert inode %" PRIu64 " key '%s' (%d)\n",
                               meta_recs[i].inode_id, meta_recs[i].key, rc);
                }
            }
            if(fixed_idx == missing_from_idx)
            {
                result_fixed("Index entries:", "%" PRIu64 " fixed", fixed_idx);
                (*errors)--;
            }
            else
            {
                result_fixed("Index entries:", "%" PRIu64 " of %" PRIu64 " fixed",
                             fixed_idx, missing_from_idx);
            }
        }
    }

    /* ---- Phase 2: entries in index but not in metadata ---- */
    uint64_t missing_from_meta = 0, fixed_meta = 0;
    for(uint64_t i = 0; i < idx_count; i++)
    {
        if(!meta_triple_sorted_contains(meta_recs, meta_count, &idx_recs[i]))
            missing_from_meta++;
    }

    if(missing_from_meta > 0)
    {
        result_bad("Missing from meta:", "%" PRIu64 " record(s)", missing_from_meta);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Re-insert missing entries into metadata tree?"))
        {
            for(uint64_t i = 0; i < idx_count; i++)
            {
                if(!meta_triple_sorted_contains(meta_recs, meta_count, &idx_recs[i]))
                {
                    rc = obmafs3_metadata_put(ctx, idx_recs[i].inode_id,
                                              idx_recs[i].key, idx_recs[i].value);
                    if(rc == OBMAFS3_OK)
                        fixed_meta++;
                    else
                        printf("    Error: could not re-insert inode %" PRIu64 " key '%s' (%d)\n",
                               idx_recs[i].inode_id, idx_recs[i].key, rc);
                }
            }
            if(fixed_meta == missing_from_meta)
            {
                result_fixed("Meta entries:", "%" PRIu64 " fixed", fixed_meta);
                (*errors)--;
            }
            else
            {
                result_fixed("Meta entries:", "%" PRIu64 " of %" PRIu64 " fixed",
                             fixed_meta, missing_from_meta);
            }
        }
    }

    /* ---- Summary ---- */
    if(missing_from_idx == 0 && missing_from_meta == 0)
        result_ok("Status:", "%" PRIu64 " record(s)", meta_count);

    free(meta_recs);
    free(idx_recs);
}

/* ------------------------------------------------------------------ */
