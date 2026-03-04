// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_dedup.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Deduplication statistics for obmafsck.
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
/*  LBA hash set — O(1) uniqueness checks (open addressing)            */
/* ------------------------------------------------------------------ */

struct lba_set
{
    uint64_t *slots;    /* 0 = empty sentinel */
    uint64_t  capacity; /* always a power of 2 */
    uint64_t  count;
    uint64_t  mask;     /* capacity - 1 */
};

static struct lba_set *lba_set_create(uint64_t initial_cap)
{
    struct lba_set *s = calloc(1, sizeof(*s));
    if(!s) return NULL;

    /* Round up to power of 2 */
    uint64_t cap = 64;
    while(cap < initial_cap) cap <<= 1;

    s->slots    = calloc((size_t)cap, sizeof(uint64_t));
    if(!s->slots) { free(s); return NULL; }
    s->capacity = cap;
    s->mask     = cap - 1;
    s->count    = 0;
    return s;
}

static void lba_set_grow(struct lba_set *s)
{
    uint64_t  old_cap   = s->capacity;
    uint64_t *old_slots = s->slots;
    uint64_t  new_cap   = old_cap * 2;

    s->slots    = calloc((size_t)new_cap, sizeof(uint64_t));
    s->capacity = new_cap;
    s->mask     = new_cap - 1;
    s->count    = 0;

    if(!s->slots) { s->slots = old_slots; s->capacity = old_cap; s->mask = old_cap - 1; return; }

    /* Re-insert all existing entries */
    for(uint64_t i = 0; i < old_cap; i++)
    {
        if(old_slots[i] != 0)
        {
            uint64_t idx = (old_slots[i] * 0x9E3779B97F4A7C15ULL) & s->mask;
            while(s->slots[idx] != 0)
                idx = (idx + 1) & s->mask;
            s->slots[idx] = old_slots[i];
            s->count++;
        }
    }
    free(old_slots);
}

/**
 * Insert @p lba into the set.
 * @return 1 if the LBA was newly inserted, 0 if it already existed.
 * @note LBA value 0 cannot be stored (used as empty sentinel).
 */
static int lba_set_insert(struct lba_set *s, uint64_t lba)
{
    if(lba == 0) return 0;

    /* Grow at 70% load */
    if(s->count * 10 >= s->capacity * 7)
        lba_set_grow(s);

    uint64_t idx = (lba * 0x9E3779B97F4A7C15ULL) & s->mask;
    for(;;)
    {
        if(s->slots[idx] == 0)   { s->slots[idx] = lba; s->count++; return 1; }
        if(s->slots[idx] == lba) { return 0; }
        idx = (idx + 1) & s->mask;
    }
}

static void lba_set_destroy(struct lba_set *s)
{
    if(!s) return;
    free(s->slots);
    free(s);
}

/** qsort comparator for uint64_t (ascending). */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/* ------------------------------------------------------------------ */
/*  Dedup statistics                                                   */
/* ------------------------------------------------------------------ */

/**
 * Print a byte count as a human-readable string (B/KiB/MiB/GiB/TiB).
 *
 * @param bytes  Number of bytes to format.
 */
