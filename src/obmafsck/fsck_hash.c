// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_hash.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Hash verification (dedup + CD prefix/suffix/subchannel) for obmafsck.
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
/*  Hash verification (dedup + CD prefix/suffix/subchannel)            */
/* ------------------------------------------------------------------ */

/**
 * Verify CD prefix/suffix/subchannel B+Tree hashes.
 *
 * Each CD record stores an XXH64 hash alongside inline data.  This
 * function walks the leaf nodes of the given tree and recomputes the
 * hash from the inline data, reporting mismatches.
 *
 * @param ctx        Filesystem context.
 * @param hdr        Cached B+Tree header for the tree.
 * @param label      Human-readable tree name for output (e.g. "CD prefix").
 * @param rec_size   Size of one leaf record (hash + inline data).
 * @param data_size  Size of the inline data portion after the hash.
 * @return Number of hash mismatches detected.
 */
uint64_t verify_cd_tree_hashes(struct obmafs3_ctx *ctx, const struct btree_header *hdr, const char *label,
                               size_t rec_size, size_t data_size)
{
    printf("\n  %s%s hash verification%s\n", CLR_BOLD, label, CLR_RESET);

    if(hdr->root_node_lba == 0)
    {
        result_info("Status:", "no entries to verify");
        return 0;
    }

    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!node_buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        return 0;
    }

    /* First pass: count total entries for progress reporting */
    uint64_t total_entries = 0;
    {
        uint64_t lba = hdr->root_node_lba;
        /* Descend to left-most leaf */
        while(lba != 0)
        {
            int rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                /* Index node: follow first child */
                struct btree_index_entry ie;
                memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                /* Leaf level: walk right links and count */
                while(lba != 0)
                {
                    rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;
                    memcpy(&nhdr, node_buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;
                    total_entries += nhdr.node_keys;
                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

    if(total_entries == 0)
    {
        result_info("Status:", "no entries to verify");
        free(node_buf);
        return 0;
    }

    result_info("Entries to verify:", "%" PRIu64, total_entries);

    /* Second pass: verify hashes */
    uint64_t checked = 0;
    uint64_t bad     = 0;
    uint64_t lba     = hdr->root_node_lba;

    /* Descend to left-most leaf */
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, node_buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        if(nhdr.level > 0)
        {
            struct btree_index_entry ie;
            memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            /* Leaf level: walk right links and verify */
            while(lba != 0)
            {
                rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK) break;
                memcpy(&nhdr, node_buf, sizeof(nhdr));
                if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                const uint8_t *rp = node_buf + sizeof(struct btree_node_header);
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    uint64_t stored_hash;
                    memcpy(&stored_hash, rp + i * rec_size, sizeof(stored_hash));
                    const uint8_t *data_ptr = rp + i * rec_size + sizeof(uint64_t);

                    uint64_t computed = obmafs3_checksum_xxh64(data_ptr, data_size);
                    if(computed != stored_hash) bad++;

                    checked++;
                    if(checked % 256 == 0 || checked == total_entries)
                        print_progress("CD hash verify", checked, total_entries, bad);
                }

                lba = nhdr.right_link;
            }
            break;
        }
    }

    print_progress("CD hash verify", total_entries, total_entries, bad);
    bar_clear();

    if(bad == 0)
        result_ok("Result:", "");
    else
        result_bad("Result:", "%" PRIu64 " hash mismatch(es)", bad);

    free(node_buf);
    return bad;
}

/**
 * Verify dedup entry hashes against the actual stored sector data.
 *
 * Walks all dedup B+Trees (one per sector size).  For each dedup_entry,
 * reads the sector data from the dedup data block at (block_lba,
 * block_offset), recomputes the XXH64 hash, and compares it to the
 * stored hash.
 *
 * @param ctx  Filesystem context.
 * @return Number of hash mismatches detected.
 */
