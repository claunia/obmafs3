// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_btree.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     B+Tree walking, checksum verification, ordering validation, free-node
//     chain, sibling-link repair for obmafsck.
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

/* Context for qsort comparator (file-scope; obmafsck is single-threaded). */
static int  s_ord_key_type;
static int  s_ord_is_leaf;

/** qsort comparator that delegates to ordering_key_cmp(). */
static int ordering_qsort_cmp(const void *a, const void *b)
{
    return ordering_key_cmp((const uint8_t *)a, (const uint8_t *)b, s_ord_key_type, s_ord_is_leaf);
}

/* ------------------------------------------------------------------ */
/*  Walk all nodes in a single-block B+Tree (DFS)                      */
/*  index_entry_size / child_lba_off parameterize the index entry.     */
/* ------------------------------------------------------------------ */

/**
 * Walk all nodes in a single-block B+Tree via iterative DFS.
 *
 * Collects the LBA of every node (both index and leaf) reachable
 * from @p root_lba.  The caller provides the index entry size and the
 * byte offset of the @c child_lba field to correctly parse index nodes
 * of different tree types.
 *
 * @param ctx              Filesystem context.
 * @param root_lba         Root node LBA of the tree.
 * @param index_entry_size Size in bytes of each index entry.
 * @param child_lba_off    Byte offset of the @c child_lba field in
 *                         the index entry structure.
 * @param out_lbas         Output: heap-allocated array of node LBAs.
 * @param out_count        Output: number of elements in @p out_lbas.
 * @param total_nodes      Expected total nodes (for progress; 0 to disable).
 * @param label            Tree label for progress display (NULL to disable).
 * @return @c OBMAFS3_OK on success.
 */
int walk_inode_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                                  size_t child_lba_off, uint64_t **out_lbas, uint64_t *out_count,
                                  uint32_t total_nodes, const char *label)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(root_lba == 0) return OBMAFS3_OK;

    int show_progress = (total_nodes > 10 && label != NULL);

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Iterative DFS via explicit stack */
    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    if(show_progress) fflush(stdout); /* ensure prior output appears before progress */

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        /* Grow output array */
        if(count >= cap)
        {
            cap           = cap == 0 ? 64 : cap * 2;
            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
            if(!tmp)
            {
                free(buf);
                free(stack);
                free(lbas);
                return OBMAFS3_ERR_NOMEM;
            }
            lbas = tmp;
        }
        lbas[count++] = lba;

        if(show_progress && (count <= 1 || (count & 0xFF) == 0 || count == (uint64_t)total_nodes))
        {
            char pfx[64];
            snprintf(pfx, sizeof(pfx), "Walking %s tree", label);
            print_bar(pfx, count, (uint64_t)total_nodes);
        }

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(hdr.level > 0)
        {
            /* Index node: push children onto stack */
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                uint64_t child_lba;
                memcpy(&child_lba,
                       buf + sizeof(struct btree_node_header) + (size_t)i * index_entry_size + child_lba_off,
                       sizeof(child_lba));

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = child_lba;
            }
        }
    }

    free(buf);
    free(stack);

    if(show_progress)
    {
        bar_clear();
    }

    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Walk all nodes in the catalog B+Tree (DFS)                         */
/*  Uses catalog_index_entry instead of btree_index_entry.             */
/* ------------------------------------------------------------------ */

/**
 * Walk all nodes in the catalog B+Tree via iterative DFS.
 *
 * Uses @c catalog_index_entry (rather than @c btree_index_entry) to
 * decode index node children.
 *
 * @param ctx        Filesystem context.
 * @param root_lba   Root node LBA of the catalog tree.
 * @param out_lbas   Output: heap-allocated array of node LBAs.
 * @param out_count  Output: number of elements in @p out_lbas.
 * @return @c OBMAFS3_OK on success.
 */
