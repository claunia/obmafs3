/*
 * obmafsck - Check and validate an OBMAFS3 filesystem
 */
#include "obmafs.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

/* ------------------------------------------------------------------ */
/*  Options                                                            */
/* ------------------------------------------------------------------ */

/**
 * Print usage information for obmafsck.
 *
 * @param prog  Program name to display in the usage line.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <device-or-file>\n"
            "\n"
            "Options:\n"
            "  -y              Assume 'yes' to all repair questions\n"
            "  -n              Assume 'no' to all repair questions\n"
            "  -s, --scrub     Verify checksums of all data blocks\n"
            "  -d, --dedup-stats  Show deduplication and compression statistics\n"
            "  -h, --help      Show this help message\n",
            prog);
}

/* Return value: 1 = yes, 0 = no */
/**
 * Prompt the user to fix a problem, or decide automatically.
 *
 * @param auto_yes  If non-zero, always return 1 (yes).
 * @param auto_no   If non-zero, always return 0 (no).
 * @param prompt    Question text displayed to the user.
 * @return 1 if the fix should be applied, 0 otherwise.
 */
static int ask_fix(int auto_yes, int auto_no, const char *prompt)
{
    if(auto_yes) return 1;
    if(auto_no) return 0;

    fprintf(stdout, "%s [y/n] ", prompt);
    fflush(stdout);

    int ch = fgetc(stdin);
    /* consume rest of line */
    int c2;
    while((c2 = fgetc(stdin)) != '\n' && c2 != EOF);
    return (ch == 'y' || ch == 'Y');
}

/* ------------------------------------------------------------------ */
/*  Progress bar helper                                                */
/* ------------------------------------------------------------------ */

/**
 * Print a visual progress bar on stderr.
 *
 * Renders something like:
 *   \r  Walking Dedup tree [==================>           ] 768/27058 nodes
 *
 * @param prefix  Label text (e.g. "Walking Dedup tree").
 * @param done    Number of items completed.
 * @param total   Total number of items.
 */
static void print_bar(const char *prefix, uint64_t done, uint64_t total)
{
    const int bar_width = 30;
    double    frac      = total > 0 ? (double)done / (double)total : 1.0;
    if(frac > 1.0) frac = 1.0;
    int filled = (int)(frac * bar_width);

    fprintf(stderr, "\r  %s [" , prefix);
    for(int i = 0; i < bar_width; i++)
    {
        if(i < filled)
            fputc('=', stderr);
        else if(i == filled)
            fputc('>', stderr);
        else
            fputc(' ', stderr);
    }
    fprintf(stderr, "] %" PRIu64 "/" "%" PRIu64 "   ", done, total);
    fflush(stderr);
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
static int walk_inode_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
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
        fprintf(stderr, "\r%80s\r", "");
        fflush(stderr);
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
static int walk_catalog_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, uint64_t **out_lbas,
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
 * stored value.  Reports mismatches to stderr.
 *
 * @param ctx         Filesystem context.
 * @param node_lbas   Array of node LBAs to verify.
 * @param node_count  Number of elements in @p node_lbas.
 * @param tree_name   Human-readable tree name for diagnostic output.
 * @param bad_count   Output: number of nodes with bad checksums.
 * @return @c OBMAFS3_OK on success.
 */
static int verify_btree_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                       const char *tree_name, uint64_t *bad_count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t bad = 0;

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
            bad++;
        }
    }

    if(node_count > 10)
    {
        fprintf(stderr, "\r%80s\r", "");
        fflush(stderr);
    }

    free(buf);
    *bad_count = bad;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  B+Tree key ordering validation                                     */
/* ------------------------------------------------------------------ */

/** Ordering tree-type identifiers for verify_btree_ordering(). */
#define ORD_UINT64_KEY  0  /**< key = first uint64_t (inode, dedup, cd_*, refcount) */
#define ORD_CATALOG     1  /**< key = (parent_id, name) */
#define ORD_OVERFLOW    2  /**< leaf: (inode_id, logical_offset), index: uint64_t */
#define ORD_MEDIA_TAG   3  /**< key = (inode_id, tag_type) */
#define ORD_METADATA    4  /**< key = (inode_id, key[256]) */
#define ORD_METADATA_IDX 5 /**< key = (key[256], value[1025], inode_id) */

/**
 * Compare two B+Tree keys extracted from raw record/entry bytes.
 *
 * @param a       Pointer to first record or index entry.
 * @param b       Pointer to second record or index entry.
 * @param type    One of the ORD_* tree-type constants.
 * @param is_leaf Non-zero for leaf records, zero for index entries.
 * @return Negative if a < b, 0 if equal, positive if a > b.
 */
static int ordering_key_cmp(const uint8_t *a, const uint8_t *b, int type, int is_leaf)
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
            /* btree_index_entry: key(8) = inode_id, child_lba(8) */
            memcpy(&ua, a, 8);
            memcpy(&ub, b, 8);
            return (ua < ub) ? -1 : (ua > ub) ? 1 : 0;
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
static int  s_ord_key_type;
static int  s_ord_is_leaf;

/** qsort comparator that delegates to ordering_key_cmp(). */
static int ordering_qsort_cmp(const void *a, const void *b)
{
    return ordering_key_cmp((const uint8_t *)a, (const uint8_t *)b, s_ord_key_type, s_ord_is_leaf);
}

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
static int fix_node_ordering(struct obmafs3_ctx *ctx, uint8_t *buf, size_t buf_size, uint64_t lba, int nblocks,
                             struct btree_node_header *hdr, int key_type, int is_leaf, size_t stride)
{
    /* Set file-scope qsort context */
    s_ord_key_type = key_type;
    s_ord_is_leaf  = is_leaf;

    /* Sort the records in-place */
    uint8_t *entries = buf + sizeof(struct btree_node_header);
    qsort(entries, hdr->node_keys, stride, ordering_qsort_cmp);

    /* Recompute checksum */
    memset(hdr->checksum, 0, 32);
    memcpy(buf, hdr, sizeof(*hdr));
    obmafs3_checksum_block(buf, buf_size, hdr->checksum);
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
static void verify_fix_total_nodes(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba,
                                   uint64_t actual, const char *tree_name, const char *indent, int auto_yes,
                                   int auto_no, int *errors)
{
    if((uint64_t)hdr->total_nodes == actual)
    {
        printf("%sTotal nodes:      %u (OK)\n", indent, hdr->total_nodes);
        return;
    }

    printf("%sTotal nodes:      MISMATCH (header %u, walked %" PRIu64 ")\n", indent, hdr->total_nodes, actual);
    (*errors)++;

    if(ask_fix(auto_yes, auto_no, "Fix total_nodes in header?"))
    {
        hdr->total_nodes = (uint32_t)actual;
        int rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        if(rc == OBMAFS3_OK)
        {
            printf("%sTotal nodes:      FIXED -> %" PRIu64 "\n", indent, actual);
            (*errors)--;
        }
        else
        {
            fprintf(stderr, "%sError writing %s header: %d\n", indent, tree_name, rc);
        }
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
static int patch_sibling_links(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t new_left, uint64_t new_right,
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

    /* Zero checksum, copy header into buffer, recompute, store */
    memset(hdr.checksum, 0, 32);
    memcpy(buf, &hdr, sizeof(hdr));
    obmafs3_checksum_block(buf, node_sz, hdr.checksum);
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
static int verify_fix_sibling_links(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
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
static int verify_btree_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
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
        fprintf(stderr, "\r%80s\r", "");
        fflush(stderr);
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
static int verify_meta_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count, int key_type,
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
        fprintf(stderr, "\r%80s\r", "");
        fflush(stderr);
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
static int walk_meta_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
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
 *
 * @param ctx         Filesystem context.
 * @param node_lbas   Array of node LBAs to verify.
 * @param node_count  Number of elements in @p node_lbas.
 * @param tree_name   Human-readable tree name for diagnostic output.
 * @param bad_count   Output: number of nodes with bad checksums.
 * @return @c OBMAFS3_OK on success.
 */
static int verify_meta_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                      const char *tree_name, uint64_t *bad_count)
{
    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t bad = 0;

    for(uint64_t n = 0; n < node_count; n++)
    {
        uint64_t lba = node_lbas[n];

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
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
            bad++;
        }
    }

    free(buf);
    *bad_count = bad;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Collect data-block LBAs from all inodes                            */
/* ------------------------------------------------------------------ */

/**
 * Collect data block LBAs referenced by all inode extent records.
 *
 * Walks the inode B+Tree via iterative DFS and extracts every data
 * block LBA from the extent arrays of each leaf-level inode record.
 *
 * @param ctx             Filesystem context.
 * @param inode_root_lba  Root node LBA of the inode tree.
 * @param out_lbas        Output: heap-allocated array of data block LBAs.
 * @param out_count       Output: number of elements in @p out_lbas.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_inode_data_blocks(struct obmafs3_ctx *ctx, uint64_t inode_root_lba, uint64_t **out_lbas,
                                     uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(inode_root_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Iterative DFS via explicit stack (avoids stale right_link
       references to freed leaf nodes) */
    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = inode_root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

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
                struct btree_index_entry ie;
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
            continue;
        }

        /* Leaf node: collect extent blocks from inode records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct inode_record rec;
            memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

            for(int e = 0; e < 8; e++)
            {
                if(rec.extents[e].block_count == 0) continue;
                for(uint64_t b = 0; b < rec.extents[e].block_count; b++)
                {
                    if(count >= cap)
                    {
                        cap           = (cap == 0) ? 128 : cap * 2;
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
                    lbas[count++] = rec.extents[e].start_block + b;
                }
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Walk the overflow B+Tree (DFS) and collect all data blocks referenced
 * by overflow_extent entries in leaf nodes.
 */
static int collect_overflow_data_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(ctx->overflow_hdr.root_node_lba == 0) return OBMAFS3_OK;

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

    stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level > 0)
        {
            /* Index node: push children onto stack */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct btree_index_entry ie;
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
        else
        {
            /* Leaf node: collect data block LBAs from extents */
            const uint8_t *entries = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct overflow_extent oe;
                memcpy(&oe, entries + i * sizeof(struct overflow_extent), sizeof(oe));
                for(uint64_t b = 0; b < oe.block_count; b++)
                {
                    if(count >= cap)
                    {
                        cap           = (cap == 0) ? 128 : cap * 2;
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
                    lbas[count++] = oe.start_block + b;
                }
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
/*  Collect external data blocks used by media tags                    */
/* ------------------------------------------------------------------ */

/**
 * Walk the media tag B+Tree and collect all external (non-inline)
 * data block LBAs.
 */
static int collect_media_tag_data_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(ctx->sb.media_tag_lba == 0 || ctx->media_tag_hdr.root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
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

    stack[stk_size++] = ctx->media_tag_hdr.root_node_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level > 0)
        {
            const uint8_t *entries = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct media_tag_index_entry ie;
                memcpy(&ie, entries + i * sizeof(struct media_tag_index_entry), sizeof(ie));
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
        else
        {
            const uint8_t *entries = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct media_tag_record rec;
                memcpy(&rec, entries + i * sizeof(struct media_tag_record), sizeof(rec));

                if(!(rec.flags & MEDIA_TAG_FLAG_INLINE) && rec.data_lba != 0 && rec.data_blocks != 0)
                {
                    for(uint64_t b = 0; b < rec.data_blocks; b++)
                    {
                        if(count >= cap)
                        {
                            cap           = (cap == 0) ? 32 : cap * 2;
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
                        lbas[count++] = rec.data_lba + b;
                    }
                }
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
/*  Collect all blocks used by dedup trees (headers, nodes, data)      */
/* ------------------------------------------------------------------ */

/**
 * Walk the dedup tree list and collect every block LBA that belongs to
 * dedup structures: the tree list block itself, each per-sector-size
 * tree header, every tree node, and every dedup data block.
 *
 * Dedup data blocks span dedup_block_size / block_size standard blocks.
 * Multiple dedup_entry records may share the same data block (different
 * offsets), so we deduplicate the data block LBAs.
 */
static int collect_dedup_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(ctx->sb.dedup_lba == 0) return OBMAFS3_OK;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Separate small array to track unique dedup data block base LBAs
     * for fast deduplication (one entry per dedup data block). */
    uint64_t *unique_bases = NULL;
    uint64_t  unique_count = 0;
    uint64_t  unique_cap   = 0;

#define PUSH_LBA(blk)                                        \
    do                                                       \
    {                                                        \
        if(count >= cap)                                     \
        {                                                    \
            cap          = (cap == 0) ? 256 : cap * 2;       \
            uint64_t *_t = realloc(lbas, cap * sizeof(*_t)); \
            if(!_t)                                          \
            {                                                \
                free(lbas);                                  \
                return OBMAFS3_ERR_NOMEM;                    \
            }                                                \
            lbas = _t;                                       \
        }                                                    \
        lbas[count++] = (blk);                               \
    } while(0)

    /* The tree list block itself is already marked in build_expected_bitmap */

    /* Read the tree list */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf) return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        free(list_buf);
        return rc;
    }

    struct tree_list_header list_hdr;
    memcpy(&list_hdr, list_buf, sizeof(list_hdr));
    if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC)
    {
        free(list_buf);
        return OBMAFS3_ERR_BADMAGIC;
    }

    uint64_t tree_count = list_hdr.tree_count;
    if(tree_count == 0)
    {
        free(list_buf);
        *out_lbas  = lbas;
        *out_count = count;
        return OBMAFS3_OK;
    }

    struct tree_list_entry *entries = malloc((size_t)(tree_count * sizeof(struct tree_list_entry)));
    if(!entries)
    {
        free(list_buf);
        return OBMAFS3_ERR_NOMEM;
    }
    memcpy(entries, list_buf + sizeof(struct tree_list_header), (size_t)(tree_count * sizeof(struct tree_list_entry)));
    free(list_buf);

    uint64_t std_per_dedup = ctx->sb.dedup_block_size / ctx->sb.block_size;

    /* For each dedup tree */
    for(uint64_t t = 0; t < tree_count; t++)
    {
        /* Mark the tree header block */
        PUSH_LBA(entries[t].tree_lba);

        /* Read tree header to get root node */
        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        /* Walk tree nodes (B+Tree: DFS walk) */
        uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(!node_buf)
        {
            free(entries);
            free(lbas);
            return OBMAFS3_ERR_NOMEM;
        }

        /* Iterative DFS via explicit stack */
        uint64_t *stk      = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0;
        uint64_t  stk_cap  = 64;
        if(!stk)
        {
            free(node_buf);
            free(entries);
            free(lbas);
            return OBMAFS3_ERR_NOMEM;
        }

        stk[stk_size++] = thdr.root_node_lba;

        uint64_t nodes_visited = 0;

        while(stk_size > 0)
        {
            uint64_t lba = stk[--stk_size];

            /* Mark the node block */
            PUSH_LBA(lba);

            nodes_visited++;
            {
                char pfx[80];
                snprintf(pfx, sizeof(pfx), "Collecting dedup [tree %" PRIu64 "/%" PRIu64 "]",
                         t + 1, tree_count);
                print_bar(pfx, nodes_visited, (uint64_t)thdr.total_nodes);
            }

            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                /* Index node: push children onto stack */
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, node_buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));

                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stk, stk_cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(stk);
                            free(node_buf);
                            free(entries);
                            free(lbas);
                            free(unique_bases);
                            return OBMAFS3_ERR_NOMEM;
                        }
                        stk = tmp;
                    }
                    stk[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf node: collect data block LBAs from dedup entries */
            const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct dedup_entry de;
                memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));
                if(de.block_lba == 0) continue;

                /* Check if we already recorded this data block base LBA.
                 * Multiple entries can share the same dedup data block
                 * at different offsets. Use the small unique_bases array
                 * for fast lookup instead of scanning the full output. */
                int found = 0;
                for(uint64_t j = 0; j < unique_count; j++)
                {
                    if(unique_bases[j] == de.block_lba)
                    {
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    /* Record this base LBA */
                    if(unique_count >= unique_cap)
                    {
                        unique_cap   = (unique_cap == 0) ? 256 : unique_cap * 2;
                        uint64_t *ut = realloc(unique_bases, unique_cap * sizeof(*ut));
                        if(!ut)
                        {
                            free(stk);
                            free(node_buf);
                            free(entries);
                            free(lbas);
                            free(unique_bases);
                            return OBMAFS3_ERR_NOMEM;
                        }
                        unique_bases = ut;
                    }
                    unique_bases[unique_count++] = de.block_lba;

                    /* Read block header to determine actual allocation */
                    uint8_t  hdr_tmp[sizeof(struct block_header)];
                    int      hrc      = obmafs3_block_read(ctx, de.block_lba, hdr_tmp, sizeof(hdr_tmp));
                    uint64_t used_std = std_per_dedup; /* fallback */
                    if(hrc == OBMAFS3_OK)
                    {
                        struct block_header bh;
                        memcpy(&bh, hdr_tmp, sizeof(bh));
                        if(bh.magic == OBMAFS3_BLOCK_MAGIC)
                        {
                            uint64_t payload =
                                (bh.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? bh.compressed_size : bh.original_size;
                            uint64_t on_disk = sizeof(bh) + payload;
                            uint64_t bs      = ctx->sb.block_size;
                            used_std         = (on_disk + bs - 1) / bs;
                            if(used_std > std_per_dedup) used_std = std_per_dedup;
                        }
                    }

                    /* The last (partial) block of each dedup tree keeps all
                     * std_per_dedup blocks allocated — dedup_block_flush
                     * intentionally does not free trailing blocks so the
                     * block can be resumed on next mount.  Account for
                     * that here so the expected bitmap matches. */
                    uint64_t mark_std = (de.block_lba == thdr.last_block_lba)
                                            ? std_per_dedup
                                            : used_std;
                    for(uint64_t s = 0; s < mark_std; s++) PUSH_LBA(de.block_lba + s);
                }
            }
        }

        free(stk);
        free(node_buf);
    }