uint64_t verify_dedup_hashes(struct obmafs3_ctx *ctx)
{
    printf("\n  %sDedup hash verification%s\n", CLR_BOLD, CLR_RESET);

    if(ctx->sb.dedup_lba == 0)
    {
        result_info("Status:", "no dedup entries to verify");
        return 0;
    }

    /* Read the tree list header */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        return 0;
    }

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "  Error: could not read dedup tree list: %d\n", rc);
        free(list_buf);
        return 0;
    }

    struct tree_list_header list_hdr;
    memcpy(&list_hdr, list_buf, sizeof(list_hdr));
    if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC || list_hdr.tree_count == 0)
    {
        free(list_buf);
        result_info("Status:", "no dedup entries to verify");
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

    /* First pass: count total dedup entries for progress */
    uint64_t total_entries = 0;
    uint8_t *node_buf      = calloc(1, (size_t)ctx->sb.block_size);
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

        /* Descend to left-most leaf */
        while(lba != 0)
        {
            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                struct btree_index_entry ie;
                memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                while(lba != 0)
                {
                    rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;
                    memcpy(&nhdr, node_buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;
                    total_entries += nhdr.node_keys;
                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

    if(total_entries == 0)
    {
        result_info("Status:", "no dedup entries to verify");
        free(node_buf);
        free(entries);
        return 0;
    }

    result_info("Entries to verify:", "%" PRIu64, total_entries);

    /* Allocate buffers for reading dedup data blocks */
    size_t   dedup_size = (size_t)ctx->sb.dedup_block_size;
    uint8_t *dedup_buf  = calloc(1, dedup_size);
    uint8_t *decomp_buf = NULL;
    if(!dedup_buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        free(node_buf);
        free(entries);
        return 0;
    }

    uint64_t checked           = 0;
    uint64_t bad               = 0;
    uint64_t read_errors       = 0;
    uint64_t cached_dedup_lba  = 0;
    int      cached_compressed = 0;

    /* Second pass: verify each entry */
    for(uint64_t t = 0; t < tree_count; t++)
    {
        uint16_t sector_size = entries[t].sector_size;

        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        uint64_t lba = thdr.root_node_lba;

        /* Descend to left-most leaf */
        while(lba != 0)
        {
            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                struct btree_index_entry ie;
                memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                while(lba != 0)
                {
                    rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;
                    memcpy(&nhdr, node_buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                    const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
                    for(uint16_t i = 0; i < nhdr.node_keys; i++)
                    {
                        struct dedup_entry de;
                        memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));

                        if(de.block_lba == 0)
                        {
                            checked++;
                            continue;
                        }

                        /* Read the dedup data block if not cached */
                        if(de.block_lba != cached_dedup_lba)
                        {
                            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf, (size_t)ctx->sb.block_size);
                            if(rc != OBMAFS3_OK)
                            {
                                read_errors++;
                                bad++;
                                checked++;
                                cached_dedup_lba = 0;
                                if(checked % 256 == 0 || checked == total_entries)
                                    print_progress("Dedup hash verify", checked, total_entries, bad);
                                continue;
                            }

                            struct block_header bhdr;
                            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

                            uint64_t payload_size;
                            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                                payload_size = bhdr.compressed_size;
                            else
                                payload_size = bhdr.original_size;

                            uint64_t total_on_disk = sizeof(bhdr) + payload_size;
                            uint64_t bs            = ctx->sb.block_size;
                            uint64_t needed_std    = (total_on_disk + bs - 1) / bs;

                            if(needed_std > 1)
                            {
                                rc = obmafs3_block_read(ctx, de.block_lba + 1, dedup_buf + bs,
                                                        (size_t)((needed_std - 1) * bs));
                                if(rc != OBMAFS3_OK)
                                {
                                    read_errors++;
                                    bad++;
                                    checked++;
                                    cached_dedup_lba = 0;
                                    if(checked % 256 == 0 || checked == total_entries)
                                        print_progress("Dedup hash verify", checked, total_entries, bad);
                                    continue;
                                }
                            }

                            cached_dedup_lba = de.block_lba;

                            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                            {
                                if(!decomp_buf)
                                {
                                    decomp_buf = malloc(dedup_size);
                                    if(!decomp_buf)
                                    {
                                        fprintf(stderr, "\n  Error: out of memory for decompression buffer\n");
                                        goto done;
                                    }
                                }

                                ZSTD_DCtx *dctx = ZSTD_createDCtx();
                                if(!dctx)
                                {
                                    fprintf(stderr, "\n  Error: cannot create ZSTD decompression context\n");
                                    goto done;
                                }

                                size_t dret =
                                    ZSTD_decompressDCtx(dctx, decomp_buf, dedup_size, dedup_buf + sizeof(bhdr),
                                                        (size_t)bhdr.compressed_size);
                                ZSTD_freeDCtx(dctx);

                                if(ZSTD_isError(dret))
                                {
                                    read_errors++;
                                    bad++;
                                    checked++;
                                    cached_dedup_lba = 0;
                                    if(checked % 256 == 0 || checked == total_entries)
                                        print_progress("Dedup hash verify", checked, total_entries, bad);
                                    continue;
                                }
                                cached_compressed = 1;
                            }
                            else
                            {
                                cached_compressed = 0;
                            }
                        }

                        /* Extract the sector data and compute hash */
                        const uint8_t *sector_data;
                        if(cached_compressed)
                        {
                            size_t decomp_off = (size_t)(de.block_offset - sizeof(struct block_header));
                            sector_data       = decomp_buf + decomp_off;
                        }
                        else
                        {
                            sector_data = dedup_buf + de.block_offset;
                        }

                        uint64_t computed = obmafs3_checksum_xxh64(sector_data, (size_t)sector_size);
                        if(computed != de.hash) bad++;

                        checked++;
                        if(checked % 256 == 0 || checked == total_entries)
                            print_progress("Dedup hash verify", checked, total_entries, bad);
                    }

                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

done:
    print_progress("Dedup hash verify", total_entries, total_entries, bad);
    bar_clear();

    if(bad == 0)
        result_ok("Result:", "");
    else
    {
        result_bad("Result:", "%" PRIu64 " error(s)", bad);
        if(read_errors > 0) printf("    Read/decomp errors: %" PRIu64 "\n", read_errors);
        uint64_t hash_bad = bad - read_errors;
        if(hash_bad > 0) printf("    Hash mismatches:    %" PRIu64 "\n", hash_bad);
    }

    free(decomp_buf);
    free(dedup_buf);
    free(node_buf);
    free(entries);
    return bad;
}

/* ------------------------------------------------------------------ */