int walk_catalog_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, uint64_t **out_lbas,
                                    uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(root_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Iterative DFS via explicit stack */
    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        /* Grow output array */
        if(count >= cap)
        {
            cap           = cap == 0 ? 64 : cap * 2;
            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
            if(!tmp)
            {
                free(buf);
                free(stack);
                free(lbas);
                return OBMAFS3_ERR_NOMEM;
            }
            lbas = tmp;
        }
        lbas[count++] = lba;

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(hdr.level > 0)
        {
            /* Index node: push children (catalog_index_entry) */
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct catalog_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Verify checksums for a list of B+Tree node LBAs                    */
/* ------------------------------------------------------------------ */

/**
 * Verify stored checksums of a list of B+Tree node blocks.
 *
 * Reads each node, recomputes its checksum, and compares it with the
 * stored value.  Reports mismatches to stderr.  Optionally rewrites
 * nodes with corrected checksums.
 *
 * @param ctx          Filesystem context.
 * @param node_lbas    Array of node LBAs to verify.
 * @param node_count   Number of elements in @p node_lbas.
 * @param tree_name    Human-readable tree name for diagnostic output.
 * @param auto_yes     If non-zero, always repair without asking.
 * @param auto_no      If non-zero, never repair.
 * @param bad_count    Output: number of nodes with bad checksums.
 * @param fixed_count  Output: number of nodes whose checksums were repaired.
 * @return @c OBMAFS3_OK on success.
 */
int verify_btree_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                       const char *tree_name, int auto_yes, int auto_no, uint64_t *bad_count,
                                       uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Collect LBAs of nodes with bad checksums */
    uint64_t *bad_lbas = NULL;
    uint64_t  bad_cap  = 0;
    uint64_t  bad      = 0;

    for(uint64_t n = 0; n < node_count; n++)
    {
        if(node_count > 10)
        {
            char pfx[64];
            snprintf(pfx, sizeof(pfx), "Verifying %s nodes", tree_name);
            print_bar(pfx, n + 1, node_count);
        }

        uint64_t lba = node_lbas[n];

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(bad_lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": bad magic\n", tree_name, lba);
            bad++;
            continue;
        }

        size_t  data_size = sizeof(struct btree_node_header) + hdr.keys_length;
        uint8_t stored[32];
        memcpy(stored, hdr.checksum, 32);
        memset(buf + __builtin_offsetof(struct btree_node_header, checksum), 0, 32);
        uint8_t computed[32];
        obmafs3_checksum_block(buf, data_size, computed);
        memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), stored, 32);

        if(memcmp(stored, computed, 32) != 0)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": checksum mismatch\n", tree_name, lba);
            if(bad >= bad_cap)
            {
                bad_cap = bad_cap ? bad_cap * 2 : 16;
                uint64_t *tmp = realloc(bad_lbas, bad_cap * sizeof(uint64_t));
                if(!tmp) { free(buf); free(bad_lbas); return OBMAFS3_ERR_NOMEM; }
                bad_lbas = tmp;
            }
            bad_lbas[bad] = lba;
            bad++;
        }
    }

    if(node_count > 10)
    {
        bar_clear();
    }

    /* Offer to fix */
    uint64_t fixes = 0;
    if(bad > 0)
    {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "  Fix %" PRIu64 " %s node checksum%s?", bad, tree_name,
                 bad == 1 ? "" : "s");
        if(ask_fix(auto_yes, auto_no, prompt))
        {
            for(uint64_t i = 0; i < bad; i++)
            {
                int rc = obmafs3_block_read(ctx, bad_lbas[i], buf, (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK) continue;

                struct btree_node_header hdr;
                memcpy(&hdr, buf, sizeof(hdr));
                size_t data_size = sizeof(struct btree_node_header) + hdr.keys_length;
                memset(hdr.checksum, 0, 32);
                memcpy(buf, &hdr, sizeof(hdr));
                obmafs3_checksum_block(buf, data_size, hdr.checksum);
                memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr.checksum, 32);

                rc = obmafs3_block_write(ctx, bad_lbas[i], buf, (size_t)ctx->sb.block_size);
                if(rc == OBMAFS3_OK)
                    fixes++;
                else
                    fprintf(stderr, "    Error writing LBA %" PRIu64 ": %d\n", bad_lbas[i], rc);
            }
        }
    }

    free(buf);
    free(bad_lbas);
    *bad_count   = bad;
    *fixed_count = fixes;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  B+Tree key ordering validation                                     */
/* ------------------------------------------------------------------ */

/** Ordering tree-type identifiers for verify_btree_ordering(). */

/**
 * Compare two B+Tree keys extracted from raw record/entry bytes.
 *
 * @param a       Pointer to first record or index entry.
 * @param b       Pointer to second record or index entry.
 * @param type    One of the ORD_* tree-type constants.
 * @param is_leaf Non-zero for leaf records, zero for index entries.
 * @return Negative if a < b, 0 if equal, positive if a > b.
 */