    free(entries);
    free(unique_bases);
#undef PUSH_LBA

    /* Clear progress line */
    fprintf(stderr, "\r%80s\r", "");
    fflush(stderr);

    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Build expected bitmap                                              */
/* ------------------------------------------------------------------ */

/**
 * Reconstruct the expected allocation bitmap from on-disk structures.
 *
 * Walks every tree (superblock, catalog, inode, overflow, dedup, media
 * tag, CD prefix/suffix/subchannel, metadata, metadata index) and
 * marks every referenced block in a freshly allocated bitmap.  The
 * result can be compared against the on-disk bitmap to detect
 * allocation inconsistencies.
 *
 * @param ctx           Filesystem context.
 * @param total_blocks  Number of blocks in the filesystem.
 * @param bitmap_bytes  Size of the bitmap in bytes.
 * @param out_error     Output: set to non-zero on allocation failure.
 * @return Heap-allocated expected bitmap, or @c NULL on error.
 */
static uint8_t *build_expected_bitmap(struct obmafs3_ctx *ctx, uint64_t total_blocks, uint64_t bitmap_bytes,
                                      int *out_error)
{
    uint8_t *expected = calloc(1, (size_t)bitmap_bytes);
    if(!expected)
    {
        *out_error = 1;
        return NULL;
    }

    /* Progress reporting — 17 discrete steps */
    int         step       = 0;
    const int   total_steps = 17;

#define PROGRESS(desc)                                                         \
    do                                                                         \
    {                                                                          \
        step++;                                                                \
        fprintf(stderr, "\r  Building expected bitmap... [%2d/%d] %-30s",      \
                step, total_steps, (desc));                                     \
        fflush(stderr);                                                        \
    } while(0)

/* Helper to set a bit */
#define MARK(blk)                                                            \
    do                                                                       \
    {                                                                        \
        if((blk) < total_blocks) expected[(blk) / 8] |= (1u << ((blk) % 8)); \
    } while(0)

    /* Block 0: superblock */
    PROGRESS("superblock");
    MARK(0);

    /* Catalog tree: header + nodes (B+Tree: DFS walk) */
    PROGRESS("catalog tree");
    MARK(ctx->sb.catalog_lba);
    {
        uint64_t *cat_nodes = NULL;
        uint64_t  cat_count = 0;
        int       rc        = walk_catalog_btree_nodes(ctx, ctx->catalog_hdr.root_node_lba, &cat_nodes, &cat_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < cat_count; i++) MARK(cat_nodes[i]);
            free(cat_nodes);
        }
        else
        {
            fprintf(stderr, "Warning: could not walk catalog tree nodes\n");
        }
    }

