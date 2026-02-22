// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_defrag.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Tree defragmentation for obmafsck.
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
/*  Tree defragmentation                                               */
/* ------------------------------------------------------------------ */

/**
 * LBA relocation map entry for defragmentation.
 */
struct defrag_map_entry
{
    uint64_t old_lba;
    uint64_t new_lba;
};

/**
 * Compare two @c defrag_map_entry by @c old_lba for @c qsort / @c bsearch.
 */
static int defrag_map_cmp(const void *a, const void *b)
{
    uint64_t la = ((const struct defrag_map_entry *)a)->old_lba;
    uint64_t lb = ((const struct defrag_map_entry *)b)->old_lba;
    return (la < lb) ? -1 : (la > lb) ? 1 : 0;
}

/**
 * Look up an old LBA in the sorted relocation map.
 *
 * @param map       Sorted array of map entries.
 * @param map_count Number of entries.
 * @param old_lba   LBA to look up.
 * @return The new LBA, or @p old_lba if not found.
 */
static uint64_t defrag_map_lookup(const struct defrag_map_entry *map, uint64_t map_count, uint64_t old_lba)
{
    struct defrag_map_entry key = {.old_lba = old_lba, .new_lba = 0};
    const struct defrag_map_entry *found =
        bsearch(&key, map, (size_t)map_count, sizeof(*map), defrag_map_cmp);
    return found ? found->new_lba : old_lba;
}

/**
 * Compare two @c uint64_t values for qsort.
 */