int ordering_key_cmp(const uint8_t *a, const uint8_t *b, int type, int is_leaf)
{
    uint64_t ua, ub;

    switch(type)
    {
    case ORD_UINT64_KEY:
        memcpy(&ua, a, 8);
        memcpy(&ub, b, 8);
        return (ua < ub) ? -1 : (ua > ub) ? 1 : 0;

    case ORD_CATALOG:
        if(is_leaf)
        {
            /* catalog_record: inode_id(8), parent_id(8), directory_flag(1), name[256] */
            uint64_t pid_a, pid_b;
            memcpy(&pid_a, a + 8, 8);
            memcpy(&pid_b, b + 8, 8);
            if(pid_a != pid_b) return (pid_a < pid_b) ? -1 : 1;
            return strcmp((const char *)(a + 17), (const char *)(b + 17));
        }
        else
        {
            /* catalog_index_entry: parent_id(8), name[256], child_lba(8) */
            uint64_t pid_a, pid_b;
            memcpy(&pid_a, a, 8);
            memcpy(&pid_b, b, 8);
            if(pid_a != pid_b) return (pid_a < pid_b) ? -1 : 1;
            return strcmp((const char *)(a + 8), (const char *)(b + 8));
        }

    case ORD_OVERFLOW:
        if(is_leaf)
        {
            /* overflow_extent: inode_id(8), logical_offset(8), ... */
            uint64_t id_a, id_b;
            memcpy(&id_a, a, 8);
            memcpy(&id_b, b, 8);
            if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
            uint64_t off_a, off_b;
            memcpy(&off_a, a + 8, 8);
            memcpy(&off_b, b + 8, 8);
            return (off_a < off_b) ? -1 : (off_a > off_b) ? 1 : 0;
        }
        else
        {
            /* overflow_index_entry: inode_id(8), logical_offset(8), child_lba(8) */
            uint64_t id_a, id_b;
            memcpy(&id_a, a, 8);
            memcpy(&id_b, b, 8);
            if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
            uint64_t off_a, off_b;
            memcpy(&off_a, a + 8, 8);
            memcpy(&off_b, b + 8, 8);
            return (off_a < off_b) ? -1 : (off_a > off_b) ? 1 : 0;
        }

    case ORD_MEDIA_TAG:
    {
        /* Both leaf and index begin with inode_id(8), tag_type(2) */
        uint64_t id_a, id_b;
        memcpy(&id_a, a, 8);
        memcpy(&id_b, b, 8);
        if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
        uint16_t ta, tb;
        memcpy(&ta, a + 8, 2);
        memcpy(&tb, b + 8, 2);
        return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
    }

    case ORD_METADATA:
        if(is_leaf)
        {
            /* metadata_record: inode_id(8), key[256], value[1025] */
            uint64_t id_a, id_b;
            memcpy(&id_a, a, 8);
            memcpy(&id_b, b, 8);
            if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
            return strncmp((const char *)(a + 8), (const char *)(b + 8), METADATA_KEY_MAX);
        }
        else
        {
            /* metadata_index_entry: inode_id(8), key[256], child_lba(8) */
            uint64_t id_a, id_b;
            memcpy(&id_a, a, 8);
            memcpy(&id_b, b, 8);
            if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
            return strncmp((const char *)(a + 8), (const char *)(b + 8), METADATA_KEY_MAX);
        }

    case ORD_METADATA_IDX:
        if(is_leaf)
        {
            /* metadata_idx_record: key[256], value[1025], inode_id(8) */
            int r = strncmp((const char *)a, (const char *)b, METADATA_KEY_MAX);
            if(r != 0) return r;
            r = strncmp((const char *)(a + METADATA_KEY_MAX), (const char *)(b + METADATA_KEY_MAX),
                        METADATA_VALUE_MAX);
            if(r != 0) return r;
            uint64_t id_a, id_b;
            memcpy(&id_a, a + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            memcpy(&id_b, b + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            return (id_a < id_b) ? -1 : (id_a > id_b) ? 1 : 0;
        }
        else
        {
            /* metadata_idx_index_entry: key[256], value[1025], inode_id(8), child_lba(8) */
            int r = strncmp((const char *)a, (const char *)b, METADATA_KEY_MAX);
            if(r != 0) return r;
            r = strncmp((const char *)(a + METADATA_KEY_MAX), (const char *)(b + METADATA_KEY_MAX),
                        METADATA_VALUE_MAX);
            if(r != 0) return r;
            uint64_t id_a, id_b;
            memcpy(&id_a, a + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            memcpy(&id_b, b + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            return (id_a < id_b) ? -1 : (id_a > id_b) ? 1 : 0;
        }

    default:
        return 0;
    }
}

/* Context for qsort comparator (file-scope; obmafsck is single-threaded). */

/** qsort comparator that delegates to ordering_key_cmp(). */

/**
 * Sort records within a node buffer, recompute the checksum, and write
 * the corrected node back to disk.
 *
 * @param ctx       Filesystem context.
 * @param buf       Node buffer (single block or multi-block).
 * @param buf_size  Total size of @p buf in bytes.
 * @param lba       LBA of the node (first block for multi-block).
 * @param nblocks   Number of blocks the node spans (1 for normal trees).
 * @param hdr       Parsed node header.
 * @param key_type  One of the ORD_* tree-type constants.
 * @param is_leaf   Non-zero for leaf nodes.
 * @param stride    Record/entry size in bytes.
 * @return @c OBMAFS3_OK on successful write.
 */
int fix_node_ordering(struct obmafs3_ctx *ctx, uint8_t *buf, size_t buf_size, uint64_t lba, int nblocks,
                             struct btree_node_header *hdr, int key_type, int is_leaf, size_t stride)
{
    /* Set file-scope qsort context */
    s_ord_key_type = key_type;
    s_ord_is_leaf  = is_leaf;

    /* Sort the records in-place */
    uint8_t *entries = buf + sizeof(struct btree_node_header);
    qsort(entries, hdr->node_keys, stride, ordering_qsort_cmp);

    /* Recompute checksum over header+keys only */
    size_t cs_data_size = sizeof(struct btree_node_header) + hdr->keys_length;
    memset(hdr->checksum, 0, 32);
    memcpy(buf, hdr, sizeof(*hdr));
    obmafs3_checksum_block(buf, cs_data_size, hdr->checksum);
    memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr->checksum, 32);

    /* Write back */
    for(int b = 0; b < nblocks; b++)
    {
        int rc = obmafs3_block_write(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                     (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;
    }
    return OBMAFS3_OK;
}

/**
 * Compare the header's total_nodes with the actual walked count and
 * optionally repair the header if they disagree.
 *
 * @param ctx        Filesystem context.
 * @param hdr        Pointer to the btree_header (will be modified on fix).
 * @param hdr_lba    LBA of the header block on disk.
 * @param actual     Actual number of nodes discovered by the walk.
 * @param tree_name  Human-readable tree name for messages.
 * @param auto_yes   If non-zero, always repair without asking.
 * @param auto_no    If non-zero, never repair.
 * @param errors     Pointer to the cumulative error counter.
 */
void verify_fix_total_nodes(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba,
                                   uint64_t actual, const char *tree_name, const char *indent, int auto_yes,
                                   int auto_no, int *errors)
{
    if((uint64_t)hdr->total_nodes == actual)
    {
        result_ok("Total nodes:", "%u", hdr->total_nodes);
        return;
    }

    result_bad("Total nodes:", "MISMATCH (header %u, walked %" PRIu64 ")", hdr->total_nodes, actual);
    (*errors)++;

    if(ask_fix(auto_yes, auto_no, "Fix total_nodes in header?"))
    {
        hdr->total_nodes = (uint32_t)actual;
        int rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        if(rc == OBMAFS3_OK)
        {
            result_fixed("Total nodes:", "%" PRIu64, actual);
            (*errors)--;
        }
        else
        {
            fprintf(stderr, "%sError writing %s header: %d\n", indent, tree_name, rc);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Free node chain consistency                                        */
/* ------------------------------------------------------------------ */

/**
 * Walk the free-node chain starting at @c hdr->free_node_lba and verify
 * that the declared @c free_nodes count matches the actual chain length,
 * that every LBA is within filesystem bounds, and that the chain
 * terminates (no cycles).
 *
 * Each free node stores a uint64_t "next" pointer at byte offset 0.
 * The chain ends when next == 0.
 *
 * @param ctx        Filesystem context.
 * @param hdr        Pointer to the btree_header (will be modified on fix).
 * @param hdr_lba    LBA of the header block on disk.
 * @param tree_name  Human-readable tree name for messages.
 * @param indent     Indentation prefix for output lines.
 * @param auto_yes   If non-zero, always repair without asking.
 * @param auto_no    If non-zero, never repair.
 * @param errors     Pointer to the cumulative error counter.
 */
void verify_fix_free_nodes(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba,
                                  const char *tree_name, const char *indent, int auto_yes, int auto_no, int *errors)
{
    if(hdr->free_node_lba == 0 && hdr->free_nodes == 0)
    {
        result_ok("Free node chain:", "");
        return;
    }

    /* Both must be zero together or both non-zero */
    if((hdr->free_node_lba == 0) != (hdr->free_nodes == 0))
    {
        result_bad("Free node chain:",
                   "inconsistent: free_node_lba=%" PRIu64 ", free_nodes=%u",
                   hdr->free_node_lba, hdr->free_nodes);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "Reset free node chain in header?"))
        {
            hdr->free_node_lba = 0;
            hdr->free_nodes    = 0;
            int rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
            if(rc == OBMAFS3_OK)
            {
                result_fixed("Free node chain:", "reset to 0");
                (*errors)--;
            }
            else
                fprintf(stderr, "%sError writing %s header: %d\n", indent, tree_name, rc);
        }
        return;
    }

    /* Walk the chain and count nodes */
    size_t   bsz       = (size_t)ctx->sb.block_size;
    uint64_t max_lba   = ctx->sb.total_bytes / ctx->sb.block_size;
    uint8_t *buf       = calloc(1, bsz);
    if(!buf)
    {
        result_bad("Free node chain:", "out of memory");
        (*errors)++;
        return;
    }

    uint32_t walked    = 0;
    uint64_t cur       = hdr->free_node_lba;
    int      chain_ok  = 1;

    while(cur != 0)
    {
        if(cur >= max_lba)
        {
            result_bad("Free node chain:",
                       "LBA %" PRIu64 " out of bounds (max %" PRIu64 ") at position %u",
                       cur, max_lba - 1, walked);
            chain_ok = 0;
            break;
        }

        if(walked >= hdr->free_nodes + 1)
        {
            /* More nodes than declared — likely a cycle */
            result_bad("Free node chain:",
                       "chain longer than declared free_nodes=%u (possible cycle)",
                       hdr->free_nodes);
            chain_ok = 0;
            break;
        }

        int rc = obmafs3_block_read(ctx, cur, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            result_bad("Free node chain:",
                       "read error at LBA %" PRIu64 " (position %u): %d",
                       cur, walked, rc);
            chain_ok = 0;
            break;
        }

        uint64_t next;
        memcpy(&next, buf, sizeof(next));
        walked++;
        cur = next;
    }

    free(buf);

    if(chain_ok && walked == hdr->free_nodes)
    {
        result_ok("Free node chain:",
                  "free_node_lba=%" PRIu64 ", free_nodes=%u",
                  hdr->free_node_lba, hdr->free_nodes);
        return;
    }

    if(chain_ok && walked != hdr->free_nodes)
    {
        result_bad("Free node chain:",
                   "declared free_nodes=%u but walked %u",
                   hdr->free_nodes, walked);
    }

    (*errors)++;

    if(ask_fix(auto_yes, auto_no, "Reset free node chain in header?"))
    {
        hdr->free_node_lba = 0;
        hdr->free_nodes    = 0;
        int rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        if(rc == OBMAFS3_OK)
        {
            result_fixed("Free node chain:", "reset to 0");
            (*errors)--;
        }
        else
            fprintf(stderr, "%sError writing %s header: %d\n", indent, tree_name, rc);
    }
}

/* ------------------------------------------------------------------ */
/*  Sibling-link consistency                                           */
/* ------------------------------------------------------------------ */

/**
 * Patch a single node's left_link and/or right_link on disk.
 *
 * Reads the node, updates the header, recomputes the checksum, and
 * writes back.
 *
 * @param ctx         Filesystem context.
 * @param lba         LBA of the node to patch.
 * @param new_left    New left_link value.
 * @param new_right   New right_link value.
 * @param nblocks     Number of contiguous blocks per node (1 or METADATA_NODE_BLOCKS).
 * @return @c OBMAFS3_OK on success.
 */
int patch_sibling_links(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t new_left, uint64_t new_right,
                               int nblocks)
{
    size_t   node_sz = (size_t)nblocks * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    for(int b = 0; b < nblocks; b++)
    {
        int rc = obmafs3_block_read(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                    (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
    }

    struct btree_node_header hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    hdr.left_link  = new_left;
    hdr.right_link = new_right;

    /* Zero checksum, copy header into buffer, recompute over header+keys only */
    size_t data_size = sizeof(struct btree_node_header) + hdr.keys_length;
    memset(hdr.checksum, 0, 32);
    memcpy(buf, &hdr, sizeof(hdr));
    obmafs3_checksum_block(buf, data_size, hdr.checksum);
    memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr.checksum, 32);

    for(int b = 0; b < nblocks; b++)
    {
        int rc = obmafs3_block_write(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                     (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
    }
    free(buf);
    return OBMAFS3_OK;
}

/**
 * Verify (and optionally repair) sibling-link consistency for a B+Tree.
 *
 * Performs a BFS from the root so that nodes at each level are
 * encountered in left-to-right order.  For every adjacent pair
 * (A, B) at the same level the check verifies:
 *   - A.right_link == LBA(B)
 *   - B.left_link  == LBA(A)
 *
 * The leftmost node must have left_link == 0 and the rightmost must
 * have right_link == 0.
 *
 * @param ctx              Filesystem context.
 * @param root_lba         Root node LBA.
 * @param index_entry_size sizeof() of the index entry structure.
 * @param child_lba_off    Offset within the index entry to the child_lba field.
 * @param nblocks          Blocks per node (1 for normal trees, METADATA_NODE_BLOCKS for metadata).
 * @param tree_name        Human-readable tree name.
 * @param auto_yes         If non-zero, always repair without asking.
 * @param auto_no          If non-zero, never repair.
 * @param bad_count        Output: number of link violations found.
 * @param fixed_count      Output: number of nodes whose links were repaired.
 * @return @c OBMAFS3_OK on success (even if violations were found).
 */
int verify_fix_sibling_links(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                                    size_t child_lba_off, int nblocks, const char *tree_name, int auto_yes,
                                    int auto_no, uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)nblocks * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /*
     * BFS queue.  Each entry is an LBA.  Because BFS visits parents
     * before children and enumerates children left-to-right, nodes at
     * each level appear in the correct left-to-right order.
     */
    uint64_t *queue    = malloc(64 * sizeof(uint64_t));
    uint64_t  q_head   = 0;
    uint64_t  q_tail   = 0;
    uint64_t  q_cap    = 64;
    if(!queue) { free(buf); return OBMAFS3_ERR_NOMEM; }

    /* Per-node metadata gathered during BFS */
    typedef struct
    {
        uint64_t lba;
        uint64_t left_link;
        uint64_t right_link;
        uint8_t  level;
    } node_info_t;

    node_info_t *infos    = malloc(64 * sizeof(node_info_t));
    uint64_t     info_cnt = 0;
    uint64_t     info_cap = 64;
    if(!infos) { free(buf); free(queue); return OBMAFS3_ERR_NOMEM; }

    /* Enqueue root */
    queue[q_tail++] = root_lba;

    while(q_head < q_tail)
    {
        uint64_t lba = queue[q_head++];

        /* Read node */
        int read_ok = 1;
        for(int b = 0; b < nblocks; b++)
        {
            int rc = obmafs3_block_read(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                        (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) { read_ok = 0; break; }
        }
        if(!read_ok) continue;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

        /* Record node info */
        if(info_cnt >= info_cap)
        {
            info_cap *= 2;
            node_info_t *tmp = realloc(infos, info_cap * sizeof(node_info_t));
            if(!tmp) { free(buf); free(queue); free(infos); return OBMAFS3_ERR_NOMEM; }
            infos = tmp;
        }
        infos[info_cnt++] = (node_info_t){lba, hdr.left_link, hdr.right_link, hdr.level};

        /* If index node, enqueue children left-to-right */
        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                uint64_t child_lba;
                memcpy(&child_lba,
                       buf + sizeof(struct btree_node_header) + (size_t)i * index_entry_size + child_lba_off,
                       sizeof(child_lba));

                if(q_tail >= q_cap)
                {
                    q_cap *= 2;
                    uint64_t *tmp = realloc(queue, q_cap * sizeof(uint64_t));
                    if(!tmp) { free(buf); free(queue); free(infos); return OBMAFS3_ERR_NOMEM; }
                    queue = tmp;
                }
                queue[q_tail++] = child_lba;
            }
        }
    }

    free(buf);
    free(queue);

    /*
     * infos[] now contains nodes in BFS order.  Within the same level
     * they are already in left-to-right order.  Walk the array and
     * verify the doubly-linked list for each level.
     */
    uint64_t violations = 0;
    uint64_t fixes      = 0;

    uint64_t run_start = 0;
    while(run_start < info_cnt)
    {
        uint8_t  cur_level = infos[run_start].level;
        uint64_t run_end   = run_start + 1;
        while(run_end < info_cnt && infos[run_end].level == cur_level)
            run_end++;

        /* run_start..run_end-1 are all nodes at cur_level in L→R order */
        uint64_t run_len = run_end - run_start;

        int level_bad = 0; /* any violation at this level? */

        for(uint64_t i = run_start; i < run_end; i++)
        {
            uint64_t expect_left  = (i == run_start) ? 0 : infos[i - 1].lba;
            uint64_t expect_right = (i == run_end - 1) ? 0 : infos[i + 1].lba;

            if(infos[i].left_link != expect_left || infos[i].right_link != expect_right)
            {
                if(!level_bad)
                    fprintf(stderr,
                            "\n  %s: sibling-link violations at level %u (%" PRIu64 " node%s):\n",
                            tree_name, cur_level, run_len, run_len == 1 ? "" : "s");
                level_bad = 1;
                violations++;

                fprintf(stderr,
                        "    LBA %" PRIu64 ": left=%" PRIu64 " (expect %" PRIu64 "), "
                        "right=%" PRIu64 " (expect %" PRIu64 ")\n",
                        infos[i].lba, infos[i].left_link, expect_left, infos[i].right_link, expect_right);
            }
        }

        if(level_bad)
        {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "  Fix sibling links at %s level %u?", tree_name, cur_level);
            if(ask_fix(auto_yes, auto_no, prompt))
            {
                for(uint64_t i = run_start; i < run_end; i++)
                {
                    uint64_t expect_left  = (i == run_start) ? 0 : infos[i - 1].lba;
                    uint64_t expect_right = (i == run_end - 1) ? 0 : infos[i + 1].lba;

                    if(infos[i].left_link != expect_left || infos[i].right_link != expect_right)
                    {
                        int rc = patch_sibling_links(ctx, infos[i].lba, expect_left, expect_right, nblocks);
                        if(rc == OBMAFS3_OK)
                            fixes++;
                        else
                            fprintf(stderr, "    Error patching LBA %" PRIu64 ": %d\n", infos[i].lba, rc);
                    }
                }
            }
        }

        run_start = run_end;
    }

    free(infos);

    *bad_count   = violations;
    *fixed_count = fixes;
    return OBMAFS3_OK;
}

/**
 * Verify that keys within every node of a single-block B+Tree are
 * strictly ascending (leaf) or non-decreasing (index).
 * Optionally repairs misordered nodes by sorting records in-place.
 *
 * @param ctx              Filesystem context.
 * @param node_lbas        Array of node LBAs collected by a walk function.
 * @param node_count       Number of node LBAs.
 * @param key_type         One of the ORD_* tree-type constants.
 * @param leaf_rec_size    sizeof() of the leaf record structure.
 * @param idx_entry_size   sizeof() of the index entry structure.
 * @param tree_name        Human-readable tree name for progress/error messages.
 * @param auto_yes         If non-zero, always repair without asking.
 * @param auto_no          If non-zero, never repair.
 * @param bad_count        Output: number of nodes with ordering violations.
 * @param fixed_count      Output: number of nodes successfully repaired.
 * @return @c OBMAFS3_OK on success (even if ordering errors were found).
 */
int verify_btree_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                 int key_type, size_t leaf_rec_size, size_t idx_entry_size, const char *tree_name,
                                 int auto_yes, int auto_no, uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    for(uint64_t n = 0; n < node_count; n++)
    {
        if(node_count > 10 && (n == 0 || (n & 0xFF) == 0 || n == node_count - 1))
        {
            char pfx[80];
            snprintf(pfx, sizeof(pfx), "Ordering %s", tree_name);
            print_bar(pfx, n + 1, node_count);
        }

        int rc = obmafs3_block_read(ctx, node_lbas[n], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) continue;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;
        if(hdr.node_keys < 2) continue; /* 0 or 1 keys — nothing to compare */

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            is_leaf = (hdr.level == 0);
        size_t         stride  = is_leaf ? leaf_rec_size : idx_entry_size;

        /* Sanity: ensure records fit within the block */
        size_t avail = (size_t)ctx->sb.block_size - sizeof(struct btree_node_header);
        if((size_t)hdr.node_keys * stride > avail)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": record count (%u) exceeds block capacity\n", tree_name,
                    node_lbas[n], hdr.node_keys);
            (*bad_count)++;
            continue;
        }

        int misordered = 0;
        for(uint16_t i = 1; i < hdr.node_keys; i++)
        {
            const uint8_t *prev = entries + (size_t)(i - 1) * stride;
            const uint8_t *curr = entries + (size_t)i * stride;
            int            cmp  = ordering_key_cmp(prev, curr, key_type, is_leaf);

            if(is_leaf && cmp >= 0)
            {
                fprintf(stderr, "\n  %s leaf at LBA %" PRIu64 ": keys not strictly ascending at position %u\n",
                        tree_name, node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
            else if(!is_leaf && cmp > 0)
            {
                fprintf(stderr, "\n  %s index at LBA %" PRIu64 ": keys not in order at position %u\n", tree_name,
                        node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
        }

        if(misordered)
        {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "  Sort %s node at LBA %" PRIu64 "?", tree_name, node_lbas[n]);
            if(ask_fix(auto_yes, auto_no, prompt))
            {
                rc = fix_node_ordering(ctx, buf, (size_t)ctx->sb.block_size, node_lbas[n], 1, &hdr, key_type, is_leaf,
                                       stride);
                if(rc == OBMAFS3_OK)
                {
                    printf("  Repaired %s node at LBA %" PRIu64 "\n", tree_name, node_lbas[n]);
                    (*fixed_count)++;
                }
                else
                {
                    fprintf(stderr, "  Error: failed to write repaired node at LBA %" PRIu64 ": %d\n", node_lbas[n],
                            rc);
                }
            }
        }
    }

    if(node_count > 10)
    {
        bar_clear();
    }

    free(buf);
    return OBMAFS3_OK;
}

/**
 * Verify key ordering within every node of a multi-block metadata B+Tree.
 *
 * Each node spans @c METADATA_NODE_BLOCKS contiguous blocks.
 *
 * @param ctx              Filesystem context.
 * @param node_lbas        Array of node start-LBAs collected by walk_meta_btree_nodes().
 * @param node_count       Number of node LBAs.
 * @param key_type         One of ORD_METADATA or ORD_METADATA_IDX.
 * @param leaf_rec_size    sizeof() of the leaf record structure.
 * @param idx_entry_size   sizeof() of the index entry structure.
 * @param tree_name        Human-readable tree name for progress/error messages.
 * @param auto_yes         If non-zero, always repair without asking.
 * @param auto_no          If non-zero, never repair.
 * @param bad_count        Output: number of nodes with ordering violations.
 * @param fixed_count      Output: number of nodes successfully repaired.
 * @return @c OBMAFS3_OK on success (even if ordering errors were found).
 */
int verify_meta_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count, int key_type,
                                size_t leaf_rec_size, size_t idx_entry_size, const char *tree_name, int auto_yes,
                                int auto_no, uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    size_t   node_bytes = (size_t)ctx->sb.block_size * METADATA_NODE_BLOCKS;
    uint8_t *buf        = calloc(1, node_bytes);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    for(uint64_t n = 0; n < node_count; n++)
    {
        if(node_count > 10 && (n == 0 || (n & 0xFF) == 0 || n == node_count - 1))
        {
            char pfx[80];
            snprintf(pfx, sizeof(pfx), "Ordering %s", tree_name);
            print_bar(pfx, n + 1, node_count);
        }

        /* Read all blocks of this multi-block node */
        int ok = 1;
        for(int b = 0; b < METADATA_NODE_BLOCKS; b++)
        {
            int rc = obmafs3_block_read(ctx, node_lbas[n] + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                        (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) { ok = 0; break; }
        }
        if(!ok) continue;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;
        if(hdr.node_keys < 2) continue;

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            is_leaf = (hdr.level == 0);
        size_t         stride  = is_leaf ? leaf_rec_size : idx_entry_size;

        size_t avail = node_bytes - sizeof(struct btree_node_header);
        if((size_t)hdr.node_keys * stride > avail)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": record count (%u) exceeds node capacity\n", tree_name,
                    node_lbas[n], hdr.node_keys);
            (*bad_count)++;
            continue;
        }

        int misordered = 0;
        for(uint16_t i = 1; i < hdr.node_keys; i++)
        {
            const uint8_t *prev = entries + (size_t)(i - 1) * stride;
            const uint8_t *curr = entries + (size_t)i * stride;
            int            cmp  = ordering_key_cmp(prev, curr, key_type, is_leaf);

            if(is_leaf && cmp >= 0)
            {
                fprintf(stderr, "\n  %s leaf at LBA %" PRIu64 ": keys not strictly ascending at position %u\n",
                        tree_name, node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
            else if(!is_leaf && cmp > 0)
            {
                fprintf(stderr, "\n  %s index at LBA %" PRIu64 ": keys not in order at position %u\n", tree_name,
                        node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
        }

        if(misordered)
        {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "  Sort %s node at LBA %" PRIu64 "?", tree_name, node_lbas[n]);
            if(ask_fix(auto_yes, auto_no, prompt))
            {
                int wrc = fix_node_ordering(ctx, buf, node_bytes, node_lbas[n], METADATA_NODE_BLOCKS, &hdr, key_type,
                                            is_leaf, stride);
                if(wrc == OBMAFS3_OK)
                {
                    printf("  Repaired %s node at LBA %" PRIu64 "\n", tree_name, node_lbas[n]);
                    (*fixed_count)++;
                }
                else
                {
                    fprintf(stderr, "  Error: failed to write repaired node at LBA %" PRIu64 ": %d\n", node_lbas[n],
                            wrc);
                }
            }
        }
    }

    if(node_count > 10)
    {
        bar_clear();
    }

    free(buf);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Walk all nodes in a multi-block metadata B+Tree (DFS)              */
/*  Each node spans METADATA_NODE_BLOCKS contiguous blocks.            */
/*  index_entry_size / child_lba_off parameterize the index entry.     */
/* ------------------------------------------------------------------ */

/**
 * Walk all nodes in a multi-block metadata B+Tree via iterative DFS.
 *
 * Each node spans @c METADATA_NODE_BLOCKS contiguous blocks.  The
 * caller provides the index entry size and the byte offset of the
 * child_lba field within that entry to locate children.
 *
 * @param ctx               Filesystem context.
 * @param root_lba          Root node LBA.
 * @param index_entry_size  Size in bytes of each index entry.
 * @param child_lba_off     Byte offset of the @c child_lba field in
 *                          the index entry structure.
 * @param out_lbas          Output: heap-allocated array of node LBAs.
 * @param out_count         Output: number of elements in @p out_lbas.
 * @return @c OBMAFS3_OK on success.
 */
int walk_meta_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                                 size_t child_lba_off, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        if(count >= cap)
        {
            cap           = cap == 0 ? 64 : cap * 2;
            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
            if(!tmp)
            {
                free(buf);
                free(stack);
                free(lbas);
                return OBMAFS3_ERR_NOMEM;
            }
            lbas = tmp;
        }
        lbas[count++] = lba;

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                uint64_t child;
                memcpy(&child, buf + sizeof(struct btree_node_header) + (size_t)i * index_entry_size + child_lba_off,
                       sizeof(uint64_t));

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = child;
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Verify checksums for a list of multi-block metadata node LBAs      */
/* ------------------------------------------------------------------ */

/**
 * Verify stored checksums of a list of multi-block metadata node
 * blocks.
 *
 * Each node spans @c METADATA_NODE_BLOCKS contiguous blocks.
 * Optionally rewrites nodes with corrected checksums.
 *
 * @param ctx          Filesystem context.
 * @param node_lbas    Array of node LBAs to verify.
 * @param node_count   Number of elements in @p node_lbas.
 * @param tree_name    Human-readable tree name for diagnostic output.
 * @param auto_yes     If non-zero, always repair without asking.
 * @param auto_no      If non-zero, never repair.
 * @param bad_count    Output: number of nodes with bad checksums.
 * @param fixed_count  Output: number of nodes whose checksums were repaired.
 * @return @c OBMAFS3_OK on success.
 */
int verify_meta_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                      const char *tree_name, int auto_yes, int auto_no, uint64_t *bad_count,
                                      uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *bad_lbas = NULL;
    uint64_t  bad_cap  = 0;
    uint64_t  bad      = 0;

    for(uint64_t n = 0; n < node_count; n++)
    {
        uint64_t lba = node_lbas[n];

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(bad_lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            fprintf(stderr, "  %s node at LBA %" PRIu64 ": bad magic\n", tree_name, lba);
            bad++;
            continue;
        }

        size_t  data_size = sizeof(struct btree_node_header) + hdr.keys_length;
        uint8_t stored[32];
        memcpy(stored, hdr.checksum, 32);
        memset(buf + __builtin_offsetof(struct btree_node_header, checksum), 0, 32);
        uint8_t computed[32];
        obmafs3_checksum_block(buf, data_size, computed);
        memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), stored, 32);

        if(memcmp(stored, computed, 32) != 0)
        {
            fprintf(stderr, "  %s node at LBA %" PRIu64 ": checksum mismatch\n", tree_name, lba);
            if(bad >= bad_cap)
            {
                bad_cap = bad_cap ? bad_cap * 2 : 16;
                uint64_t *tmp = realloc(bad_lbas, bad_cap * sizeof(uint64_t));
                if(!tmp) { free(buf); free(bad_lbas); return OBMAFS3_ERR_NOMEM; }
                bad_lbas = tmp;
            }
            bad_lbas[bad] = lba;
            bad++;
        }
    }

    /* Offer to fix */
    uint64_t fixes = 0;
    if(bad > 0)
    {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "  Fix %" PRIu64 " %s node checksum%s?", bad, tree_name,
                 bad == 1 ? "" : "s");
        if(ask_fix(auto_yes, auto_no, prompt))
        {
            for(uint64_t i = 0; i < bad; i++)
            {
                int rc = obmafs3_block_read(ctx, bad_lbas[i], buf, node_sz);
                if(rc != OBMAFS3_OK) continue;

                struct btree_node_header hdr;
                memcpy(&hdr, buf, sizeof(hdr));
                size_t data_size = sizeof(struct btree_node_header) + hdr.keys_length;
                memset(hdr.checksum, 0, 32);
                memcpy(buf, &hdr, sizeof(hdr));
                obmafs3_checksum_block(buf, data_size, hdr.checksum);
                memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr.checksum, 32);

                rc = obmafs3_block_write(ctx, bad_lbas[i], buf, node_sz);
                if(rc == OBMAFS3_OK)
                    fixes++;
                else
                    fprintf(stderr, "    Error writing LBA %" PRIu64 ": %d\n", bad_lbas[i], rc);
            }
        }
    }

    free(buf);
    free(bad_lbas);
    *bad_count   = bad;
    *fixed_count = fixes;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