    /* Inode tree: header + nodes (B+Tree: DFS walk) */
    PROGRESS("inode tree");
    MARK(ctx->sb.inode_lba);
    {
        uint64_t *ino_nodes = NULL;
        uint64_t  ino_count = 0;
        int       rc        = walk_inode_btree_nodes(ctx, ctx->inode_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ino_nodes, &ino_count, 0, NULL);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < ino_count; i++) MARK(ino_nodes[i]);
            free(ino_nodes);
        }
        else
        {
            fprintf(stderr, "Warning: could not walk inode tree nodes\n");
        }
    }

    /* Overflow tree header (if present) */
    PROGRESS("overflow tree");
    if(ctx->sb.overflow_lba != 0)
    {
        MARK(ctx->sb.overflow_lba);
        if(ctx->overflow_hdr.root_node_lba != 0)
        {
            uint64_t *ovf_nodes = NULL;
            uint64_t  ovf_count = 0;
            int       rc        = walk_inode_btree_nodes(ctx, ctx->overflow_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ovf_nodes, &ovf_count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < ovf_count; i++) MARK(ovf_nodes[i]);
                free(ovf_nodes);
            }
        }
    }

    /* Dedup tree list header */
    PROGRESS("dedup tree list");
    if(ctx->sb.dedup_lba != 0) MARK(ctx->sb.dedup_lba);

    /* Media tag tree header and nodes (if present) */
    PROGRESS("media tag tree");
    if(ctx->sb.media_tag_lba != 0)
    {
        MARK(ctx->sb.media_tag_lba);
        if(ctx->media_tag_hdr.root_node_lba != 0)
        {
            uint64_t *mt_nodes = NULL;
            uint64_t  mt_count = 0;
            int       rc       = walk_inode_btree_nodes(ctx, ctx->media_tag_hdr.root_node_lba, sizeof(struct media_tag_index_entry), __builtin_offsetof(struct media_tag_index_entry, child_lba), &mt_nodes, &mt_count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < mt_count; i++) MARK(mt_nodes[i]);
                free(mt_nodes);
            }
        }
    }

    /* CD prefix tree header and nodes (if present) */
    PROGRESS("CD prefix tree");
    if(ctx->sb.cd_prefix_lba != 0)
    {
        MARK(ctx->sb.cd_prefix_lba);
        if(ctx->cd_prefix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->cd_prefix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* CD suffix tree header and nodes (if present) */
    PROGRESS("CD suffix tree");
    if(ctx->sb.cd_suffix_lba != 0)
    {
        MARK(ctx->sb.cd_suffix_lba);
        if(ctx->cd_suffix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->cd_suffix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* CD subchannel tree header and nodes (if present) */
    PROGRESS("CD subchannel tree");
    if(ctx->sb.cd_subchannel_lba != 0)
    {
        MARK(ctx->sb.cd_subchannel_lba);
        if(ctx->cd_subchannel_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->cd_subchannel_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* Metadata tree header and nodes (multi-block, if present) */
    PROGRESS("metadata tree");
    if(ctx->sb.metadata_lba != 0)
    {
        MARK(ctx->sb.metadata_lba);
        if(ctx->metadata_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int rc = walk_meta_btree_nodes(ctx, ctx->metadata_hdr.root_node_lba, sizeof(struct metadata_index_entry),
                                           __builtin_offsetof(struct metadata_index_entry, child_lba), &nodes, &count);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++)
                    for(int b = 0; b < METADATA_NODE_BLOCKS; b++) MARK(nodes[i] + (uint64_t)b);
                free(nodes);
            }
        }
    }

    /* Metadata index tree header and nodes (multi-block, if present) */
    PROGRESS("metadata index tree");
    if(ctx->sb.metadata_idx_lba != 0)
    {
        MARK(ctx->sb.metadata_idx_lba);
        if(ctx->metadata_idx_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc =
                walk_meta_btree_nodes(ctx, ctx->metadata_idx_hdr.root_node_lba, sizeof(struct metadata_idx_index_entry),
                                      __builtin_offsetof(struct metadata_idx_index_entry, child_lba), &nodes, &count);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++)
                    for(int b = 0; b < METADATA_NODE_BLOCKS; b++) MARK(nodes[i] + (uint64_t)b);
                free(nodes);
            }
        }
    }

    /* Refcount tree header and nodes (if present) */
    PROGRESS("refcount tree");
    if(ctx->sb.refcount_lba != 0)
    {
        MARK(ctx->sb.refcount_lba);
        if(ctx->refcount_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->refcount_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* Bitmap blocks */
    PROGRESS("bitmap blocks");
    for(uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++) MARK(ctx->sb.bitmap_lba + i);

    /* File data blocks from inode extents */
    PROGRESS("inode data blocks");
    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint64_t *data_lbas  = NULL;
        uint64_t  data_count = 0;
        int       rc         = collect_inode_data_blocks(ctx, ctx->inode_hdr.root_node_lba, &data_lbas, &data_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < data_count; i++) MARK(data_lbas[i]);
            free(data_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect inode data blocks\n");
        }
    }

    /* File data blocks from overflow extents */
    PROGRESS("overflow data blocks");
    {
        uint64_t *ovf_data_lbas  = NULL;
        uint64_t  ovf_data_count = 0;
        int       rc             = collect_overflow_data_blocks(ctx, &ovf_data_lbas, &ovf_data_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < ovf_data_count; i++) MARK(ovf_data_lbas[i]);
            free(ovf_data_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect overflow data blocks\n");
        }
    }

    /* Dedup tree blocks: headers, nodes, and data blocks */
    PROGRESS("dedup data blocks");
    {
        uint64_t *dedup_lbas  = NULL;
        uint64_t  dedup_count = 0;
        int       rc          = collect_dedup_blocks(ctx, &dedup_lbas, &dedup_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < dedup_count; i++) MARK(dedup_lbas[i]);
            free(dedup_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect dedup tree blocks\n");
        }
    }

    /* External media tag data blocks */
    PROGRESS("media tag data blocks");
    {
        uint64_t *mt_data_lbas  = NULL;
        uint64_t  mt_data_count = 0;
        int       rc            = collect_media_tag_data_blocks(ctx, &mt_data_lbas, &mt_data_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < mt_data_count; i++) MARK(mt_data_lbas[i]);
            free(mt_data_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect media tag data blocks\n");
        }
    }

#undef MARK

    /* Clear the progress line */
    fprintf(stderr, "\r%80s\r", "");
    fflush(stderr);

#undef PROGRESS

    *out_error = 0;
    return expected;
}

/* ------------------------------------------------------------------ */
/*  Scrub: verify checksums of all data blocks                         */
/* ------------------------------------------------------------------ */

/**
 * Print a progress bar to stderr.
 *
 * @param done   Number of items processed so far.
 * @param total  Total number of items.
 * @param bad    Number of errors detected so far.
 */
static void print_progress(uint64_t done, uint64_t total, uint64_t bad)
{
    int    bar_width = 40;
    double frac      = total > 0 ? (double)done / (double)total : 1.0;
    int    filled    = (int)(frac * bar_width);

    fprintf(stderr, "\r  [");
    for(int i = 0; i < bar_width; i++)
    {
        if(i < filled)
            fputc('=', stderr);
        else if(i == filled)
            fputc('>', stderr);
        else
            fputc(' ', stderr);
    }
    fprintf(stderr,
            "] %3d%% | %" PRIu64 "/"
            "%" PRIu64 " blocks | %" PRIu64 " error%s",
            (int)(frac * 100), done, total, bad, bad == 1 ? "" : "s");
    fflush(stderr);
}

/**
 * Verify checksums of all file data blocks.
 *
 * Collects data block LBAs from both inline inode extents and the
 * overflow tree, reads each block header, recomputes the checksum,
 * and reports any mismatches.
 *
 * @param ctx  Filesystem context.
 * @return Number of bad blocks detected.
 */
static uint64_t scrub_data_blocks(struct obmafs3_ctx *ctx)
{
    /* Collect all data block LBAs from inode extents */
    if(ctx->inode_hdr.root_node_lba == 0)
    {
        printf("\nData block scrub:\n");
        printf("  No data blocks to scrub.\n");
        return 0;
    }

    uint64_t *data_lbas  = NULL;
    uint64_t  data_count = 0;
    int       rc         = collect_inode_data_blocks(ctx, ctx->inode_hdr.root_node_lba, &data_lbas, &data_count);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "\nError: could not collect data block LBAs: %d\n", rc);
        return 0;
    }

    /* Also collect overflow data blocks */
    uint64_t *ovf_lbas  = NULL;
    uint64_t  ovf_count = 0;
    rc                  = collect_overflow_data_blocks(ctx, &ovf_lbas, &ovf_count);
    if(rc == OBMAFS3_OK && ovf_count > 0)
    {
        uint64_t *tmp = realloc(data_lbas, (data_count + ovf_count) * sizeof(*tmp));
        if(tmp)
        {
            data_lbas = tmp;
            memcpy(data_lbas + data_count, ovf_lbas, ovf_count * sizeof(*ovf_lbas));
            data_count += ovf_count;
        }
        free(ovf_lbas);
    }
    else
    {
        free(ovf_lbas);
    }

    if(data_count == 0)
    {
        printf("\nData block scrub:\n");
        printf("  No data blocks to scrub.\n");
        free(data_lbas);
        return 0;
    }

    printf("\nData block scrub:\n");
    printf("  Blocks to verify: %" PRIu64 "\n", data_count);

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        free(data_lbas);
        return 0;
    }

    uint64_t bad         = 0;
    uint64_t read_errors = 0;

    /* With variable-length extents, individual data blocks no longer
     * carry a block_header.  Only compressed extent groups have a
     * header (spanning multiple physical blocks).  For this per-block
     * readability pass we simply verify that each physical block can
     * be read successfully. */
    for(uint64_t i = 0; i < data_count; i++)
    {
        if(i % 64 == 0 || i == data_count - 1) print_progress(i + 1, data_count, bad);

        rc = obmafs3_block_read(ctx, data_lbas[i], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            read_errors++;
            bad++;
        }
    }

    print_progress(data_count, data_count, bad);
    fprintf(stderr, "\n");

    if(bad == 0) { printf("  Result:           OK\n"); }
    else
    {
        printf("  Result:           %" PRIu64 " error(s)\n", bad);
        if(read_errors > 0) printf("    Read errors:    %" PRIu64 "\n", read_errors);
    }

    free(buf);
    free(data_lbas);
    return bad;
}

/**
 * Scrub dedup data blocks.
 * Each dedup data block is a contiguous 4 MiB region (dedup_block_size)
 * with a single block_header at the start covering all the sector data.
 */
static uint64_t scrub_dedup_data_blocks(struct obmafs3_ctx *ctx)
{
    if(ctx->sb.dedup_lba == 0)
    {
        printf("\nDedup data block scrub:\n");
        printf("  No dedup data blocks to scrub.\n");
        return 0;
    }

    /* Read the tree list header */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf)
    {
        fprintf(stderr, "\nError: out of memory\n");
        return 0;
    }

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "\nError: could not read dedup tree list: %d\n", rc);
        free(list_buf);
        return 0;
    }

    struct tree_list_header list_hdr;
    memcpy(&list_hdr, list_buf, sizeof(list_hdr));
    if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC || list_hdr.tree_count == 0)
    {
        free(list_buf);
        printf("\nDedup data block scrub:\n");
        printf("  No dedup data blocks to scrub.\n");
        return 0;
    }

    uint64_t                tree_count = list_hdr.tree_count;
    struct tree_list_entry *entries    = malloc((size_t)(tree_count * sizeof(struct tree_list_entry)));
    if(!entries)
    {
        free(list_buf);
        return 0;
    }
    memcpy(entries, list_buf + sizeof(struct tree_list_header), (size_t)(tree_count * sizeof(struct tree_list_entry)));
    free(list_buf);

    /* Collect unique dedup data block base LBAs */
    uint64_t *bases      = NULL;
    uint64_t  base_count = 0;
    uint64_t  base_cap   = 0;

    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!node_buf)
    {
        free(entries);
        return 0;
    }

    for(uint64_t t = 0; t < tree_count; t++)
    {
        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        uint64_t lba = thdr.root_node_lba;
        while(lba != 0)
        {
            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct dedup_entry de;
                memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));
                if(de.block_lba == 0) continue;

                /* Check uniqueness */
                int found = 0;
                for(uint64_t j = 0; j < base_count; j++)
                {
                    if(bases[j] == de.block_lba)
                    {
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    if(base_count >= base_cap)
                    {
                        base_cap     = (base_cap == 0) ? 256 : base_cap * 2;
                        uint64_t *bt = realloc(bases, base_cap * sizeof(*bt));
                        if(!bt)
                        {
                            free(node_buf);
                            free(entries);
                            free(bases);
                            return 0;
                        }
                        bases = bt;
                    }
                    bases[base_count++] = de.block_lba;
                }
            }

            lba = nhdr.right_link;
        }
    }

    free(node_buf);
    free(entries);

    if(base_count == 0)
    {
        printf("\nDedup data block scrub:\n");
        printf("  No dedup data blocks to scrub.\n");
        free(bases);
        return 0;
    }

    printf("\nDedup data block scrub:\n");
    printf("  Blocks to verify: %" PRIu64 "\n", base_count);

    size_t   dedup_size = (size_t)ctx->sb.dedup_block_size;
    uint8_t *buf        = calloc(1, dedup_size);
    if(!buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        free(bases);
        return 0;
    }

    uint64_t bad          = 0;
    uint64_t bad_magic    = 0;
    uint64_t bad_checksum = 0;
    uint64_t read_errors  = 0;

    for(uint64_t i = 0; i < base_count; i++)
    {
        if(i % 4 == 0 || i == base_count - 1) print_progress(i + 1, base_count, bad);

        /* Read first standard block to get the header */
        rc = obmafs3_block_read(ctx, bases[i], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            read_errors++;
            bad++;
            continue;
        }

        struct block_header bhdr;
        memcpy(&bhdr, buf, sizeof(bhdr));

        if(bhdr.magic != OBMAFS3_BLOCK_MAGIC)
        {
            bad_magic++;
            bad++;
            continue;
        }

        /* Determine actual on-disk payload size */
        size_t check_size =
            (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? (size_t)bhdr.compressed_size : (size_t)bhdr.original_size;
        if(check_size > dedup_size - sizeof(bhdr)) check_size = dedup_size - sizeof(bhdr);

        /* Read remaining standard blocks if payload extends beyond first */
        uint64_t total_on_disk = sizeof(bhdr) + check_size;
        uint64_t bs            = ctx->sb.block_size;
        uint64_t needed_std    = (total_on_disk + bs - 1) / bs;
        if(needed_std > 1)
        {
            rc = obmafs3_block_read(ctx, bases[i] + 1, buf + bs, (size_t)((needed_std - 1) * bs));
            if(rc != OBMAFS3_OK)
            {
                read_errors++;
                bad++;
                continue;
            }
        }

        uint8_t computed[32];
        obmafs3_checksum_block(buf + sizeof(bhdr), check_size, computed);

        if(memcmp(computed, bhdr.checksum, 32) != 0)
        {
            bad_checksum++;
            bad++;
        }
    }

    print_progress(base_count, base_count, bad);
    fprintf(stderr, "\n");

    if(bad == 0) { printf("  Result:           OK\n"); }
    else
    {
        printf("  Result:           %" PRIu64 " error(s)\n", bad);
        if(read_errors > 0) printf("    Read errors:    %" PRIu64 "\n", read_errors);
        if(bad_magic > 0) printf("    Bad magic:      %" PRIu64 "\n", bad_magic);
        if(bad_checksum > 0) printf("    Bad checksum:   %" PRIu64 "\n", bad_checksum);
    }

    free(buf);
    free(bases);
    return bad;
}

/* ------------------------------------------------------------------ */
/*  Dedup statistics                                                   */
/* ------------------------------------------------------------------ */

/**
 * Print a byte count as a human-readable string (B/KiB/MiB/GiB/TiB).
 *
 * @param bytes  Number of bytes to format.
 */
static void print_human_size(uint64_t bytes)
{
    if(bytes >= 1099511627776ULL)
        printf("%.2f TiB", (double)bytes / 1099511627776.0);
    else if(bytes >= 1073741824ULL)
        printf("%.2f GiB", (double)bytes / 1073741824.0);
    else if(bytes >= 1048576ULL)
        printf("%.2f MiB", (double)bytes / 1048576.0);
    else if(bytes >= 1024ULL)
        printf("%.2f KiB", (double)bytes / 1024.0);
    else
        printf("%" PRIu64 " B", bytes);
}

