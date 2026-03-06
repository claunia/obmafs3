// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_collect.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Collect data-block LBAs and build expected bitmap for obmafsck.
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
int collect_inode_data_blocks(struct obmafs3_ctx *ctx, uint64_t inode_root_lba, uint64_t **out_lbas,
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
int collect_overflow_data_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
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
                struct overflow_index_entry ie;
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
int collect_media_tag_data_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
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
int collect_dedup_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
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
                snprintf(pfx, sizeof(pfx), "Collecting dedup [tree %" PRIu64 "/%" PRIu64 "]", t + 1, tree_count);
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
                    uint64_t mark_std = (de.block_lba == thdr.last_block_lba) ? std_per_dedup : used_std;
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
    bar_clear();

    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Collect free-chain block LBAs from a B+Tree header                 */
/* ------------------------------------------------------------------ */

/**
 * Walk the free-node chain starting at @c hdr->free_node_lba and
 * collect every LBA in the chain.  Each free node stores a uint64_t
 * "next" pointer at byte offset 0; the chain ends when next == 0.
 *
 * For multi-block nodes (blocks_per_node > 1), each free node occupies
 * @p blocks_per_node contiguous blocks.
 *
 * @param ctx             Filesystem context.
 * @param hdr             B+Tree header to walk.
 * @param blocks_per_node Number of contiguous blocks per node (1 for normal trees).
 * @param out_lbas        Output: heap-allocated array of block LBAs (caller frees).
 * @param out_count       Output: number of elements in @p out_lbas.
 * @return @c OBMAFS3_OK on success.
 */
int collect_free_chain_blocks(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t blocks_per_node,
                              uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(hdr->free_node_lba == 0 || hdr->free_nodes == 0) return OBMAFS3_OK;

    size_t   bsz     = (size_t)ctx->sb.block_size;
    uint64_t max_lba = ctx->sb.total_bytes / ctx->sb.block_size;
    uint8_t *buf     = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;
    uint64_t  cur   = hdr->free_node_lba;
    uint64_t  limit = ((uint64_t)hdr->free_nodes + 1) * blocks_per_node; /* safety limit (in blocks) to avoid cycles */

    while(cur != 0 && count < limit)
    {
        if(cur >= max_lba) break;

        /* Record all blocks for this node */
        for(uint64_t b = 0; b < blocks_per_node; b++)
        {
            if(count >= cap)
            {
                cap         = (cap == 0) ? 64 : cap * 2;
                uint64_t *t = realloc(lbas, cap * sizeof(*t));
                if(!t)
                {
                    free(buf);
                    free(lbas);
                    return OBMAFS3_ERR_NOMEM;
                }
                lbas = t;
            }
            lbas[count++] = cur + b;
        }

        int rc = obmafs3_block_read(ctx, cur, buf, bsz);
        if(rc != OBMAFS3_OK) break;

        uint64_t next;
        memcpy(&next, buf, sizeof(next));
        cur = next;
    }

    free(buf);
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
uint8_t *build_expected_bitmap(struct obmafs3_ctx *ctx, uint64_t total_blocks, uint64_t bitmap_bytes, int *out_error)
{
    uint8_t *expected = calloc(1, (size_t)bitmap_bytes);
    if(!expected)
    {
        *out_error = 1;
        return NULL;
    }

    /* Progress reporting — 19 discrete steps */
    int       step        = 0;
    const int total_steps = 19;

#define PROGRESS(desc)                                                                     \
    do                                                                                     \
    {                                                                                      \
        step++;                                                                            \
        char _p_pfx[64];                                                                   \
        snprintf(_p_pfx, sizeof(_p_pfx), "Bitmap [%2d/%d] %s", step, total_steps, (desc)); \
        print_bar(_p_pfx, (uint64_t)step, (uint64_t)total_steps);                          \
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
        int       rc = walk_inode_btree_nodes(ctx, ctx->inode_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                              __builtin_offsetof(struct btree_index_entry, child_lba), &ino_nodes, &ino_count,
                                              0, NULL);
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
            int rc = walk_inode_btree_nodes(ctx, ctx->overflow_hdr.root_node_lba, sizeof(struct overflow_index_entry),
                                            __builtin_offsetof(struct overflow_index_entry, child_lba), &ovf_nodes,
                                            &ovf_count, 0, NULL);
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
            int rc = walk_inode_btree_nodes(ctx, ctx->media_tag_hdr.root_node_lba, sizeof(struct media_tag_index_entry),
                                            __builtin_offsetof(struct media_tag_index_entry, child_lba), &mt_nodes,
                                            &mt_count, 0, NULL);
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
            int rc = walk_inode_btree_nodes(ctx, ctx->cd_prefix_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                            __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0,
                                            NULL);
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
            int rc = walk_inode_btree_nodes(ctx, ctx->cd_suffix_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                            __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0,
                                            NULL);
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
            int rc = walk_inode_btree_nodes(ctx, ctx->cd_subchannel_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                            __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0,
                                            NULL);
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
            int rc = walk_inode_btree_nodes(ctx, ctx->refcount_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                            __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0,
                                            NULL);
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

    /* Keyset blocks */
    if(ctx->sb.keyset_lba != 0 && ctx->sb.keyset_blocks != 0)
    {
        PROGRESS("keyset blocks");
        for(uint64_t i = 0; i < ctx->sb.keyset_blocks; i++) MARK(ctx->sb.keyset_lba + i);
    }

    /* Pending insert buffer blocks */
    if(ctx->sb.pending_lba != 0 && ctx->sb.pending_blocks != 0)
    {
        PROGRESS("pending buffer blocks");
        for(uint64_t i = 0; i < ctx->sb.pending_blocks; i++) MARK(ctx->sb.pending_lba + i);
    }

    /* Backup superblock at the last block */
    MARK(total_blocks - 1);

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

    /* Dedup data blocks referenced by the persisted pending buffer.
     * These entries have been deferred and not yet drained into the
     * B+Tree, so collect_dedup_blocks (which walks tree leaves) will
     * not see their data blocks.  We must read the on-disk pending
     * buffer and mark each referenced data block. */
    if(ctx->sb.pending_lba != 0 && ctx->sb.pending_blocks != 0)
    {
        PROGRESS("pending dedup data");
        uint64_t pb_buf_size = ctx->sb.pending_blocks * ctx->sb.block_size;
        uint8_t *pb_buf      = malloc((size_t)pb_buf_size);
        if(pb_buf)
        {
            int rc = obmafs3_block_read(ctx, ctx->sb.pending_lba, pb_buf, (size_t)pb_buf_size);
            if(rc == OBMAFS3_OK)
            {
                /* Validate pending persist header */
                struct
                {
                    uint64_t magic;
                    uint64_t count;
                    uint16_t sector_size;
                    uint8_t  _pad[6];
                    uint64_t checksum;
                } __attribute__((packed)) pb_hdr;

                memcpy(&pb_hdr, pb_buf, sizeof(pb_hdr));
                if(pb_hdr.magic == 0x474E49444E455055ULL /* PENDING_PERSIST_MAGIC */
                   && sizeof(pb_hdr) + pb_hdr.count * sizeof(struct dedup_entry) <= pb_buf_size)
                {
                    const struct dedup_entry *pb_entries = (const struct dedup_entry *)(pb_buf + sizeof(pb_hdr));

                    /* Collect unique data block base LBAs */
                    uint64_t *pb_unique  = NULL;
                    uint64_t  pb_ucnt    = 0;
                    uint64_t  pb_ucap    = 0;
                    uint64_t  std_per_dd = ctx->sb.dedup_block_size / ctx->sb.block_size;

                    for(uint64_t i = 0; i < pb_hdr.count; i++)
                    {
                        uint64_t blba = pb_entries[i].block_lba;
                        if(blba == 0) continue;

                        /* Deduplicate */
                        int dup = 0;
                        for(uint64_t j = 0; j < pb_ucnt; j++)
                        {
                            if(pb_unique[j] == blba)
                            {
                                dup = 1;
                                break;
                            }
                        }
                        if(dup) continue;

                        if(pb_ucnt >= pb_ucap)
                        {
                            pb_ucap      = (pb_ucap == 0) ? 64 : pb_ucap * 2;
                            uint64_t *ut = realloc(pb_unique, pb_ucap * sizeof(*ut));
                            if(!ut) break;
                            pb_unique = ut;
                        }
                        pb_unique[pb_ucnt++] = blba;

                        /* Read block header to determine actual allocation.
                         * If this block is already marked (from tree walk),
                         * skip — but MARK is idempotent so we just mark. */
                        uint8_t  hdr_tmp[sizeof(struct block_header)];
                        int      hrc      = obmafs3_block_read(ctx, blba, hdr_tmp, sizeof(hdr_tmp));
                        uint64_t mark_std = std_per_dd; /* fallback: full allocation */
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
                                uint64_t used    = (on_disk + bs - 1) / bs;
                                if(used > std_per_dd) used = std_per_dd;
                                /* Pending data blocks may be partial (last
                                 * block) and keep all std_per_dedup allocated,
                                 * or they may have been compressed by the bg
                                 * pool which frees trailing blocks.  Check if
                                 * trailing blocks are still allocated in the
                                 * on-disk bitmap to decide. */
                                mark_std          = used;
                                /* Also check if the full allocation is present
                                 * by testing the last standard block. */
                                uint64_t last_blk = blba + std_per_dd - 1;
                                if(last_blk < total_blocks)
                                {
                                    int last_set = (expected[last_blk / 8] >> (last_blk % 8)) & 1;
                                    if(!last_set)
                                    {
                                        /* Not already marked — check on-disk bitmap */
                                        const uint8_t *disk_bm = ctx->bitmap;
                                        if(disk_bm)
                                        {
                                            int on_disk_set = (disk_bm[last_blk / 8] >> (last_blk % 8)) & 1;
                                            if(on_disk_set) mark_std = std_per_dd;
                                        }
                                    }
                                }
                            }
                        }
                        for(uint64_t s = 0; s < mark_std; s++) MARK(blba + s);
                    }
                    free(pb_unique);
                }
            }
            free(pb_buf);
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

    /* Free-chain blocks for every B+Tree that uses clump allocation */
    PROGRESS("free node chains");
    {
        /* Helper macro: walk one tree's free chain and mark blocks */
#define MARK_FREE_CHAIN(hdr_ptr, bpn)                                                  \
    do                                                                                 \
    {                                                                                  \
        uint64_t *_fc = NULL;                                                          \
        uint64_t  _fn = 0;                                                             \
        if(collect_free_chain_blocks(ctx, (hdr_ptr), (bpn), &_fc, &_fn) == OBMAFS3_OK) \
        {                                                                              \
            for(uint64_t _i = 0; _i < _fn; _i++) MARK(_fc[_i]);                        \
            free(_fc);                                                                 \
        }                                                                              \
    } while(0)

        /* Catalog tree */
        MARK_FREE_CHAIN(&ctx->catalog_hdr, 1);

        /* Inode tree */
        MARK_FREE_CHAIN(&ctx->inode_hdr, 1);

        /* Overflow tree */
        MARK_FREE_CHAIN(&ctx->overflow_hdr, 1);

        /* Media tag tree */
        if(ctx->sb.media_tag_lba != 0) MARK_FREE_CHAIN(&ctx->media_tag_hdr, 1);

        /* CD prefix tree */
        if(ctx->sb.cd_prefix_lba != 0) MARK_FREE_CHAIN(&ctx->cd_prefix_hdr, 1);

        /* CD suffix tree */
        if(ctx->sb.cd_suffix_lba != 0) MARK_FREE_CHAIN(&ctx->cd_suffix_hdr, 1);

        /* CD subchannel tree */
        if(ctx->sb.cd_subchannel_lba != 0) MARK_FREE_CHAIN(&ctx->cd_subchannel_hdr, 1);

        /* Metadata tree (multi-block nodes) */
        if(ctx->sb.metadata_lba != 0) MARK_FREE_CHAIN(&ctx->metadata_hdr, METADATA_NODE_BLOCKS);

        /* Metadata index tree (multi-block nodes) */
        if(ctx->sb.metadata_idx_lba != 0) MARK_FREE_CHAIN(&ctx->metadata_idx_hdr, METADATA_NODE_BLOCKS);

        /* Refcount tree */
        if(ctx->sb.refcount_lba != 0) MARK_FREE_CHAIN(&ctx->refcount_hdr, 1);

        /* Dedup trees (each has its own header and free chain) */
        if(ctx->sb.dedup_lba != 0)
        {
            uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
            if(list_buf)
            {
                int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
                if(rc == OBMAFS3_OK)
                {
                    struct tree_list_header lh;
                    memcpy(&lh, list_buf, sizeof(lh));
                    if(lh.magic == OBMAFS3_TREELIST_MAGIC)
                    {
                        for(uint64_t t = 0; t < lh.tree_count; t++)
                        {
                            struct tree_list_entry te;
                            memcpy(&te, list_buf + sizeof(struct tree_list_header) + t * sizeof(te), sizeof(te));
                            struct btree_header thdr;
                            if(obmafs3_btree_header_read(ctx, te.tree_lba, &thdr) == OBMAFS3_OK)
                                MARK_FREE_CHAIN(&thdr, 1);
                        }
                    }
                }
                free(list_buf);
            }
        }

#undef MARK_FREE_CHAIN
    }

#undef MARK

    /* Clear the progress line */
    bar_clear();

#undef PROGRESS

    *out_error = 0;
    return expected;
}
