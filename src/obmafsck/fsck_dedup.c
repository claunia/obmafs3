/*
 * fsck_dedup.c — Dedup statistics for obmafsck.
 */
#include "fsck.h"

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