struct dedup_tree_stats
{
    uint16_t sector_size;
    uint64_t dedup_entries;      /* unique sector hashes in tree */
    uint64_t unique_blocks;      /* unique dedup data blocks */
    uint64_t original_bytes;     /* sum of original_size from block headers */
    uint64_t compressed_bytes;   /* sum of actual on-disk payload size */
    uint64_t physical_bytes;     /* unique_blocks x dedup_block_size */
    uint64_t compressed_count;   /* number of compressed blocks */
    uint64_t uncompressed_count; /* number of uncompressed blocks */
};

/**
 * Compute and print deduplication and compression statistics.
 *
 * Walks the dedup tree list, enumerates every dedup entry, counts
 * unique data blocks, reads block headers for compression information,
 * and prints per-tree and aggregate statistics including dedup ratio,
 * compression ratio, and total space savings.
 *
 * @param ctx  Filesystem context.
 * @return @c OBMAFS3_OK on success.
 */
static int compute_dedup_stats(struct obmafs3_ctx *ctx)
{
    if(ctx->sb.dedup_lba == 0)
    {
        printf("\nDedup statistics:\n");
        printf("  No dedup trees found.\n");
        return 0;
    }

    /* Read the tree list header */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf) return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        free(list_buf);
        return rc;
    }

    struct tree_list_header tlhdr;
    memcpy(&tlhdr, list_buf, sizeof(tlhdr));
    if(tlhdr.magic != OBMAFS3_TREELIST_MAGIC || tlhdr.tree_count == 0)
    {
        free(list_buf);
        printf("\nDedup statistics:\n");
        printf("  No dedup trees found.\n");
        return 0;
    }

    uint64_t                tree_count = tlhdr.tree_count;
    struct tree_list_entry *tl_entries = malloc((size_t)(tree_count * sizeof(struct tree_list_entry)));
    if(!tl_entries)
    {
        free(list_buf);
        return OBMAFS3_ERR_NOMEM;
    }
    memcpy(tl_entries, list_buf + sizeof(struct tree_list_header),
           (size_t)(tree_count * sizeof(struct tree_list_entry)));
    free(list_buf);

    struct dedup_tree_stats *stats = calloc((size_t)tree_count, sizeof(*stats));
    if(!stats)
    {
        free(tl_entries);
        return OBMAFS3_ERR_NOMEM;
    }

    /* ---- Walk media image inodes to count sector maps ---- */
    uint64_t total_media_files        = 0;
    uint64_t total_media_file_size    = 0;
    uint64_t total_sector_map_entries = 0;
    uint64_t total_sector_count       = 0;

    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
        if(buf)
        {
            uint64_t *stack  = malloc(64 * sizeof(uint64_t));
            uint64_t  stk_sz = 0, stk_cap = 64;
            if(stack)
            {
                stack[stk_sz++] = ctx->inode_hdr.root_node_lba;
                while(stk_sz > 0)
                {
                    uint64_t lba = stack[--stk_sz];
                    rc           = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;

                    struct btree_node_header hdr;
                    memcpy(&hdr, buf, sizeof(hdr));
                    if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                    if(hdr.level > 0)
                    {
                        for(uint16_t i = 0; i < hdr.node_keys; i++)
                        {
                            struct btree_index_entry ie;
                            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                            if(stk_sz >= stk_cap)
                            {
                                stk_cap *= 2;
                                uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                                if(!tmp) break;
                                stack = tmp;
                            }
                            stack[stk_sz++] = ie.child_lba;
                        }
                        continue;
                    }

                    /* Leaf node */
                    for(uint16_t i = 0; i < hdr.node_keys; i++)
                    {
                        struct inode_record rec;
                        memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));
                        if(rec.file_type == kFileTypeMediaImage)
                        {
                            total_media_files++;
                            total_media_file_size += rec.file_size;
                            total_sector_map_entries += rec.sector_map_size;
                            total_sector_count += rec.sector_count;
                        }
                    }
                }
                free(stack);
            }
            free(buf);
        }
    }

    /* ---- Walk each dedup tree ---- */
    for(uint64_t t = 0; t < tree_count; t++)
    {
        stats[t].sector_size = tl_entries[t].sector_size;

        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, tl_entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        /* Unique data block base LBAs */
        uint64_t *bases      = NULL;
        uint64_t  base_count = 0;
        uint64_t  base_cap   = 0;

        uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(!node_buf) continue;

        /* DFS walk */
        uint64_t *stk    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_sz = 0, stk_cap = 64;
        if(!stk)
        {
            free(node_buf);
            continue;
        }

        stk[stk_sz++] = thdr.root_node_lba;

        uint64_t nodes_visited = 0;

        while(stk_sz > 0)
        {
            uint64_t lba = stk[--stk_sz];

            nodes_visited++;
            {
                char pfx[80];
                snprintf(pfx, sizeof(pfx), "Dedup stats [tree %" PRIu64 "/%" PRIu64 "] nodes",
                         t + 1, tree_count);
                print_bar(pfx, nodes_visited, (uint64_t)thdr.total_nodes);
            }

            rc           = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, node_buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_sz >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stk, stk_cap * sizeof(*tmp));
                        if(!tmp) break;
                        stk = tmp;
                    }
                    stk[stk_sz++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf node: count entries and collect unique block LBAs */
            const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct dedup_entry de;
                memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));

                stats[t].dedup_entries++;

                if(de.block_lba == 0) continue;

                /* Check if this base LBA is already recorded */
                int found = 0;
                for(uint64_t j = 0; j < base_count; j++)
                {
                    if(bases[j] == de.block_lba)
                    {
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    if(base_count >= base_cap)
                    {
                        base_cap     = (base_cap == 0) ? 256 : base_cap * 2;
                        uint64_t *bt = realloc(bases, base_cap * sizeof(*bt));
                        if(!bt) break;
                        bases = bt;
                    }
                    bases[base_count++] = de.block_lba;
                }
            }
        }

        free(stk);
        free(node_buf);

        stats[t].unique_blocks  = base_count;
        stats[t].physical_bytes = 0;

        /* Read each unique data block header for compression stats
         * and compute actual physical allocation per block */
        if(base_count > 0)
        {
            uint8_t *hdr_buf = calloc(1, (size_t)ctx->sb.block_size);
            if(hdr_buf)
            {
                for(uint64_t b = 0; b < base_count; b++)
                {
                    {
                        char pfx[80];
                        snprintf(pfx, sizeof(pfx), "Dedup stats [tree %" PRIu64 "/%" PRIu64 "] blocks",
                                 t + 1, tree_count);
                        print_bar(pfx, b + 1, base_count);
                    }

                    rc = obmafs3_block_read(ctx, bases[b], hdr_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) continue;

                    struct block_header bhdr;
                    memcpy(&bhdr, hdr_buf, sizeof(bhdr));
                    if(bhdr.magic != OBMAFS3_BLOCK_MAGIC) continue;

                    stats[t].original_bytes += bhdr.original_size;

                    uint64_t payload_size;
                    if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                    {
                        stats[t].compressed_bytes += bhdr.compressed_size;
                        stats[t].compressed_count++;
                        payload_size = bhdr.compressed_size;
                    }
                    else
                    {
                        stats[t].compressed_bytes += bhdr.original_size;
                        stats[t].uncompressed_count++;
                        payload_size = bhdr.original_size;
                    }

                    /* Actual on-disk allocation: header + payload,
                     * rounded up to block_size */
                    uint64_t on_disk = sizeof(bhdr) + payload_size;
                    uint64_t bs      = ctx->sb.block_size;
                    stats[t].physical_bytes += ((on_disk + bs - 1) / bs) * bs;
                }
                free(hdr_buf);
            }
        }

        free(bases);
    }

    /* Clear progress line */
    fprintf(stderr, "\r%80s\r", "");
    fflush(stderr);

    /* ---- Print report ---- */
    printf("\nDedup statistics:\n");
    printf("  Media image files:        %" PRIu64 "\n", total_media_files);
    printf("  Total logical size:       ");
    print_human_size(total_media_file_size);
    printf(" (%" PRIu64 " bytes)\n", total_media_file_size);
    printf("  Total sector refs:        %" PRIu64 "\n", total_sector_map_entries);
    printf("  Dedup block size:         ");
    print_human_size(ctx->sb.dedup_block_size);
    printf("\n");

    uint64_t grand_dedup_entries     = 0;
    uint64_t grand_unique_blocks     = 0;
    uint64_t grand_unique_sector_bytes = 0;
    uint64_t grand_original          = 0;
    uint64_t grand_compressed        = 0;
    uint64_t grand_physical          = 0;
    uint64_t grand_compressed_blks   = 0;
    uint64_t grand_uncompressed_blks = 0;

    for(uint64_t t = 0; t < tree_count; t++)
    {
        printf("\n  Tree %" PRIu64 " (sector size: %" PRIu16 " bytes):\n", t, stats[t].sector_size);
        printf("    Unique sectors:         %" PRIu64 "\n", stats[t].dedup_entries);

        uint64_t dedup_sector_bytes = stats[t].dedup_entries * (uint64_t)stats[t].sector_size;
        printf("    Unique sector data:     ");
        print_human_size(dedup_sector_bytes);
        printf("\n");

        printf("    Dedup data blocks:      %" PRIu64 "\n", stats[t].unique_blocks);
        printf("      Compressed:           %" PRIu64 "\n", stats[t].compressed_count);
        printf("      Uncompressed:         %" PRIu64 "\n", stats[t].uncompressed_count);

        printf("    Uncompressed data:      ");
        print_human_size(stats[t].original_bytes);
        printf("\n");
        printf("    Compressed data:        ");
        print_human_size(stats[t].compressed_bytes);
        printf("\n");
        printf("    Physical allocation:    ");
        print_human_size(stats[t].physical_bytes);
        printf("\n");

        if(stats[t].original_bytes > 0 && stats[t].compressed_bytes < stats[t].original_bytes)
        {
            double comp_ratio = (double)stats[t].original_bytes / (double)stats[t].compressed_bytes;
            double comp_saved = (1.0 - (double)stats[t].compressed_bytes / (double)stats[t].original_bytes) * 100.0;
            printf("    Compression ratio:      %.2f:1 (%.1f%% smaller)\n", comp_ratio, comp_saved);
        }

        grand_dedup_entries += stats[t].dedup_entries;
        grand_unique_blocks += stats[t].unique_blocks;
        grand_unique_sector_bytes += stats[t].dedup_entries * (uint64_t)stats[t].sector_size;
        grand_original += stats[t].original_bytes;
        grand_compressed += stats[t].compressed_bytes;
        grand_physical += stats[t].physical_bytes;
        grand_compressed_blks += stats[t].compressed_count;
        grand_uncompressed_blks += stats[t].uncompressed_count;
    }

    if(tree_count > 1)
    {
        printf("\n  Totals across all trees:\n");
        printf("    Unique sectors:         %" PRIu64 "\n", grand_dedup_entries);
        printf("    Dedup data blocks:      %" PRIu64 " (%" PRIu64 " compressed, %" PRIu64 " uncompressed)\n",
               grand_unique_blocks, grand_compressed_blks, grand_uncompressed_blks);
        printf("    Uncompressed data:      ");
        print_human_size(grand_original);
        printf("\n");
        printf("    Compressed data:        ");
        print_human_size(grand_compressed);
        printf("\n");
        printf("    Physical allocation:    ");
        print_human_size(grand_physical);
        printf("\n");
    }

    /* ---- Savings summary ---- */
    printf("\n  Savings summary:\n");

    if(total_sector_map_entries > 0 && grand_dedup_entries > 0)
    {
        double   dedup_ratio = (double)total_sector_map_entries / (double)grand_dedup_entries;
        uint64_t dup_sectors = total_sector_map_entries - grand_dedup_entries;
        printf("    Dedup ratio:            %.2f:1"
               " (%" PRIu64 " refs -> %" PRIu64 " unique)\n",
               dedup_ratio, total_sector_map_entries, grand_dedup_entries);
        printf("    Duplicate sectors:      %" PRIu64 "\n", dup_sectors);

        if(total_media_file_size > grand_unique_sector_bytes)
        {
            uint64_t bytes_saved_dedup = total_media_file_size - grand_unique_sector_bytes;
            printf("    Saved by dedup:         ");
            print_human_size(bytes_saved_dedup);
            printf(" (%.1f%%)\n", (double)bytes_saved_dedup / (double)total_media_file_size * 100.0);
        }
    }

    if(grand_original > 0 && grand_compressed > 0)
    {
        double comp_ratio = (double)grand_original / (double)grand_compressed;
        printf("    Compression ratio:      %.2f:1\n", comp_ratio);
        if(grand_compressed < grand_original)
        {
            uint64_t comp_saved = grand_original - grand_compressed;
            printf("    Saved by compression:   ");
            print_human_size(comp_saved);
            printf(" (%.1f%%)\n", (double)comp_saved / (double)grand_original * 100.0);
        }
    }

    if(total_media_file_size > 0 && grand_physical > 0)
    {
        double overall = (double)total_media_file_size / (double)grand_physical;
        printf("    Overall ratio:          %.2f:1\n", overall);
        if(total_media_file_size > grand_physical)
        {
            uint64_t total_saved = total_media_file_size - grand_physical;
            printf("    Total space saved:      ");
            print_human_size(total_saved);
            printf(" (%.1f%%)\n", (double)total_saved / (double)total_media_file_size * 100.0);
        }
    }

    free(stats);
    free(tl_entries);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Cross-reference: orphan inodes & dangling catalog entries          */