void print_human_size(uint64_t bytes)
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
int compute_dedup_stats(struct obmafs3_ctx *ctx)
{
    if(ctx->sb.dedup_lba == 0)
    {
        printf("\n  %sDedup statistics%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup trees found");
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
        printf("\n  %sDedup statistics%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup trees found");
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

        /* Level-order BFS with sorted LBAs for sequential I/O */
        uint64_t *cur_level = malloc(256 * sizeof(uint64_t));
        uint64_t  cur_count = 0, cur_cap = 256;
        uint64_t *nxt_level = malloc(256 * sizeof(uint64_t));
        uint64_t  nxt_count = 0, nxt_cap = 256;

        if(!cur_level || !nxt_level)
        {
            free(cur_level);
            free(nxt_level);
            free(node_buf);
            continue;
        }

        cur_level[cur_count++] = thdr.root_node_lba;

        struct lba_set *seen = lba_set_create(4096);

        uint64_t nodes_visited = 0;

        while(cur_count > 0)
        {
            /* Sort this level's LBAs for sequential I/O */
            if(cur_count > 1)
                qsort(cur_level, (size_t)cur_count, sizeof(uint64_t), cmp_u64);

            nxt_count = 0;

            /* Prefetch window: advise ahead in batches while reading */
            #define NODE_PREFETCH_BATCH 256
            uint64_t prefetched_up_to = 0;

            for(uint64_t ci = 0; ci < cur_count; ci++)
            {
                /* Issue prefetch for the next batch if needed */
                if(ci >= prefetched_up_to)
                {
                    uint64_t end = ci + NODE_PREFETCH_BATCH;
                    if(end > cur_count) end = cur_count;
                    for(uint64_t p = ci; p < end; p++)
                        posix_fadvise(ctx->fd, (off_t)(cur_level[p] * ctx->sb.block_size),
                                      (off_t)ctx->sb.block_size, POSIX_FADV_WILLNEED);
                    prefetched_up_to = end;
                }

                uint64_t lba = cur_level[ci];

                nodes_visited++;
                {
                    char pfx[80];
                    snprintf(pfx, sizeof(pfx), "Dedup stats [tree %" PRIu64 "/%" PRIu64 "] nodes",
                             t + 1, tree_count);
                    print_bar(pfx, nodes_visited, (uint64_t)thdr.total_nodes);
                }

                rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK) continue;

                struct btree_node_header nhdr;
                memcpy(&nhdr, node_buf, sizeof(nhdr));
                if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

                if(nhdr.level > 0)
                {
                    for(uint16_t i = 0; i < nhdr.node_keys; i++)
                    {
                        struct btree_index_entry ie;
                        memcpy(&ie, node_buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie),
                               sizeof(ie));
                        if(nxt_count >= nxt_cap)
                        {
                            nxt_cap *= 2;
                            uint64_t *tmp = realloc(nxt_level, nxt_cap * sizeof(*tmp));
                            if(!tmp) break;
                            nxt_level = tmp;
                        }
                        nxt_level[nxt_count++] = ie.child_lba;
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

                    /* O(1) hash-set uniqueness check */
                    if(lba_set_insert(seen, de.block_lba))
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

            #undef NODE_PREFETCH_BATCH

            /* Swap levels */
            uint64_t *tmp_ptr = cur_level;
            cur_level = nxt_level;
            nxt_level = tmp_ptr;
            cur_count = nxt_count;

            uint64_t tmp_cap = cur_cap;
            cur_cap   = nxt_cap;
            nxt_cap   = tmp_cap;
        }

        free(cur_level);
        free(nxt_level);
        free(node_buf);
        lba_set_destroy(seen);

        stats[t].unique_blocks  = base_count;
        stats[t].physical_bytes = 0;

        /* Sort base LBAs for sequential I/O when reading block headers */
        if(base_count > 1)
            qsort(bases, (size_t)base_count, sizeof(uint64_t), cmp_u64);

        /* Read each unique data block header for compression stats
         * and compute actual physical allocation per block.
         * Use raw pread for just the header (58 bytes) instead of
         * full blocks, and prefetch in batches via posix_fadvise. */
        if(base_count > 0)
        {
            #define PREFETCH_BATCH 256

            for(uint64_t b = 0; b < base_count; b++)
            {
                /* Issue prefetch hints in batches */
                if((b % PREFETCH_BATCH) == 0)
                {
                    uint64_t end = b + PREFETCH_BATCH;
                    if(end > base_count) end = base_count;
                    for(uint64_t p = b; p < end; p++)
                        posix_fadvise(ctx->fd, (off_t)(bases[p] * ctx->sb.block_size),
                                      (off_t)sizeof(struct block_header), POSIX_FADV_WILLNEED);
                }

                {
                    char pfx[80];
                    snprintf(pfx, sizeof(pfx), "Dedup stats [tree %" PRIu64 "/%" PRIu64 "] blocks", t + 1,
                             tree_count);
                    print_bar(pfx, b + 1, base_count);
                }

                /* Read only the block header via pread — no need for a full block */
                struct block_header bhdr;
                ssize_t rd = pread(ctx->fd, &bhdr, sizeof(bhdr),
                                   (off_t)(bases[b] * ctx->sb.block_size));
                if(rd < (ssize_t)sizeof(bhdr)) continue;
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

            #undef PREFETCH_BATCH
        }

        free(bases);
    }

    /* Clear progress line */
    bar_clear();

    /* ---- Print report ---- */
    printf("\n  %sDedup statistics%s\n", CLR_BOLD, CLR_RESET);
    printf("  Media image files:        %" PRIu64 "\n", total_media_files);
    printf("  Total logical size:       ");
    print_human_size(total_media_file_size);
    printf(" (%" PRIu64 " bytes)\n", total_media_file_size);
    printf("  Total sector refs:        %" PRIu64 "\n", total_sector_map_entries);
    printf("  Dedup block size:         ");
    print_human_size(ctx->sb.dedup_block_size);
    printf("\n");

    uint64_t grand_dedup_entries       = 0;
    uint64_t grand_unique_blocks       = 0;
    uint64_t grand_unique_sector_bytes = 0;
    uint64_t grand_original            = 0;
    uint64_t grand_compressed          = 0;
    uint64_t grand_physical            = 0;
    uint64_t grand_compressed_blks     = 0;
    uint64_t grand_uncompressed_blks   = 0;

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