static int u64_cmp(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

/**
 * Defragment a single B+Tree by relocating all active nodes into a
 * contiguous block range.
 *
 * The tree header block itself is NOT moved — only its contents
 * (root_node_lba, free chain, etc.) are updated.
 *
 * @param ctx              Filesystem context (bitmap must be loaded).
 * @param tree_name        Human-readable tree name for messages.
 * @param hdr_lba          LBA of the btree_header block.
 * @param hdr              Pointer to the in-memory header (updated on success).
 * @param index_entry_size Size of each index entry in bytes.
 * @param child_lba_off    Byte offset of child_lba within the index entry.
 * @param blocks_per_node  Number of contiguous blocks per node (1 or METADATA_NODE_BLOCKS).
 * @param auto_yes         Auto-accept repairs.
 * @param auto_no          Auto-decline repairs.
 * @return 0 on success or skip, -1 on fatal error.
 */
static int defrag_tree(struct obmafs3_ctx *ctx, const char *tree_name, uint64_t hdr_lba, struct btree_header *hdr,
                       size_t index_entry_size, size_t child_lba_off, int blocks_per_node, int auto_yes, int auto_no)
{
    if(hdr->root_node_lba == 0)
    {
        printf("    %s: %s empty\n", tree_name, SYM_SKIP);
        return 0;
    }

    if(!ctx->bitmap)
    {
        printf("    %s: %s bitmap not loaded, skipping\n", tree_name, SYM_WARN);
        return 0;
    }

    size_t   bsz     = (size_t)ctx->sb.block_size;
    size_t   node_sz = (size_t)blocks_per_node * bsz;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return -1;

    /* ---- Phase 1: BFS to collect active nodes ---- */
    uint64_t *active     = NULL;
    uint64_t  active_cnt = 0;
    uint64_t  active_cap = 0;

    uint64_t *queue  = malloc(64 * sizeof(uint64_t));
    uint64_t  q_head = 0, q_tail = 0, q_cap = 64;
    if(!queue) { free(buf); return -1; }

    queue[q_tail++] = hdr->root_node_lba;

    while(q_head < q_tail)
    {
        uint64_t lba = queue[q_head++];

        /* Grow active array */
        if(active_cnt >= active_cap)
        {
            active_cap        = active_cap == 0 ? 64 : active_cap * 2;
            uint64_t *tmp     = realloc(active, active_cap * sizeof(*tmp));
            if(!tmp) { free(buf); free(queue); free(active); return -1; }
            active = tmp;
        }
        active[active_cnt++] = lba;

        /* Read node */
        for(int b = 0; b < blocks_per_node; b++)
        {
            int rc = obmafs3_block_read(ctx, lba + (uint64_t)b, buf + (size_t)b * bsz, bsz);
            if(rc != OBMAFS3_OK) { free(buf); free(queue); free(active); return -1; }
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf); free(queue); free(active);
            return -1;
        }

        /* Enqueue children left-to-right (index nodes only) */
        if(nhdr.level > 0)
        {
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                uint64_t child_lba;
                memcpy(&child_lba,
                       buf + sizeof(struct btree_node_header) + (size_t)i * index_entry_size + child_lba_off,
                       sizeof(child_lba));
                if(q_tail >= q_cap)
                {
                    q_cap *= 2;
                    uint64_t *tmp = realloc(queue, q_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(queue); free(active); return -1; }
                    queue = tmp;
                }
                queue[q_tail++] = child_lba;
            }
        }
    }
    free(queue);

    if(active_cnt == 0)
    {
        printf("    %s: %s no active nodes\n", tree_name, SYM_SKIP);
        free(buf); free(active);
        return 0;
    }

    /* ---- Phase 2: Check if already contiguous ---- */
    {
        uint64_t *sorted = malloc(active_cnt * sizeof(*sorted));
        if(!sorted) { free(buf); free(active); return -1; }
        memcpy(sorted, active, active_cnt * sizeof(*sorted));
        qsort(sorted, (size_t)active_cnt, sizeof(*sorted), u64_cmp);

        int contiguous = 1;
        for(uint64_t i = 1; i < active_cnt; i++)
        {
            if(sorted[i] != sorted[0] + i * (uint64_t)blocks_per_node)
            {
                contiguous = 0;
                break;
            }
        }
        free(sorted);

        if(contiguous)
        {
            printf("    %s: %s already contiguous (%" PRIu64 " nodes)\n",
                   tree_name, SYM_OK, active_cnt);
            free(buf); free(active);
            return 0;
        }
    }

    /* ---- Phase 3: Find contiguous free region ---- */
    uint64_t needed = active_cnt * (uint64_t)blocks_per_node;
    uint64_t new_start;
    int      rc = obmafs3_bitmap_find_free(ctx, needed, &new_start);
    if(rc != OBMAFS3_OK)
    {
        printf("    %s: %s cannot find %" PRIu64 " contiguous free blocks, skipping\n",
               tree_name, SYM_WARN, needed);
        free(buf); free(active);
        return 0;
    }

    /* ---- Phase 4: Ask user ---- */
    printf("    %s: %" PRIu64 " active nodes can be packed into %" PRIu64
           " contiguous blocks at LBA %" PRIu64 "\n",
           tree_name, active_cnt, needed, new_start);

    {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "Defragment %s tree?", tree_name);
        if(!ask_fix(auto_yes, auto_no, prompt))
        {
            free(buf); free(active);
            return 0;
        }
    }

    /* ---- Phase 5: Build old→new LBA relocation map ---- */
    struct defrag_map_entry *map = malloc(active_cnt * sizeof(*map));
    if(!map) { free(buf); free(active); return -1; }

    for(uint64_t i = 0; i < active_cnt; i++)
    {
        map[i].old_lba = active[i];
        map[i].new_lba = new_start + i * (uint64_t)blocks_per_node;
    }

    /* Sort by old_lba for binary-search lookups */
    qsort(map, (size_t)active_cnt, sizeof(*map), defrag_map_cmp);

    /* Mark destination blocks as allocated BEFORE any writes */
    obmafs3_bitmap_set(ctx, new_start, needed);

    /* ---- Phase 6: Read, remap, write each node ---- */
    for(uint64_t i = 0; i < active_cnt; i++)
    {
        uint64_t old_lba = active[i];
        uint64_t new_lba = new_start + i * (uint64_t)blocks_per_node;

        if(active_cnt > 10)
        {
            char pfx[64];
            snprintf(pfx, sizeof(pfx), "Defrag %s", tree_name);
            print_bar(pfx, i + 1, active_cnt);
        }

        /* Read from old location */
        for(int b = 0; b < blocks_per_node; b++)
        {
            rc = obmafs3_block_read(ctx, old_lba + (uint64_t)b, buf + (size_t)b * bsz, bsz);
            if(rc != OBMAFS3_OK)
            {
                fprintf(stderr, "    Error reading LBA %" PRIu64 ": %d\n", old_lba + (uint64_t)b, rc);
                free(map); free(buf); free(active);
                return -1;
            }
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        /* Remap sibling links */
        if(nhdr.left_link != 0)
            nhdr.left_link = defrag_map_lookup(map, active_cnt, nhdr.left_link);
        if(nhdr.right_link != 0)
            nhdr.right_link = defrag_map_lookup(map, active_cnt, nhdr.right_link);
        if(nhdr.overflow_link != 0)
            nhdr.overflow_link = defrag_map_lookup(map, active_cnt, nhdr.overflow_link);

        /* Remap child pointers in index nodes */
        if(nhdr.level > 0)
        {
            for(uint16_t k = 0; k < nhdr.node_keys; k++)
            {
                size_t   off = sizeof(struct btree_node_header) + (size_t)k * index_entry_size + child_lba_off;
                uint64_t child;
                memcpy(&child, buf + off, sizeof(child));
                child = defrag_map_lookup(map, active_cnt, child);
                memcpy(buf + off, &child, sizeof(child));
            }
        }

        /* Write updated header back into buffer */
        memcpy(buf, &nhdr, sizeof(nhdr));

        /* Recompute node checksum */
        size_t data_size = sizeof(struct btree_node_header) + nhdr.keys_length;
        memset(buf + __builtin_offsetof(struct btree_node_header, checksum), 0, 32);
        uint8_t computed[32];
        obmafs3_checksum_block(buf, data_size, computed);
        memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), computed, 32);

        /* Write to new location */
        for(int b = 0; b < blocks_per_node; b++)
        {
            rc = obmafs3_block_write(ctx, new_lba + (uint64_t)b, buf + (size_t)b * bsz, bsz);
            if(rc != OBMAFS3_OK)
            {
                fprintf(stderr, "    Error writing LBA %" PRIu64 ": %d\n", new_lba + (uint64_t)b, rc);
                free(map); free(buf); free(active);
                return -1;
            }
        }
    }

    if(active_cnt > 10) bar_clear();

    /* ---- Phase 7: Free old blocks in bitmap ---- */
    for(uint64_t i = 0; i < active_cnt; i++)
        obmafs3_bitmap_clear(ctx, active[i], (uint64_t)blocks_per_node);

    /* Also free any old free-chain blocks */
    {
        uint64_t fc = hdr->free_node_lba;
        while(fc != 0)
        {
            obmafs3_bitmap_clear(ctx, fc, (uint64_t)blocks_per_node);
            uint8_t fc_buf[8];
            rc = obmafs3_block_read(ctx, fc, fc_buf, sizeof(fc_buf));
            if(rc != OBMAFS3_OK) break;
            uint64_t next;
            memcpy(&next, fc_buf, sizeof(next));
            fc = next;
        }
    }

    /* ---- Phase 8: Update btree header ---- */
    hdr->root_node_lba = new_start; /* BFS index 0 = root */
    hdr->free_node_lba = 0;
    hdr->free_nodes    = 0;
    hdr->total_nodes   = (uint32_t)active_cnt;

    rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "    Error writing %s header: %d\n", tree_name, rc);
        free(map); free(buf); free(active);
        return -1;
    }

    /* ---- Phase 9: Persist bitmap ---- */
    rc = obmafs3_bitmap_write(ctx);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "    Error writing bitmap: %d\n", rc);
        free(map); free(buf); free(active);
        return -1;
    }

    result_fixed("Defragment:", "%s — %" PRIu64 " nodes → contiguous at LBA %" PRIu64,
                 tree_name, active_cnt, new_start);

    free(map);
    free(buf);
    free(active);
    return 0;
}