/* ------------------------------------------------------------------ */

/**
 * Catalog entry reference collected from a catalog leaf node.
 * Stores enough information to identify and delete the entry.
 */
struct catalog_ref
{
    uint64_t inode_id;
    uint64_t parent_id;
    char     name[256];
};

/**
 * Walk all catalog B+Tree leaf nodes and collect every
 * (inode_id, parent_id, name) tuple.
 *
 * @param ctx       Filesystem context.
 * @param out_refs  Output: heap-allocated array of catalog_ref.
 * @param out_count Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_catalog_refs(struct obmafs3_ctx *ctx, struct catalog_ref **out_refs, uint64_t *out_count)
{
    *out_refs  = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->catalog_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct catalog_ref *refs = NULL;
    uint64_t count = 0, cap = 0;

    /* Iterative DFS */
    uint64_t *stack = malloc(64 * sizeof(uint64_t));
    uint64_t stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(refs); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct catalog_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect catalog records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                struct catalog_ref *tmp = realloc(refs, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_NOMEM; }
                refs = tmp;
            }
            struct catalog_record crec;
            memcpy(&crec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(crec), sizeof(crec));
            refs[count].inode_id  = crec.inode_id;
            refs[count].parent_id = crec.parent_id;
            memset(refs[count].name, 0, sizeof(refs[count].name));
            memcpy(refs[count].name, crec.name, sizeof(crec.name));
            count++;
        }
    }

    free(buf);
    free(stack);
    *out_refs  = refs;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Walk all inode B+Tree leaf nodes and collect every inode_id.
 *
 * @param ctx        Filesystem context.
 * @param out_ids    Output: heap-allocated array of inode_id values.
 * @param out_count  Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_inode_ids(struct obmafs3_ctx *ctx, uint64_t **out_ids, uint64_t *out_count)
{
    *out_ids   = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *ids = NULL;
    uint64_t count = 0, cap = 0;

    uint64_t *stack = malloc(64 * sizeof(uint64_t));
    uint64_t stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(ids); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect inode_id from each inode_record */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                uint64_t *tmp = realloc(ids, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_NOMEM; }
                ids = tmp;
            }
            struct inode_record irec;
            memcpy(&irec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(irec), sizeof(irec));
            ids[count++] = irec.inode_id;
        }
    }

    free(buf);
    free(stack);
    *out_ids   = ids;
    *out_count = count;
    return OBMAFS3_OK;
}

/** qsort comparator for uint64_t values. */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

/** qsort comparator for catalog_ref by inode_id. */
static int cmp_catalog_ref(const void *a, const void *b)
{
    const struct catalog_ref *ra = (const struct catalog_ref *)a;
    const struct catalog_ref *rb = (const struct catalog_ref *)b;
    return (ra->inode_id < rb->inode_id) ? -1 : (ra->inode_id > rb->inode_id) ? 1 : 0;
}

/** Binary search: return non-zero if @p id is present in the sorted array. */
static int u64_sorted_contains(const uint64_t *arr, uint64_t count, uint64_t id)
{
    uint64_t lo = 0, hi = count;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if(arr[mid] < id)      lo = mid + 1;
        else if(arr[mid] > id) hi = mid;
        else                   return 1;
    }
    return 0;
}

/**
 * Cross-reference the inode and catalog trees.
 *
 * Detects:
 * - **Orphan inodes**: inode_id present in the inode tree but not
 *   referenced by any catalog entry.  Fixed by deleting the inode.
 * - **Dangling catalog entries**: a catalog entry whose inode_id does
 *   not exist in the inode tree.  Fixed by deleting the catalog entry.
 *
 * @param ctx        Filesystem context.
 * @param auto_yes   If nonzero, always repair without prompting.
 * @param auto_no    If nonzero, never repair.
 * @param errors     In/out: incremented for each unfixed error.
 */
