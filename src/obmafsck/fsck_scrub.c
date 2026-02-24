// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_scrub.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Data block scrub (checksum verification) for obmafsck.
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
/*  Scrub: verify checksums of all data blocks                         */
/* ------------------------------------------------------------------ */

/**
 * Print a progress bar (with error count) to stderr.
 *
 * Uses the same visual style as print_bar but appends an error counter.
 *
 * @param label  Activity label (e.g. "Scrubbing data blocks").
 * @param done   Number of items processed so far.
 * @param total  Total number of items.
 * @param bad    Number of errors detected so far.
 */
void print_progress(const char *label, uint64_t done, uint64_t total, uint64_t bad)
{
    const int bar_width = 30;
    double    frac      = total > 0 ? (double)done / (double)total : 1.0;
    if(frac > 1.0) frac = 1.0;
    int pct = (int)(frac * 100.0);

    if(g_use_color)
    {
        static const char *blocks[] = {" ",      "\u258F", "\u258E", "\u258D", "\u258C",
                                       "\u258B", "\u258A", "\u2589", "\u2588"};
        double             filled_f = frac * bar_width;
        int                filled_i = (int)filled_f;
        int                sub      = (int)((filled_f - filled_i) * 8.0);

        fprintf(stderr, "\r  \033[36m%-24s\033[0m ", label);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled_i)
                fprintf(stderr, "\033[36m\u2588\033[0m");
            else if(i == filled_i)
                fprintf(stderr, "\033[36m%s\033[0m", blocks[sub]);
            else
                fprintf(stderr, "\033[2m\u2591\033[0m");
        }
        fprintf(stderr, " %3d%% \033[2m\u00B7\033[0m %" PRIu64 "/%" PRIu64, pct, done, total);
        if(bad > 0) fprintf(stderr, " \033[31m\u00B7 %" PRIu64 " error%s\033[0m", bad, bad == 1 ? "" : "s");
        fprintf(stderr, "  ");
    }
    else
    {
        int filled = (int)(frac * bar_width);
        fprintf(stderr, "\r  %-24s [", label);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled)
                fputc('=', stderr);
            else if(i == filled)
                fputc('>', stderr);
            else
                fputc(' ', stderr);
        }
        fprintf(stderr, "] %3d%% %" PRIu64 "/%" PRIu64, pct, done, total);
        if(bad > 0) fprintf(stderr, " | %" PRIu64 " error%s", bad, bad == 1 ? "" : "s");
        fprintf(stderr, "   ");
    }
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
uint64_t scrub_data_blocks(struct obmafs3_ctx *ctx)
{
    /* Collect all data block LBAs from inode extents */
    if(ctx->inode_hdr.root_node_lba == 0)
    {
        printf("\n  %sData block scrub%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no data blocks to scrub");
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

    printf("\n  %sData block scrub%s\n", CLR_BOLD, CLR_RESET);
    result_info("Blocks to verify:", "%" PRIu64, data_count);

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
        if(i % 64 == 0 || i == data_count - 1) print_progress("Data blocks", i + 1, data_count, bad);

        rc = obmafs3_block_read(ctx, data_lbas[i], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            read_errors++;
            bad++;
        }
    }

    print_progress("Data blocks", data_count, data_count, bad);
    bar_clear();

    if(bad == 0) { result_ok("Result:", ""); }
    else
    {
        result_bad("Result:", "%" PRIu64 " error(s)", bad);
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
uint64_t scrub_dedup_data_blocks(struct obmafs3_ctx *ctx)
{
    if(ctx->sb.dedup_lba == 0)
    {
        printf("\n  %sDedup data block scrub%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup blocks to scrub");
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
        printf("\n  %sDedup data block scrub%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup blocks to scrub");
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

    printf("\n  %sDedup data block scrub%s\n", CLR_BOLD, CLR_RESET);
    result_info("Blocks to verify:", "%" PRIu64, base_count);

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
        if(i % 4 == 0 || i == base_count - 1) print_progress("Dedup blocks", i + 1, base_count, bad);

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

    print_progress("Dedup blocks", base_count, base_count, bad);
    bar_clear();

    if(bad == 0) { result_ok("Result:", ""); }
    else
    {
        result_bad("Result:", "%" PRIu64 " error(s)", bad);
        if(read_errors > 0) printf("    Read errors:    %" PRIu64 "\n", read_errors);
        if(bad_magic > 0) printf("    Bad magic:      %" PRIu64 "\n", bad_magic);
        if(bad_checksum > 0) printf("    Bad checksum:   %" PRIu64 "\n", bad_checksum);
    }

    free(buf);
    free(bases);
    return bad;
}

/* ------------------------------------------------------------------ */
/*  Scrub: verify cached dedup location fields in sector map entries   */
/* ------------------------------------------------------------------ */

/**
 * Verify that dedup_sector_lba / dedup_sector_offset cached in each
 * sector_map_entry and cd_sector_map_entry match the actual dedup tree
 * entries for the corresponding hash.  Mismatches (including zero
 * fields) are reported and optionally fixed.
 *
 * For CD sector map entries, dedup_subchannel_lba / dedup_subchannel_offset
 * are also verified against the subchannel B+Tree.
 *
 * @param ctx       Filesystem context.
 * @param auto_yes  Non-zero to automatically answer yes to all fix prompts.
 * @param auto_no   Non-zero to automatically answer no to all fix prompts.
 * @return Number of mismatches detected.
 */
uint64_t scrub_sector_map_dedup_fields(struct obmafs3_ctx *ctx, int auto_yes, int auto_no)
{
    if(ctx->inode_hdr.root_node_lba == 0)
    {
        printf("\n  %sSector map dedup field scrub%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no inodes to check");
        return 0;
    }

    printf("\n  %sSector map dedup field scrub%s\n", CLR_BOLD, CLR_RESET);

    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!node_buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        return 0;
    }

    /* DFS walk over inode B+Tree to visit every inode */
    uint64_t *stack  = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_sz = 0, stk_cap = 64;
    if(!stack)
    {
        free(node_buf);
        return 0;
    }
    stack[stk_sz++] = ctx->inode_hdr.root_node_lba;

    uint64_t total_checked = 0;
    uint64_t total_fixed   = 0;
    uint64_t total_bad     = 0;
    uint64_t inodes_seen   = 0;

    while(stk_sz > 0)
    {
        uint64_t lba = stack[--stk_sz];
        int      rc  = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;

        struct btree_node_header hdr;
        memcpy(&hdr, node_buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, node_buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
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

        /* Leaf node — examine each inode record */
        for(uint16_t k = 0; k < hdr.node_keys; k++)
        {
            struct inode_record irec;
            memcpy(&irec, node_buf + sizeof(struct btree_node_header) + (size_t)k * sizeof(irec), sizeof(irec));

            if(irec.sector_map_size == 0) continue;

            inodes_seen++;

            int is_cd = (irec.file_type == kFileTypeCompactDiscImage);

            /* Prepare a map inode for reading the sector map data blocks */
            struct inode_record map_inode;
            memcpy(&map_inode, &irec, sizeof(map_inode));

            if(is_cd)
            {
                /* ---- CD sector map entries ---- */
                map_inode.file_size =
                    sizeof(struct sector_map_header) + irec.sector_map_size * sizeof(struct cd_sector_map_entry);

                struct cd_sector_map_entry *entries =
                    malloc((size_t)(irec.sector_map_size * sizeof(struct cd_sector_map_entry)));
                if(!entries) continue;

                rc = obmafs3_read_file_data(ctx, &map_inode, sizeof(struct sector_map_header), entries,
                                            (size_t)(irec.sector_map_size * sizeof(struct cd_sector_map_entry)));
                if(rc != OBMAFS3_OK)
                {
                    free(entries);
                    continue;
                }

                int entries_dirty = 0;

                for(uint64_t s = 0; s < irec.sector_map_size; s++)
                {
                    struct cd_sector_map_entry *e = &entries[s];
                    total_checked++;

                    if(s % 256 == 0 || s == irec.sector_map_size - 1)
                        print_progress("Dedup fields (CD)", total_checked, 0, total_bad);

                    /* Determine data_size for this sector mode */
                    uint16_t data_size;
                    switch((enum obmafs3_cd_sector_mode)e->sector_mode)
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
                            continue;
                    }

                    /* Check dedup_sector fields */
                    struct btree_header dedup_hdr;
                    uint64_t            dedup_hdr_lba;
                    rc = obmafs3_dedup_get_tree(ctx, data_size, &dedup_hdr, &dedup_hdr_lba);
                    if(rc != OBMAFS3_OK) continue;

                    struct dedup_entry de;
                    rc = obmafs3_dedup_lookup(ctx, &dedup_hdr, e->hash, &de);
                    if(rc != OBMAFS3_OK) continue;

                    if(e->dedup_sector_lba != de.block_lba || e->dedup_sector_offset != de.block_offset)
                    {
                        total_bad++;
                        fprintf(stderr,
                                "\n    inode %" PRIu64 " sector %" PRId64 ": dedup_sector mismatch "
                                "(cached %" PRIu64 ":%" PRIu64 " vs tree %" PRIu64 ":%" PRIu64 ")\n",
                                irec.inode_id, e->sector, e->dedup_sector_lba, e->dedup_sector_offset, de.block_lba,
                                de.block_offset);

                        if(ask_fix(auto_yes, auto_no, "    Fix dedup_sector fields?"))
                        {
                            e->dedup_sector_lba    = de.block_lba;
                            e->dedup_sector_offset = de.block_offset;
                            entries_dirty          = 1;
                            total_fixed++;
                        }
                    }

                    /* Check dedup_subchannel fields (if subchannel is recorded) */
                    if(e->subchannel_hash != 0 && ctx->sb.cd_subchannel_lba != 0)
                    {
                        uint64_t sub_leaf_lba = 0, sub_rec_off = 0;
                        rc = obmafs3_cd_subchannel_get_location(ctx, e->subchannel_hash, NULL, &sub_leaf_lba,
                                                                &sub_rec_off);
                        if(rc == OBMAFS3_OK)
                        {
                            if(e->dedup_subchannel_lba != sub_leaf_lba || e->dedup_subchannel_offset != sub_rec_off)
                            {
                                total_bad++;
                                fprintf(stderr,
                                        "\n    inode %" PRIu64 " sector %" PRId64 ": dedup_subchannel mismatch "
                                        "(cached %" PRIu64 ":%" PRIu64 " vs tree %" PRIu64 ":%" PRIu64 ")\n",
                                        irec.inode_id, e->sector, e->dedup_subchannel_lba, e->dedup_subchannel_offset,
                                        sub_leaf_lba, sub_rec_off);

                                if(ask_fix(auto_yes, auto_no, "    Fix dedup_subchannel fields?"))
                                {
                                    e->dedup_subchannel_lba    = sub_leaf_lba;
                                    e->dedup_subchannel_offset = sub_rec_off;
                                    entries_dirty              = 1;
                                    total_fixed++;
                                }
                            }
                        }
                    }
                }

                /* Write back fixed entries */
                if(entries_dirty)
                {
                    map_inode.file_size =
                        sizeof(struct sector_map_header) + irec.sector_map_size * sizeof(struct cd_sector_map_entry);
                    rc = obmafs3_write_file_data(ctx, &map_inode, sizeof(struct sector_map_header), entries,
                                                 (size_t)(irec.sector_map_size * sizeof(struct cd_sector_map_entry)));
                    if(rc != OBMAFS3_OK)
                        fprintf(stderr, "    Error: could not write back fixed CD sector map for inode %" PRIu64 "\n",
                                irec.inode_id);
                }

                free(entries);
            }
            else if(irec.file_type == kFileTypeMediaImage)
            {
                /* ---- Normal sector map entries ---- */
                map_inode.file_size =
                    sizeof(struct sector_map_header) + irec.sector_map_size * sizeof(struct sector_map_entry);

                struct sector_map_entry *entries =
                    malloc((size_t)(irec.sector_map_size * sizeof(struct sector_map_entry)));
                if(!entries) continue;

                rc = obmafs3_read_file_data(ctx, &map_inode, sizeof(struct sector_map_header), entries,
                                            (size_t)(irec.sector_map_size * sizeof(struct sector_map_entry)));
                if(rc != OBMAFS3_OK)
                {
                    free(entries);
                    continue;
                }

                /* All entries share the same sector_size — use the first one */
                if(irec.sector_map_size == 0)
                {
                    free(entries);
                    continue;
                }

                uint16_t ss = entries[0].sector_size;

                struct btree_header dedup_hdr;
                uint64_t            dedup_hdr_lba;
                rc = obmafs3_dedup_get_tree(ctx, ss, &dedup_hdr, &dedup_hdr_lba);
                if(rc != OBMAFS3_OK)
                {
                    free(entries);
                    continue;
                }

                int entries_dirty = 0;

                for(uint64_t s = 0; s < irec.sector_map_size; s++)
                {
                    struct sector_map_entry *e = &entries[s];
                    total_checked++;

                    if(s % 256 == 0 || s == irec.sector_map_size - 1)
                        print_progress("Dedup fields", total_checked, 0, total_bad);

                    struct dedup_entry de;
                    rc = obmafs3_dedup_lookup(ctx, &dedup_hdr, e->hash, &de);
                    if(rc != OBMAFS3_OK) continue;

                    if(e->dedup_sector_lba != de.block_lba || e->dedup_sector_offset != de.block_offset)
                    {
                        total_bad++;
                        fprintf(stderr,
                                "\n    inode %" PRIu64 " sector %" PRId64 ": dedup_sector mismatch "
                                "(cached %" PRIu64 ":%" PRIu64 " vs tree %" PRIu64 ":%" PRIu64 ")\n",
                                irec.inode_id, e->sector, e->dedup_sector_lba, e->dedup_sector_offset, de.block_lba,
                                de.block_offset);

                        if(ask_fix(auto_yes, auto_no, "    Fix dedup_sector fields?"))
                        {
                            e->dedup_sector_lba    = de.block_lba;
                            e->dedup_sector_offset = de.block_offset;
                            entries_dirty          = 1;
                            total_fixed++;
                        }
                    }
                }

                /* Write back fixed entries */
                if(entries_dirty)
                {
                    map_inode.file_size =
                        sizeof(struct sector_map_header) + irec.sector_map_size * sizeof(struct sector_map_entry);
                    rc = obmafs3_write_file_data(ctx, &map_inode, sizeof(struct sector_map_header), entries,
                                                 (size_t)(irec.sector_map_size * sizeof(struct sector_map_entry)));
                    if(rc != OBMAFS3_OK)
                        fprintf(stderr, "    Error: could not write back fixed sector map for inode %" PRIu64 "\n",
                                irec.inode_id);
                }

                free(entries);
            }
        }
    }

    free(stack);
    free(node_buf);

    bar_clear();

    result_info("Inodes checked:", "%" PRIu64, inodes_seen);
    result_info("Entries checked:", "%" PRIu64, total_checked);
    if(total_bad == 0) { result_ok("Result:", "all dedup location fields are correct"); }
    else
    {
        if(total_fixed > 0)
            result_fixed("Result:", "%" PRIu64 " mismatch(es), %" PRIu64 " fixed", total_bad, total_fixed);
        else
            result_bad("Result:", "%" PRIu64 " mismatch(es)", total_bad);
    }

    return total_bad;
}

/* ------------------------------------------------------------------ */