/**
 * Defragment all B+Trees in the filesystem (excluding dedup data blocks).
 *
 * For each tree that exists and has active nodes, attempts to relocate
 * all nodes into a contiguous block range.  Dedup sub-trees (from the
 * tree list) are defragmented individually.
 *
 * @param ctx       Filesystem context (bitmap must be loaded).
 * @param auto_yes  Auto-accept.
 * @param auto_no   Auto-decline.
 */
void defrag_all_trees(struct obmafs3_ctx *ctx, int auto_yes, int auto_no)
{
    printf("  Checking single-block trees:\n");

    /* Catalog */
    if(ctx->sb.catalog_lba != 0)
        defrag_tree(ctx, "catalog", ctx->sb.catalog_lba, &ctx->catalog_hdr,
                    sizeof(struct catalog_index_entry),
                    __builtin_offsetof(struct catalog_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* Inode */
    if(ctx->sb.inode_lba != 0)
        defrag_tree(ctx, "inode", ctx->sb.inode_lba, &ctx->inode_hdr,
                    sizeof(struct btree_index_entry),
                    __builtin_offsetof(struct btree_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* Overflow */
    if(ctx->sb.overflow_lba != 0)
        defrag_tree(ctx, "overflow", ctx->sb.overflow_lba, &ctx->overflow_hdr,
                    sizeof(struct overflow_index_entry),
                    __builtin_offsetof(struct overflow_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* Media tag */
    if(ctx->sb.media_tag_lba != 0)
        defrag_tree(ctx, "media tag", ctx->sb.media_tag_lba, &ctx->media_tag_hdr,
                    sizeof(struct media_tag_index_entry),
                    __builtin_offsetof(struct media_tag_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* CD prefix */
    if(ctx->sb.cd_prefix_lba != 0)
        defrag_tree(ctx, "CD prefix", ctx->sb.cd_prefix_lba, &ctx->cd_prefix_hdr,
                    sizeof(struct btree_index_entry),
                    __builtin_offsetof(struct btree_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* CD suffix */
    if(ctx->sb.cd_suffix_lba != 0)
        defrag_tree(ctx, "CD suffix", ctx->sb.cd_suffix_lba, &ctx->cd_suffix_hdr,
                    sizeof(struct btree_index_entry),
                    __builtin_offsetof(struct btree_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* CD subchannel */
    if(ctx->sb.cd_subchannel_lba != 0)
        defrag_tree(ctx, "CD subchannel", ctx->sb.cd_subchannel_lba, &ctx->cd_subchannel_hdr,
                    sizeof(struct btree_index_entry),
                    __builtin_offsetof(struct btree_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* Refcount */
    if(ctx->sb.refcount_lba != 0)
        defrag_tree(ctx, "refcount", ctx->sb.refcount_lba, &ctx->refcount_hdr,
                    sizeof(struct btree_index_entry),
                    __builtin_offsetof(struct btree_index_entry, child_lba),
                    1, auto_yes, auto_no);

    /* Multi-block trees */
    printf("  Checking multi-block trees:\n");

    /* Metadata (8 blocks per node) */
    if(ctx->sb.metadata_lba != 0)
        defrag_tree(ctx, "metadata", ctx->sb.metadata_lba, &ctx->metadata_hdr,
                    sizeof(struct metadata_index_entry),
                    __builtin_offsetof(struct metadata_index_entry, child_lba),
                    METADATA_NODE_BLOCKS, auto_yes, auto_no);

    /* Metadata index (8 blocks per node) */
    if(ctx->sb.metadata_idx_lba != 0)
        defrag_tree(ctx, "metadata index", ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr,
                    sizeof(struct metadata_idx_index_entry),
                    __builtin_offsetof(struct metadata_idx_index_entry, child_lba),
                    METADATA_NODE_BLOCKS, auto_yes, auto_no);

    /* Dedup sub-trees */
    if(ctx->sb.dedup_lba != 0)
    {
        printf("  Checking dedup sub-trees:\n");

        uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(list_buf)
        {
            int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
            if(rc == OBMAFS3_OK)
            {
                struct tree_list_header tlh;
                memcpy(&tlh, list_buf, sizeof(tlh));
                if(tlh.magic == OBMAFS3_TREELIST_MAGIC && tlh.tree_count > 0)
                {
                    struct tree_list_entry *entries =
                        malloc((size_t)(tlh.tree_count * sizeof(struct tree_list_entry)));
                    if(entries)
                    {
                        memcpy(entries, list_buf + sizeof(struct tree_list_header),
                               (size_t)(tlh.tree_count * sizeof(struct tree_list_entry)));

                        for(uint64_t t = 0; t < tlh.tree_count; t++)
                        {
                            struct btree_header thdr;
                            rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
                            if(rc != OBMAFS3_OK) continue;

                            char name[64];
                            snprintf(name, sizeof(name), "dedup[%" PRIu64 "] (sector %u)",
                                     t, entries[t].sector_size);

                            defrag_tree(ctx, name, entries[t].tree_lba, &thdr,
                                        sizeof(struct btree_index_entry),
                                        __builtin_offsetof(struct btree_index_entry, child_lba),
                                        1, auto_yes, auto_no);
                        }
                        free(entries);
                    }
                }
            }
            free(list_buf);
        }
    }
}

/* ------------------------------------------------------------------ */