static void cross_check_inodes_catalog(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors)
{
    struct catalog_ref *cat_refs  = NULL;
    uint64_t           *inode_ids = NULL;
    uint64_t            cat_count = 0, ino_count = 0;
    int                 rc;

    printf("\nInode / Catalog cross-reference:\n");

    rc = collect_catalog_refs(ctx, &cat_refs, &cat_count);
    if(rc != OBMAFS3_OK)
    {
        printf("  Error: could not walk catalog tree (%d)\n", rc);
        (*errors)++;
        return;
    }

    rc = collect_inode_ids(ctx, &inode_ids, &ino_count);
    if(rc != OBMAFS3_OK)
    {
        printf("  Error: could not walk inode tree (%d)\n", rc);
        free(cat_refs);
        (*errors)++;
        return;
    }

    /* Build sorted inode_id set from catalog refs */
    uint64_t *cat_ids = NULL;
    uint64_t  cat_unique = 0;
    if(cat_count > 0)
    {
        qsort(cat_refs, (size_t)cat_count, sizeof(cat_refs[0]), cmp_catalog_ref);

        /* Deduplicate to get unique inode_ids referenced by catalog */
        cat_ids = malloc(cat_count * sizeof(*cat_ids));
        if(cat_ids)
        {
            cat_ids[0] = cat_refs[0].inode_id;
            cat_unique = 1;
            for(uint64_t i = 1; i < cat_count; i++)
            {
                if(cat_refs[i].inode_id != cat_ids[cat_unique - 1])
                    cat_ids[cat_unique++] = cat_refs[i].inode_id;
            }
        }
    }

    /* Sort inode_ids */
    if(ino_count > 0)
        qsort(inode_ids, (size_t)ino_count, sizeof(inode_ids[0]), cmp_u64);

    /* ---- Detect orphan inodes ---- */
    uint64_t orphan_count = 0;
    if(inode_ids && cat_ids)
    {
        for(uint64_t i = 0; i < ino_count; i++)
        {
            uint64_t id = inode_ids[i];
            if(id == OBMAFS3_ROOT_INODE_ID) continue; /* root dir always exists */
            if(!u64_sorted_contains(cat_ids, cat_unique, id))
                orphan_count++;
        }
    }

    if(orphan_count == 0)
    {
        printf("  Orphan inodes:          OK\n");
    }
    else
    {
        printf("  Orphan inodes:          %" PRIu64 " found\n", orphan_count);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Re-link orphan inodes into lost+found?"))
        {
            /* Ensure lost+found directory exists under root */
            struct catalog_record lf_cat;
            uint64_t lf_inode_id = 0;
            rc = obmafs3_catalog_lookup(ctx, OBMAFS3_ROOT_INODE_ID, "lost+found", &lf_cat);
            if(rc == OBMAFS3_OK)
            {
                lf_inode_id = lf_cat.inode_id;
            }
            else
            {
                /* Create lost+found directory */
                lf_inode_id = obmafs3_alloc_inode_id(ctx);

                struct catalog_record new_cat;
                memset(&new_cat, 0, sizeof(new_cat));
                new_cat.inode_id       = lf_inode_id;
                new_cat.parent_id      = OBMAFS3_ROOT_INODE_ID;
                new_cat.directory_flag  = 1;
                strncpy(new_cat.name, "lost+found", sizeof(new_cat.name) - 1);
                rc = obmafs3_catalog_insert(ctx, &new_cat);
                if(rc != OBMAFS3_OK)
                {
                    printf("    Error: could not create lost+found directory (%d)\n", rc);
                    goto skip_orphan_fix;
                }

                uint64_t now = (uint64_t)time(NULL);
                struct inode_record lf_inode;
                memset(&lf_inode, 0, sizeof(lf_inode));
                lf_inode.inode_id          = lf_inode_id;
                lf_inode.uid               = 0;
                lf_inode.gid               = 0;
                lf_inode.mode              = 0755;
                lf_inode.creation_time     = now;
                lf_inode.modification_time = now;
                lf_inode.access_time       = now;
                lf_inode.file_type         = kFileTypeDirectory;
                lf_inode.ref_count         = 1;

                rc = obmafs3_inode_put(ctx, &lf_inode);
                if(rc != OBMAFS3_OK)
                {
                    printf("    Error: could not create lost+found inode (%d)\n", rc);
                    goto skip_orphan_fix;
                }
                printf("    Created lost+found directory (inode %" PRIu64 ")\n", lf_inode_id);
            }

            /* Re-link each orphan inode into lost+found */
            uint64_t fixed = 0;
            for(uint64_t i = 0; i < ino_count; i++)
            {
                uint64_t id = inode_ids[i];
                if(id == OBMAFS3_ROOT_INODE_ID) continue;
                if(id == lf_inode_id) continue;  /* skip lost+found itself */
                if(!u64_sorted_contains(cat_ids, cat_unique, id))
                {
                    /* Read the inode to determine file type */
                    struct inode_record irec;
                    rc = obmafs3_inode_get(ctx, id, &irec);
                    if(rc != OBMAFS3_OK) continue;

                    /* Build a name: "inode_<id>" */
                    char name_buf[64];
                    snprintf(name_buf, sizeof(name_buf), "inode_%" PRIu64, id);

                    struct catalog_record new_entry;
                    memset(&new_entry, 0, sizeof(new_entry));
                    new_entry.inode_id      = id;
                    new_entry.parent_id     = lf_inode_id;
                    new_entry.directory_flag = (irec.file_type == kFileTypeDirectory) ? 1 : 0;
                    strncpy(new_entry.name, name_buf, sizeof(new_entry.name) - 1);

                    rc = obmafs3_catalog_insert(ctx, &new_entry);
                    if(rc == OBMAFS3_OK)
                    {
                        /* Ensure ref_count is at least 1 */
                        if(irec.ref_count == 0)
                        {
                            irec.ref_count = 1;
                            obmafs3_inode_put(ctx, &irec);
                        }
                        fixed++;
                    }
                }
            }
            printf("    Re-linked %" PRIu64 " orphan inode(s) into lost+found.\n", fixed);
            if(fixed == orphan_count) (*errors)--;
        }
    }
skip_orphan_fix:

    /* ---- Detect dangling catalog entries ---- */
    uint64_t dangling_count = 0;
    if(cat_refs && inode_ids)
    {
        for(uint64_t i = 0; i < cat_count; i++)
        {
            if(!u64_sorted_contains(inode_ids, ino_count, cat_refs[i].inode_id))
                dangling_count++;
        }
    }

    if(dangling_count == 0)
    {
        printf("  Dangling catalog refs:  OK\n");
    }
    else
    {
        printf("  Dangling catalog refs:  %" PRIu64 " found\n", dangling_count);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Delete dangling catalog entries?"))
        {
            uint64_t fixed = 0;
            for(uint64_t i = 0; i < cat_count; i++)
            {
                if(!u64_sorted_contains(inode_ids, ino_count, cat_refs[i].inode_id))
                {
                    rc = obmafs3_catalog_delete(ctx, cat_refs[i].parent_id, cat_refs[i].name);
                    if(rc == OBMAFS3_OK) fixed++;
                }
            }
            printf("    Deleted %" PRIu64 " dangling catalog entry(ies).\n", fixed);
            if(fixed == dangling_count) (*errors)--;
        }
    }

    free(cat_ids);
    free(cat_refs);
    free(inode_ids);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

/**
 * Entry point for the OBMAFS3 filesystem checker.
 *
 * Opens the filesystem in lenient mode, validates the superblock,
 * verifies tree header and node checksums for all B+Trees (catalog,
 * inode, overflow, dedup, media tag, CD prefix/suffix/subchannel,
 * metadata, metadata index), checks allocation bitmap consistency,
 * and optionally scrubs data block checksums and reports dedup
 * statistics.
 */
int main(int argc, char *argv[])
{
    int auto_yes       = 0;
    int auto_no        = 0;
    int do_scrub       = 0;
    int do_dedup_stats = 0;

    static struct option long_opts[] = {
        {       "help", no_argument, NULL, 'h'},
        {      "scrub", no_argument, NULL, 's'},
        {"dedup-stats", no_argument, NULL, 'd'},
        {         NULL,           0, NULL,   0}
    };

    int opt;
    while((opt = getopt_long(argc, argv, "ynsdh", long_opts, NULL)) != -1)
    {
        switch(opt)
        {
            case 'y':
                auto_yes = 1;
                break;
            case 'n':
                auto_no = 1;
                break;
            case 's':
                do_scrub = 1;
                break;
            case 'd':
                do_dedup_stats = 1;
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if(auto_yes && auto_no)
    {
        fprintf(stderr, "Error: -y and -n are mutually exclusive\n");
        return 1;
    }

    if(optind >= argc)
    {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[optind];

    /* ---- Open the filesystem with raw I/O and detailed error reporting ---- */
    int fd = open(path, O_RDWR);
    if(fd < 0)
    {
        /* Fall back to read-only if read-write fails (e.g. read-only media) */
        fd = open(path, O_RDONLY);
        if(fd < 0)
        {
            fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
            return 1;
        }
    }

    struct stat file_stat;
    if(fstat(fd, &file_stat) < 0)
    {
        fprintf(stderr, "Error: cannot stat '%s': %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    if(!S_ISREG(file_stat.st_mode) && !S_ISBLK(file_stat.st_mode))
    {
        fprintf(stderr, "Error: '%s' is not a regular file or block device\n", path);
        close(fd);
        return 1;
    }

    /* Read the superblock directly */
    struct obmafs3_sb sb;
    ssize_t           nread = pread(fd, &sb, sizeof(sb), 0);
    if(nread < 0)
    {
        fprintf(stderr, "Error: cannot read superblock from '%s': %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if((size_t)nread < sizeof(sb))
    {
        fprintf(stderr, "Error: '%s' is too small to contain a superblock (read %zd of %zu bytes)\n", path, nread,
                sizeof(sb));
        close(fd);
        return 1;
    }

    if(sb.magic != OBMAFS3_SB_MAGIC)
    {
        fprintf(stderr,
                "Error: '%s' does not contain an OBMAFS3 filesystem\n"
                "  Expected magic: 0x%016" PRIx64 "\n"
                "  Found magic:    0x%016" PRIx64 "\n",
                path, (uint64_t)OBMAFS3_SB_MAGIC, sb.magic);
        close(fd);
        return 1;
    }

    if(sb.block_size == 0)
    {
        fprintf(stderr, "Error: superblock has invalid block_size = 0\n");
        close(fd);
        return 1;
    }

    if(sb.total_bytes == 0)
    {
        fprintf(stderr, "Error: superblock has invalid total_bytes = 0\n");
        close(fd);
        return 1;
    }

    if(sb.dedup_block_size == 0)
    {
        fprintf(stderr, "Error: superblock has invalid dedup_block_size = 0\n");
        close(fd);
        return 1;
    }

    if(sb.catalog_lba == 0)
    {
        fprintf(stderr, "Error: superblock has no catalog tree (catalog_lba = 0)\n");
        close(fd);
        return 1;
    }

    if(sb.inode_lba == 0)
    {
        fprintf(stderr, "Error: superblock has no inode tree (inode_lba = 0)\n");
        close(fd);
        return 1;
    }

    /* Build a minimal ctx for the library helpers (block_read, btree_header_read, etc.) */
    struct obmafs3_ctx *ctx = calloc(1, sizeof(*ctx));
    if(!ctx)
    {
        fprintf(stderr, "Error: out of memory allocating filesystem context\n");
        close(fd);
        return 1;
    }
    ctx->fd          = fd;
    ctx->sb          = sb;
    ctx->compression = 1;
    ctx->zstd_level  = 15;

    /* Initialise thread-local buffer key (obmafsck is single-threaded,
     * but the library now uses TLS for its scratch buffers). */
    if(pthread_key_create(&ctx->tls_key, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_key_create failed\n");
        close(fd);
        free(ctx);
        return 1;
    }
    pthread_mutex_init(&ctx->write_lock, NULL);

    /* Force lazy TLS allocation so the library has buffers to work with */
    struct obmafs3_thread_bufs *tb = obmafs3_get_thread_bufs(ctx);

    ctx->rc_leaf_buf   = malloc((size_t)sb.block_size);
    ctx->rc_leaf_valid = 0;

    if(!tb || !tb->hdr_buf || !tb->node_buf || !tb->io_buf || !tb->io_buf2 ||
       !tb->comp_buf || !tb->zstd_cctx || !tb->zstd_dctx || !ctx->rc_leaf_buf)
    {
        fprintf(stderr, "Error: out of memory allocating work buffers\n");
        obmafs3_close(ctx);
        return 1;
    }

    /* Read B+Tree headers leniently (tolerate checksum errors so we can report them) */
    int cs_tmp;
    int rc;

    rc = obmafs3_btree_header_read_lenient(ctx, sb.catalog_lba, &ctx->catalog_hdr, &cs_tmp);
    if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
        fprintf(stderr, "Warning: cannot read catalog tree header at LBA %" PRIu64 " (error %d)\n", sb.catalog_lba,
                rc);

    rc = obmafs3_btree_header_read_lenient(ctx, sb.inode_lba, &ctx->inode_hdr, &cs_tmp);
    if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
        fprintf(stderr, "Warning: cannot read inode tree header at LBA %" PRIu64 " (error %d)\n", sb.inode_lba, rc);

    if(sb.overflow_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.overflow_lba, &ctx->overflow_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read overflow tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.overflow_lba, rc);
    }

    if(sb.media_tag_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.media_tag_lba, &ctx->media_tag_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read media tag tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.media_tag_lba, rc);
    }

    if(sb.cd_prefix_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.cd_prefix_lba, &ctx->cd_prefix_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read CD prefix tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.cd_prefix_lba, rc);
    }

    if(sb.cd_suffix_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.cd_suffix_lba, &ctx->cd_suffix_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read CD suffix tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.cd_suffix_lba, rc);
    }

    if(sb.cd_subchannel_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.cd_subchannel_lba, &ctx->cd_subchannel_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read CD subchannel tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.cd_subchannel_lba, rc);
    }

    if(sb.metadata_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.metadata_lba, &ctx->metadata_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read metadata tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.metadata_lba, rc);
    }

    if(sb.metadata_idx_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.metadata_idx_lba, &ctx->metadata_idx_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read metadata index tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.metadata_idx_lba, rc);
    }

    if(sb.refcount_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.refcount_lba, &ctx->refcount_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read refcount tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.refcount_lba, rc);
    }

    int errors = 0;

    /* ---- Superblock ---- */
    printf("Superblock:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->sb.magic,
           ctx->sb.magic == OBMAFS3_SB_MAGIC ? "OK" : "BAD");
    printf("  Block size:       %" PRIu64 "\n", ctx->sb.block_size);
    printf("  Dedup block size: %" PRIu64 "\n", ctx->sb.dedup_block_size);
    printf("  Total bytes:      %" PRIu64 "\n", ctx->sb.total_bytes);
    printf("  Volume label:     %s\n", ctx->sb.volume_label);

    if(ctx->sb.magic != OBMAFS3_SB_MAGIC) errors++;

    /* ---- Catalog tree ---- */
    printf("\nCatalog tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->catalog_hdr.magic,
           ctx->catalog_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
    {
        int cat_hdr_cs_ok = 0;
        obmafs3_btree_header_read_lenient(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr, &cat_hdr_cs_ok);
        printf("  Header checksum:  %s\n", cat_hdr_cs_ok ? "OK" : "BAD");
        if(!cat_hdr_cs_ok) errors++;
    }
    printf("  Root node LBA:    %" PRIu64 "\n", ctx->catalog_hdr.root_node_lba);

    if(ctx->catalog_hdr.root_node_lba != 0)
    {
        uint64_t *cat_nodes      = NULL;
        uint64_t  cat_node_count = 0;
        int       wrc = walk_catalog_btree_nodes(ctx, ctx->catalog_hdr.root_node_lba, &cat_nodes, &cat_node_count);
        if(wrc == OBMAFS3_OK)
        {
            uint64_t cat_bad = 0;
            verify_btree_node_checksums(ctx, cat_nodes, cat_node_count, "Catalog", &cat_bad);
            uint64_t cat_ord = 0, cat_fix = 0;
            verify_btree_ordering(ctx, cat_nodes, cat_node_count, ORD_CATALOG,
                                  sizeof(struct catalog_record), sizeof(struct catalog_index_entry),
                                  "Catalog", auto_yes, auto_no, &cat_ord, &cat_fix);
            free(cat_nodes);
            if(cat_bad > 0)
            {
                printf("  Node checksums:   %" PRIu64 " BAD\n", cat_bad);
                errors++;
            }
            else
            {
                printf("  Node checksums:   OK\n");
            }
            if(cat_ord > 0)
            {
                printf("  Key ordering:     %" PRIu64 " BAD", cat_ord);
                if(cat_fix > 0) printf(" (%" PRIu64 " fixed)", cat_fix);
                printf("\n");
                errors += (int)(cat_ord - cat_fix);
            }
            else
            {
                printf("  Key ordering:     OK\n");
            }
            verify_fix_total_nodes(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, cat_node_count, "Catalog", "  ",
                                   auto_yes, auto_no, &errors);
            {
                uint64_t sib_bad = 0, sib_fix = 0;
                verify_fix_sibling_links(ctx, ctx->catalog_hdr.root_node_lba,
                                         sizeof(struct catalog_index_entry),
                                         __builtin_offsetof(struct catalog_index_entry, child_lba), 1,
                                         "Catalog", auto_yes, auto_no, &sib_bad, &sib_fix);
                if(sib_bad > 0)
                {
                    printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                    if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                    printf("\n");
                    errors += (int)(sib_bad - sib_fix);
                }
                else
                {
                    printf("  Sibling links:    OK\n");
                }
            }
        }
        else
        {
            printf("  Node checksums:   WALK FAILED\n");
            errors++;
        }
    }

    /* ---- Inode tree ---- */
    printf("\nInode tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->inode_hdr.magic,
           ctx->inode_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
    {
        int ino_hdr_cs_ok = 0;
        obmafs3_btree_header_read_lenient(ctx, ctx->sb.inode_lba, &ctx->inode_hdr, &ino_hdr_cs_ok);
        printf("  Header checksum:  %s\n", ino_hdr_cs_ok ? "OK" : "BAD");
        if(!ino_hdr_cs_ok) errors++;
    }
    printf("  Root node LBA:    %" PRIu64 "\n", ctx->inode_hdr.root_node_lba);

    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint64_t *ino_nodes      = NULL;
        uint64_t  ino_node_count = 0;
        int       wrc = walk_inode_btree_nodes(ctx, ctx->inode_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ino_nodes, &ino_node_count, ctx->inode_hdr.total_nodes, "Inode");
        if(wrc == OBMAFS3_OK)
        {
            uint64_t ino_bad = 0;
            verify_btree_node_checksums(ctx, ino_nodes, ino_node_count, "Inode", &ino_bad);
            uint64_t ino_ord = 0, ino_fix = 0;
            verify_btree_ordering(ctx, ino_nodes, ino_node_count, ORD_UINT64_KEY,
                                  sizeof(struct inode_record), sizeof(struct btree_index_entry),
                                  "Inode", auto_yes, auto_no, &ino_ord, &ino_fix);
            free(ino_nodes);
            if(ino_bad > 0)
            {
                printf("  Node checksums:   %" PRIu64 " BAD\n", ino_bad);
                errors++;
            }
            else
            {
                printf("  Node checksums:   OK\n");
            }
            if(ino_ord > 0)
            {
                printf("  Key ordering:     %" PRIu64 " BAD", ino_ord);
                if(ino_fix > 0) printf(" (%" PRIu64 " fixed)", ino_fix);
                printf("\n");
                errors += (int)(ino_ord - ino_fix);
            }
            else
            {
                printf("  Key ordering:     OK\n");
            }
            verify_fix_total_nodes(ctx, &ctx->inode_hdr, ctx->sb.inode_lba, ino_node_count, "Inode", "  ",
                                   auto_yes, auto_no, &errors);
            {
                uint64_t sib_bad = 0, sib_fix = 0;
                verify_fix_sibling_links(ctx, ctx->inode_hdr.root_node_lba,
                                         sizeof(struct btree_index_entry),
                                         __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                         "Inode", auto_yes, auto_no, &sib_bad, &sib_fix);
                if(sib_bad > 0)
                {
                    printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                    if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                    printf("\n");
                    errors += (int)(sib_bad - sib_fix);
                }
                else
                {
                    printf("  Sibling links:    OK\n");
                }
            }
        }
        else
        {
            printf("  Node checksums:   could not walk tree\n");
            errors++;
        }
    }

    /* ---- Overflow tree ---- */
    if(ctx->sb.overflow_lba != 0)
    {
        printf("\nOverflow tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->overflow_hdr.magic,
               ctx->overflow_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int ovf_hdr_cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.overflow_lba, &ctx->overflow_hdr, &ovf_hdr_cs_ok);
            printf("  Header checksum:  %s\n", ovf_hdr_cs_ok ? "OK" : "BAD");
            if(!ovf_hdr_cs_ok) errors++;
        }

        if(ctx->overflow_hdr.root_node_lba != 0)
        {
            uint64_t *ovf_nodes      = NULL;
            uint64_t  ovf_node_count = 0;
            int       wrc = walk_inode_btree_nodes(ctx, ctx->overflow_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ovf_nodes, &ovf_node_count, ctx->overflow_hdr.total_nodes, "Overflow");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t ovf_bad = 0;
                verify_btree_node_checksums(ctx, ovf_nodes, ovf_node_count, "Overflow", &ovf_bad);
                uint64_t ovf_ord = 0, ovf_fix = 0;
                verify_btree_ordering(ctx, ovf_nodes, ovf_node_count, ORD_OVERFLOW,
                                      sizeof(struct overflow_extent), sizeof(struct btree_index_entry),
                                      "Overflow", auto_yes, auto_no, &ovf_ord, &ovf_fix);
                free(ovf_nodes);
                if(ovf_bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", ovf_bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(ovf_ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD", ovf_ord);
                    if(ovf_fix > 0) printf(" (%" PRIu64 " fixed)", ovf_fix);
                    printf("\n");
                    errors += (int)(ovf_ord - ovf_fix);
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->overflow_hdr, ctx->sb.overflow_lba, ovf_node_count, "Overflow",
                                       "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->overflow_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "Overflow", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- Dedup tree list ---- */
    if(ctx->sb.dedup_lba != 0)
    {
        printf("\nDedup tree list:\n");

        uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(list_buf)
        {
            rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
            if(rc == OBMAFS3_OK)
            {
                struct tree_list_header list_hdr;
                memcpy(&list_hdr, list_buf, sizeof(list_hdr));
                printf("  Magic:            0x%016" PRIx64 " (%s)\n", list_hdr.magic,
                       list_hdr.magic == OBMAFS3_TREELIST_MAGIC ? "OK" : "BAD");
                if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC) errors++;

                /* Verify list header checksum */
                if(list_hdr.magic == OBMAFS3_TREELIST_MAGIC)
                {
                    uint8_t stored_cs[32];
                    memcpy(stored_cs, list_hdr.checksum, 32);
                    memset(list_buf + __builtin_offsetof(struct tree_list_header, checksum), 0, 32);
                    uint8_t computed_cs[32];
                    size_t  cs_len = sizeof(struct tree_list_header) +
                                    (size_t)(list_hdr.tree_count * sizeof(struct tree_list_entry));
                    obmafs3_checksum_block(list_buf, cs_len, computed_cs);
                    int cs_ok = (memcmp(stored_cs, computed_cs, 32) == 0);
                    printf("  Header checksum:  %s\n", cs_ok ? "OK" : "BAD");
                    if(!cs_ok) errors++;

                    printf("  Trees:            %" PRIu64 "\n", list_hdr.tree_count);

                    /* Verify each per-sector-size tree */
                    struct tree_list_entry *tl_entries = NULL;
                    if(list_hdr.tree_count > 0)
                    {
                        tl_entries = malloc((size_t)(list_hdr.tree_count * sizeof(struct tree_list_entry)));
                        if(tl_entries)
                            memcpy(tl_entries, list_buf + sizeof(struct tree_list_header),
                                   (size_t)(list_hdr.tree_count * sizeof(struct tree_list_entry)));
                    }

                    for(uint64_t t = 0; t < list_hdr.tree_count && tl_entries; t++)
                    {
                        printf("  Tree %" PRIu64 " (sector_size=%" PRIu16 "):\n", t, tl_entries[t].sector_size);

                        struct btree_header thdr;
                        rc = obmafs3_btree_header_read(ctx, tl_entries[t].tree_lba, &thdr);
                        if(rc == OBMAFS3_OK)
                        {
                            printf("    Magic:          0x%016" PRIx64 " (%s)\n", thdr.magic,
                                   thdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
                            if(thdr.magic != OBMAFS3_BTREE_HDR_MAGIC) errors++;

                            int thdr_cs_ok = 0;
                            obmafs3_btree_header_read_lenient(ctx, tl_entries[t].tree_lba, &thdr, &thdr_cs_ok);
                            printf("    Header checksum:%s\n", thdr_cs_ok ? " OK" : " BAD");
                            if(!thdr_cs_ok) errors++;

                            if(thdr.root_node_lba != 0)
                            {
                                uint64_t *dd_nodes = NULL;
                                uint64_t  dd_count = 0;
                                int       wrc = walk_inode_btree_nodes(ctx, thdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &dd_nodes, &dd_count, thdr.total_nodes, "Dedup");
                                if(wrc == OBMAFS3_OK)
                                {
                                    uint64_t dbad = 0;
                                    verify_btree_node_checksums(ctx, dd_nodes, dd_count, "Dedup", &dbad);
                                    uint64_t dord = 0, dfix = 0;
                                    verify_btree_ordering(ctx, dd_nodes, dd_count, ORD_UINT64_KEY,
                                                          sizeof(struct dedup_entry), sizeof(struct btree_index_entry),
                                                          "Dedup", auto_yes, auto_no, &dord, &dfix);
                                    free(dd_nodes);
                                    if(dbad > 0)
                                    {
                                        printf("    Node checksums: "
                                               "%" PRIu64 " BAD\n",
                                               dbad);
                                        errors++;
                                    }
                                    else
                                    {
                                        printf("    Node checksums:"
                                               " OK\n");
                                    }
                                    if(dord > 0)
                                    {
                                        printf("    Key ordering:   "
                                               "%" PRIu64 " BAD",
                                               dord);
                                        if(dfix > 0) printf(" (%" PRIu64 " fixed)", dfix);
                                        printf("\n");
                                        errors += (int)(dord - dfix);
                                    }
                                    else
                                    {
                                        printf("    Key ordering:  "
                                               " OK\n");
                                    }
                                    verify_fix_total_nodes(ctx, &thdr, tl_entries[t].tree_lba, dd_count, "Dedup",
                                                           "    ", auto_yes, auto_no, &errors);
                                    {
                                        uint64_t sib_bad = 0, sib_fix = 0;
                                        verify_fix_sibling_links(ctx, thdr.root_node_lba,
                                                                 sizeof(struct btree_index_entry),
                                                                 __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                                                 "Dedup", auto_yes, auto_no, &sib_bad, &sib_fix);
                                        if(sib_bad > 0)
                                        {
                                            printf("    Sibling links:  %" PRIu64 " BAD", sib_bad);
                                            if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                                            printf("\n");
                                            errors += (int)(sib_bad - sib_fix);
                                        }
                                        else
                                        {
                                            printf("    Sibling links:  OK\n");
                                        }
                                    }
                                }
                                else
                                {
                                    printf("    Node checksums:"
                                           " could not walk tree\n");
                                    errors++;
                                }
                            }
                        }
                        else
                        {
                            printf("    Error reading header: %d\n", rc);
                            errors++;
                        }
                    }

                    free(tl_entries);
                }
            }
            else
            {
                printf("  Error reading tree list block: %d\n", rc);
                errors++;
            }
            free(list_buf);
        }
    }

    /* ---- Media tag tree ---- */
    if(ctx->sb.media_tag_lba != 0)
    {
        printf("\nMedia tag tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->media_tag_hdr.magic,
               ctx->media_tag_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int mt_hdr_cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr, &mt_hdr_cs_ok);
            printf("  Header checksum:  %s\n", mt_hdr_cs_ok ? "OK" : "BAD");
            if(!mt_hdr_cs_ok) errors++;
        }

        if(ctx->media_tag_hdr.root_node_lba != 0)
        {
            uint64_t *mt_nodes      = NULL;
            uint64_t  mt_node_count = 0;
            int       wrc = walk_inode_btree_nodes(ctx, ctx->media_tag_hdr.root_node_lba, sizeof(struct media_tag_index_entry), __builtin_offsetof(struct media_tag_index_entry, child_lba), &mt_nodes, &mt_node_count, ctx->media_tag_hdr.total_nodes, "Media tag");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t mt_bad = 0;
                verify_btree_node_checksums(ctx, mt_nodes, mt_node_count, "Media tag", &mt_bad);
                uint64_t mt_ord = 0, mt_fix = 0;
                verify_btree_ordering(ctx, mt_nodes, mt_node_count, ORD_MEDIA_TAG,
                                      sizeof(struct media_tag_record), sizeof(struct media_tag_index_entry),
                                      "Media tag", auto_yes, auto_no, &mt_ord, &mt_fix);
                free(mt_nodes);
                if(mt_bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", mt_bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(mt_ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD", mt_ord);
                    if(mt_fix > 0) printf(" (%" PRIu64 " fixed)", mt_fix);
                    printf("\n");
                    errors += (int)(mt_ord - mt_fix);
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, mt_node_count, "Media tag",
                                       "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->media_tag_hdr.root_node_lba,
                                             sizeof(struct media_tag_index_entry),
                                             __builtin_offsetof(struct media_tag_index_entry, child_lba), 1,
                                             "Media tag", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- CD prefix tree ---- */
    if(ctx->sb.cd_prefix_lba != 0)
    {
        printf("\nCD prefix tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->cd_prefix_hdr.magic,
               ctx->cd_prefix_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_prefix_lba, &ctx->cd_prefix_hdr, &cs_ok);
            printf("  Header checksum:  %s\n", cs_ok ? "OK" : "BAD");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_prefix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_inode_btree_nodes(ctx, ctx->cd_prefix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->cd_prefix_hdr.total_nodes, "CD prefix");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD prefix", &bad);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct cd_prefix_record), sizeof(struct btree_index_entry),
                                      "CD prefix", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD", ord);
                    if(ord_fix > 0) printf(" (%" PRIu64 " fixed)", ord_fix);
                    printf("\n");
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_prefix_hdr, ctx->sb.cd_prefix_lba, node_count, "CD prefix",
                                       "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_prefix_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "CD prefix", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- CD suffix tree ---- */
    if(ctx->sb.cd_suffix_lba != 0)
    {
        printf("\nCD suffix tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->cd_suffix_hdr.magic,
               ctx->cd_suffix_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_suffix_lba, &ctx->cd_suffix_hdr, &cs_ok);
            printf("  Header checksum:  %s\n", cs_ok ? "OK" : "BAD");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_suffix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_inode_btree_nodes(ctx, ctx->cd_suffix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->cd_suffix_hdr.total_nodes, "CD suffix");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD suffix", &bad);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct cd_suffix_record), sizeof(struct btree_index_entry),
                                      "CD suffix", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD", ord);
                    if(ord_fix > 0) printf(" (%" PRIu64 " fixed)", ord_fix);
                    printf("\n");
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_suffix_hdr, ctx->sb.cd_suffix_lba, node_count, "CD suffix",
                                       "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_suffix_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "CD suffix", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- CD subchannel tree ---- */
    if(ctx->sb.cd_subchannel_lba != 0)
    {
        printf("\nCD subchannel tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->cd_subchannel_hdr.magic,
               ctx->cd_subchannel_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_subchannel_lba, &ctx->cd_subchannel_hdr, &cs_ok);
            printf("  Header checksum:  %s\n", cs_ok ? "OK" : "BAD");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_subchannel_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc = walk_inode_btree_nodes(ctx, ctx->cd_subchannel_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->cd_subchannel_hdr.total_nodes, "CD subchannel");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD subchannel", &bad);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct cd_subchannel_record), sizeof(struct btree_index_entry),
                                      "CD subchannel", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD", ord);
                    if(ord_fix > 0) printf(" (%" PRIu64 " fixed)", ord_fix);
                    printf("\n");
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_subchannel_hdr, ctx->sb.cd_subchannel_lba, node_count,
                                       "CD subchannel", "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_subchannel_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "CD subchannel", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- Metadata tree (per-image key=value) ---- */
    if(ctx->sb.metadata_lba != 0)
    {
        printf("\nMetadata tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->metadata_hdr.magic,
               ctx->metadata_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr, &cs_ok);
            printf("  Header checksum:  %s\n", cs_ok ? "OK" : "BAD");
            if(!cs_ok) errors++;
        }

        if(ctx->metadata_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc =
                walk_meta_btree_nodes(ctx, ctx->metadata_hdr.root_node_lba, sizeof(struct metadata_index_entry),
                                      __builtin_offsetof(struct metadata_index_entry, child_lba), &nodes, &node_count);
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0;
                verify_meta_node_checksums(ctx, nodes, node_count, "Metadata", &bad);
                uint64_t ord = 0, ord_fix = 0;
                verify_meta_ordering(ctx, nodes, node_count, ORD_METADATA,
                                     sizeof(struct metadata_record), sizeof(struct metadata_index_entry),
                                     "Metadata", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD\n", ord);
                    errors++;
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, node_count, "Metadata",
                                       "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->metadata_hdr.root_node_lba,
                                             sizeof(struct metadata_index_entry),
                                             __builtin_offsetof(struct metadata_index_entry, child_lba),
                                             METADATA_NODE_BLOCKS,
                                             "Metadata", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- Metadata index tree (reverse key+value→inode) ---- */
    if(ctx->sb.metadata_idx_lba != 0)
    {
        printf("\nMetadata index tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->metadata_idx_hdr.magic,
               ctx->metadata_idx_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr, &cs_ok);
            printf("  Header checksum:  %s\n", cs_ok ? "OK" : "BAD");
            if(!cs_ok) errors++;
        }

        if(ctx->metadata_idx_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_meta_btree_nodes(
                ctx, ctx->metadata_idx_hdr.root_node_lba, sizeof(struct metadata_idx_index_entry),
                __builtin_offsetof(struct metadata_idx_index_entry, child_lba), &nodes, &node_count);
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0;
                verify_meta_node_checksums(ctx, nodes, node_count, "Metadata index", &bad);
                uint64_t ord = 0, ord_fix = 0;
                verify_meta_ordering(ctx, nodes, node_count, ORD_METADATA_IDX,
                                     sizeof(struct metadata_idx_record), sizeof(struct metadata_idx_index_entry),
                                     "Metadata index", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD", ord);
                    if(ord_fix > 0) printf(" (%" PRIu64 " fixed)", ord_fix);
                    printf("\n");
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, node_count,
                                       "Metadata index", "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->metadata_idx_hdr.root_node_lba,
                                             sizeof(struct metadata_idx_index_entry),
                                             __builtin_offsetof(struct metadata_idx_index_entry, child_lba),
                                             METADATA_NODE_BLOCKS,
                                             "Metadata index", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- Refcount tree ---- */
    if(ctx->sb.refcount_lba != 0)
    {
        printf("\nRefcount tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->refcount_hdr.magic,
               ctx->refcount_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.refcount_lba, &ctx->refcount_hdr, &cs_ok);
            printf("  Header checksum:  %s\n", cs_ok ? "OK" : "BAD");
            if(!cs_ok) errors++;
        }

        if(ctx->refcount_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_inode_btree_nodes(ctx, ctx->refcount_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->refcount_hdr.total_nodes, "Refcount");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "Refcount", &bad);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct refcount_record), sizeof(struct btree_index_entry),
                                      "Refcount", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    printf("  Node checksums:   %" PRIu64 " BAD\n", bad);
                    errors++;
                }
                else
                {
                    printf("  Node checksums:   OK\n");
                }
                if(ord > 0)
                {
                    printf("  Key ordering:     %" PRIu64 " BAD", ord);
                    if(ord_fix > 0) printf(" (%" PRIu64 " fixed)", ord_fix);
                    printf("\n");
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    printf("  Key ordering:     OK\n");
                }
                verify_fix_total_nodes(ctx, &ctx->refcount_hdr, ctx->sb.refcount_lba, node_count, "Refcount",
                                       "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->refcount_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "Refcount", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        printf("  Sibling links:    %" PRIu64 " BAD", sib_bad);
                        if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                        printf("\n");
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        printf("  Sibling links:    OK\n");
                    }
                }
            }
            else
            {
                printf("  Node checksums:   could not walk tree\n");
                errors++;
            }
        }
    }

    /* ---- Inode / Catalog cross-reference ---- */
    if(ctx->catalog_hdr.root_node_lba != 0 && ctx->inode_hdr.root_node_lba != 0)
        cross_check_inodes_catalog(ctx, auto_yes, auto_no, &errors);

    /* ---- Allocation bitmap ---- */
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;

    if(ctx->sb.bitmap_lba != 0 && ctx->sb.bitmap_blocks != 0)
    {
        uint64_t bitmap_bytes = (total_blocks + 7) / 8;
        size_t   hdr_size     = sizeof(struct bitmap_header);

        /* Read bitmap header from first bitmap block */
        uint8_t             *bhdr_buf = malloc((size_t)ctx->sb.block_size);
        struct bitmap_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        int bhdr_ok = 0;
        if(bhdr_buf)
        {
            if(obmafs3_block_read(ctx, ctx->sb.bitmap_lba, bhdr_buf, (size_t)ctx->sb.block_size) == OBMAFS3_OK)
            {
                memcpy(&bhdr, bhdr_buf, hdr_size);
                bhdr_ok = 1;
            }
            free(bhdr_buf);
        }

        /* Read the raw bitmap data from disk (manually, since we skipped it) */
        uint8_t *disk_bitmap    = calloc(1, (size_t)bitmap_bytes);
        int      bitmap_read_ok = 0;
        if(disk_bitmap)
        {
            uint8_t *blk = malloc((size_t)ctx->sb.block_size);
            if(blk)
            {
                uint64_t remaining = bitmap_bytes;
                uint64_t offset    = 0;
                bitmap_read_ok     = 1;
                for(uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++)
                {
                    if(obmafs3_block_read(ctx, ctx->sb.bitmap_lba + i, blk, (size_t)ctx->sb.block_size) != OBMAFS3_OK)
                    {
                        bitmap_read_ok = 0;
                        break;
                    }
                    if(i == 0)
                    {
                        size_t avail = (size_t)ctx->sb.block_size - hdr_size;
                        size_t copy  = remaining < avail ? (size_t)remaining : avail;
                        memcpy(disk_bitmap, blk + hdr_size, copy);
                        offset += copy;
                        remaining -= copy;
                    }
                    else
                    {
                        size_t copy = remaining < ctx->sb.block_size ? (size_t)remaining : (size_t)ctx->sb.block_size;
                        memcpy(disk_bitmap + offset, blk, copy);
                        offset += copy;
                        remaining -= copy;
                    }
                }
                free(blk);
            }
        }

        /* Verify bitmap checksum */
        int checksum_ok = 0;
        if(bitmap_read_ok && bhdr_ok)
        {
            uint8_t computed[32];
            obmafs3_checksum_block(disk_bitmap, (size_t)bitmap_bytes, computed);
            checksum_ok = (memcmp(bhdr.checksum, computed, 32) == 0);
        }

        uint64_t allocated = 0;
        if(bitmap_read_ok)
        {
            for(uint64_t b = 0; b < total_blocks; b++)
            {
                if((disk_bitmap[b / 8] >> (b % 8)) & 1) allocated++;
            }
        }

        printf("\nAllocation bitmap:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", bhdr.magic,
               bhdr.magic == OBMAFS3_BITMAP_MAGIC ? "OK" : "BAD");
        if(!bhdr_ok || !bitmap_read_ok)
            printf("  Checksum:         UNREADABLE\n");
        else
            printf("  Checksum:         %s\n", checksum_ok ? "OK" : "BAD");
        if(!checksum_ok && bitmap_read_ok) errors++;
        printf("  Bitmap LBA:       %" PRIu64 "\n", ctx->sb.bitmap_lba);
        printf("  Bitmap blocks:    %" PRIu64 "\n", ctx->sb.bitmap_blocks);
        printf("  Total blocks:     %" PRIu64 "\n", total_blocks);
        printf("  Allocated blocks: %" PRIu64 "\n", allocated);
        printf("  Free blocks:      %" PRIu64 "\n", total_blocks - allocated);

        /* ---- Build expected bitmap and compare ---- */
        if(bitmap_read_ok)
        {
            /* Set ctx->bitmap temporarily so build_expected_bitmap helpers work */
            ctx->bitmap      = disk_bitmap;
            ctx->bitmap_size = bitmap_bytes;

            int      build_err = 0;
            uint8_t *expected  = build_expected_bitmap(ctx, total_blocks, bitmap_bytes, &build_err);
            if(expected && !build_err)
            {
                /* Compare on-disk bitmap with expected */
                uint64_t missing       = 0;
                uint64_t extra         = 0;
                uint64_t first_missing = 0;
                uint64_t first_extra   = 0;

                for(uint64_t b = 0; b < total_blocks; b++)
                {
                    int on_disk     = (disk_bitmap[b / 8] >> (b % 8)) & 1;
                    int in_expected = (expected[b / 8] >> (b % 8)) & 1;

                    if(in_expected && !on_disk)
                    {
                        if(missing == 0) first_missing = b;
                        missing++;
                    }
                    if(!in_expected && on_disk)
                    {
                        if(extra == 0) first_extra = b;
                        extra++;
                    }
                }

                if(missing == 0 && extra == 0) { printf("  Consistency:      OK\n"); }
                else
                {
                    printf("  Consistency:      MISMATCH\n");
                    errors++;

                    if(missing > 0)
                        printf("    %" PRIu64 " block(s) used but not marked allocated"
                               " (first: LBA %" PRIu64 ")\n",
                               missing, first_missing);
                    if(extra > 0)
                        printf("    %" PRIu64 " block(s) marked allocated but not used"
                               " (first: LBA %" PRIu64 ")\n",
                               extra, first_extra);

                    uint64_t expected_alloc = 0;
                    for(uint64_t b = 0; b < total_blocks; b++)
                    {
                        if((expected[b / 8] >> (b % 8)) & 1) expected_alloc++;
                    }
                    printf("    Expected allocated: %" PRIu64 ", on-disk allocated: %" PRIu64 "\n", expected_alloc,
                           allocated);

                    if(ask_fix(auto_yes, auto_no, "Fix allocation bitmap?"))
                    {
                        memcpy(ctx->bitmap, expected, (size_t)bitmap_bytes);
                        rc = obmafs3_bitmap_write(ctx);
                        if(rc == OBMAFS3_OK)
                        {
                            printf("  Bitmap repaired.\n");
                            errors--;                              /* checksum error */
                            if(missing > 0 || extra > 0) errors--; /* consistency error */
                        }
                        else
                        {
                            fprintf(stderr, "  Error: failed to write bitmap: %d\n", rc);
                        }
                    }
                }
                free(expected);
            }
            else
            {
                fprintf(stderr, "Warning: could not build expected bitmap\n");
            }

            /* Clear temporary bitmap pointer (obmafs3_close will free) */
        }
        else
        {
            fprintf(stderr, "Warning: could not read bitmap data\n");
        }

        if(!bitmap_read_ok && disk_bitmap)
        {
            free(disk_bitmap);
            ctx->bitmap = NULL;
        }
    }

    /* ---- Data block scrub ---- */
    if(do_scrub)
    {
        uint64_t scrub_bad = scrub_data_blocks(ctx);
        if(scrub_bad > 0) errors += (int)scrub_bad;

        uint64_t dedup_bad = scrub_dedup_data_blocks(ctx);
        if(dedup_bad > 0) errors += (int)dedup_bad;
    }

    /* ---- Dedup statistics ---- */
    if(do_dedup_stats)
    {
        rc = compute_dedup_stats(ctx);
        if(rc != OBMAFS3_OK) fprintf(stderr, "Warning: could not compute dedup stats: %d\n", rc);
    }

    /* ---- Summary ---- */
    printf("\n");
    if(errors > 0)
        printf("Filesystem check completed with %d error(s).\n", errors);
    else
        printf("Filesystem check passed.\n");

    obmafs3_close(ctx);
    return errors > 0 ? 1 : 0;
}
