// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Entry point for the OBMAFS3 filesystem checker.
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
    int auto_yes         = 0;
    int auto_no          = 0;
    int do_scrub         = 0;
    int do_dedup_stats   = 0;
    int dedup_stats_only = 0;
    int do_verify_hashes = 0;
    int do_defrag        = 0;

    static struct option long_opts[] = {
        {            "help", no_argument, NULL, 'h'},
        {           "scrub", no_argument, NULL, 's'},
        {     "dedup-stats", no_argument, NULL, 'd'},
        {"dedup-stats-only", no_argument, NULL, 'D'},
        {   "verify-hashes", no_argument, NULL, 'v'},
        {          "defrag", no_argument, NULL, 'f'},
        {              NULL,           0, NULL,   0}
    };

    int opt;
    while((opt = getopt_long(argc, argv, "ynsdDvfh", long_opts, NULL)) != -1)
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
            case 'D':
                dedup_stats_only = 1;
                do_dedup_stats   = 1;
                break;
            case 'v':
                do_verify_hashes = 1;
                break;
            case 'f':
                do_defrag = 1;
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

    /* Initialise colour support and timing */
    init_color();
    timer_now(&g_start_time);
    g_phase_num = 0;

    printf("%sobmafsck%s — OBMAFS v3 filesystem checker\n", CLR_BOLD, CLR_RESET);
    printf("%sChecking %s%s\n", CLR_DIM, path, CLR_RESET);

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
                "Warning: primary superblock has bad magic (expected 0x%016" PRIx64 ", found 0x%016" PRIx64 ")\n",
                (uint64_t)OBMAFS3_SB_MAGIC, sb.magic);

        /* Try to recover from the backup superblock at the last block. */
        off_t                 fsize     = (S_ISREG(file_stat.st_mode)) ? file_stat.st_size : lseek(fd, 0, SEEK_END);
        int                   recovered = 0;
        static const uint64_t try_bs[]  = {4096, 512, 1024, 2048, 8192, 16384, 32768, 65536};
        if(fsize > 0)
        {
            for(int i = 0; i < (int)(sizeof(try_bs) / sizeof(try_bs[0])); i++)
            {
                uint64_t bs = try_bs[i];
                if((uint64_t)fsize < 2 * bs) continue;
                struct obmafs3_sb backup;
                if(obmafs3_sb_read_backup(fd, bs, (uint64_t)fsize, &backup) == OBMAFS3_OK &&
                   obmafs3_sb_validate(&backup) == OBMAFS3_OK && backup.block_size == bs &&
                   backup.total_bytes == (uint64_t)fsize)
                {
                    fprintf(stderr, "  Recovered superblock from backup (block_size=%" PRIu64 ")\n", bs);
                    sb        = backup;
                    recovered = 1;

                    /* Restore primary from the backup */
                    if(pwrite(fd, &sb, sizeof(sb), 0) == sizeof(sb))
                        fprintf(stderr, "  Primary superblock restored from backup.\n");
                    else
                        fprintf(stderr, "  Warning: could not restore primary superblock.\n");
                    break;
                }
            }
        }
        if(!recovered)
        {
            fprintf(stderr,
                    "Error: '%s' does not contain an OBMAFS3 filesystem\n"
                    "  (primary magic bad and no valid backup found)\n",
                    path);
            close(fd);
            return 1;
        }
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
    pthread_rwlock_init(&ctx->tree_lock, NULL);

    /* Force lazy TLS allocation so the library has buffers to work with */
    struct obmafs3_thread_bufs *tb = obmafs3_get_thread_bufs(ctx);

    ctx->rc_leaf_buf   = malloc((size_t)sb.block_size);
    ctx->rc_leaf_valid = 0;

    if(!tb || !tb->hdr_buf || !tb->node_buf || !tb->io_buf || !tb->io_buf2 || !tb->comp_buf || !tb->zstd_cctx ||
       !tb->zstd_dctx || !ctx->rc_leaf_buf)
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
        fprintf(stderr, "Warning: cannot read catalog tree header at LBA %" PRIu64 " (error %d)\n", sb.catalog_lba, rc);

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

    /* ---- Dedup-stats-only fast path: skip all integrity checks ---- */
    if(dedup_stats_only)
    {
        phase_begin("Dedup statistics");
        rc = compute_dedup_stats(ctx);
        if(rc != OBMAFS3_OK) fprintf(stderr, "Warning: could not compute dedup stats: %d\n", rc);
        phase_end();

        /* Summary */
        {
            char            total_dur[32];
            struct timespec now;
            timer_now(&now);
            fmt_duration(timer_elapsed(&g_start_time, &now), total_dur, sizeof(total_dur));
            printf("\n%s── Summary%s\n", CLR_BOLD, CLR_RESET);
            printf("  %sCompleted in %s%s\n", CLR_DIM, total_dur, CLR_RESET);
        }

        obmafs3_close(ctx);
        return 0;
    }

    /* ---- Superblock ---- */
    phase_begin("Superblock");
    if(ctx->sb.magic == OBMAFS3_SB_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->sb.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->sb.magic);

    /* Verify superblock checksum */
    {
        uint8_t stored[32], computed[32];
        memcpy(stored, ctx->sb.checksum, 32);
        memset(ctx->sb.checksum, 0, 32);
        obmafs3_checksum_block(&ctx->sb, sizeof(ctx->sb), computed);
        memcpy(ctx->sb.checksum, stored, 32);
        int sb_cs_ok = (memcmp(stored, computed, 32) == 0);
        if(sb_cs_ok)
            result_ok("Checksum:", "");
        else
        {
            result_bad("Checksum:", "mismatch");
            errors++;
            if(ask_fix(auto_yes, auto_no, "Recompute superblock checksum?"))
            {
                memset(ctx->sb.checksum, 0, 32);
                obmafs3_checksum_block(&ctx->sb, sizeof(ctx->sb), ctx->sb.checksum);
                ssize_t nn = pwrite(fd, &ctx->sb, sizeof(ctx->sb), 0);
                if(nn < 0 || (size_t)nn != sizeof(ctx->sb))
                    fprintf(stderr, "  %sError: could not write superblock checksum fix%s\n", CLR_RED, CLR_RESET);
                else
                {
                    result_fixed("Checksum:", "recomputed");
                    errors--;
                    /* Also update the backup superblock */
                    if(ctx->sb.total_bytes > 0 && ctx->sb.block_size > 0)
                    {
                        uint64_t blba = OBMAFS3_BACKUP_SB_LBA(ctx->sb.total_bytes, ctx->sb.block_size);
                        if(blba > 0) pwrite(fd, &ctx->sb, sizeof(ctx->sb), (off_t)(blba * ctx->sb.block_size));
                    }
                }
            }
        }
    }

    result_info("Block size:", "%" PRIu64, ctx->sb.block_size);
    result_info("Dedup block size:", "%" PRIu64, ctx->sb.dedup_block_size);
    result_info("Total bytes:", "%" PRIu64, ctx->sb.total_bytes);
    result_info("Volume label:", "%s", ctx->sb.volume_label);

    if(ctx->sb.magic != OBMAFS3_SB_MAGIC) errors++;

    validate_superblock_fields(&ctx->sb, fd, (uint64_t)file_stat.st_size, auto_yes, auto_no, &errors);

    /* ---- Backup superblock ---- */
    if(ctx->sb.total_bytes > 0 && ctx->sb.block_size > 0)
    {
        uint64_t          backup_lba = OBMAFS3_BACKUP_SB_LBA(ctx->sb.total_bytes, ctx->sb.block_size);
        struct obmafs3_sb backup_sb;
        int               backup_cs_ok = 0;
        int               backup_rc =
            obmafs3_sb_read_backup_lenient(fd, ctx->sb.block_size, ctx->sb.total_bytes, &backup_sb, &backup_cs_ok);

        printf("  %sBackup (LBA %" PRIu64 "):%s\n", CLR_DIM, backup_lba, CLR_RESET);

        if(backup_rc != OBMAFS3_OK)
        {
            result_bad("Read:", "failed (rc=%d)", backup_rc);
            errors++;
            if(ask_fix(auto_yes, auto_no, "Write backup superblock from primary?"))
            {
                /* Recompute checksum on the primary and write as backup */
                struct obmafs3_sb tmp = ctx->sb;
                memset(tmp.checksum, 0, sizeof(tmp.checksum));
                obmafs3_checksum_block(&tmp, sizeof(tmp), tmp.checksum);
                off_t   boff = (off_t)(backup_lba * ctx->sb.block_size);
                ssize_t nn   = pwrite(fd, &tmp, sizeof(tmp), boff);
                if(nn >= 0 && (size_t)nn == sizeof(tmp))
                {
                    result_fixed("Backup:", "written from primary");
                    errors--;
                }
                else
                {
                    fprintf(stderr, "  %sError: could not write backup superblock%s\n", CLR_RED, CLR_RESET);
                }
            }
        }
        else
        {
            if(backup_sb.magic == OBMAFS3_SB_MAGIC)
                result_ok("Magic:", "0x%016" PRIx64, backup_sb.magic);
            else
                result_bad("Magic:", "0x%016" PRIx64, backup_sb.magic);
            if(backup_cs_ok)
                result_ok("Checksum:", "");
            else
                result_bad("Checksum:", "mismatch");

            if(backup_sb.magic != OBMAFS3_SB_MAGIC || !backup_cs_ok)
            {
                errors++;
                if(ask_fix(auto_yes, auto_no, "Overwrite backup superblock from primary?"))
                {
                    struct obmafs3_sb tmp = ctx->sb;
                    memset(tmp.checksum, 0, sizeof(tmp.checksum));
                    obmafs3_checksum_block(&tmp, sizeof(tmp), tmp.checksum);
                    off_t   boff = (off_t)(backup_lba * ctx->sb.block_size);
                    ssize_t nn   = pwrite(fd, &tmp, sizeof(tmp), boff);
                    if(nn >= 0 && (size_t)nn == sizeof(tmp))
                    {
                        result_fixed("Backup:", "overwritten from primary");
                        errors--;
                    }
                    else
                    {
                        fprintf(stderr, "  %sError: could not write backup superblock%s\n", CLR_RED, CLR_RESET);
                    }
                }
            }
            else
            {
                /* Both readable — compare contents (excluding checksum which may differ) */
                struct obmafs3_sb primary_cmp = ctx->sb;
                struct obmafs3_sb backup_cmp  = backup_sb;
                memset(primary_cmp.checksum, 0, sizeof(primary_cmp.checksum));
                memset(backup_cmp.checksum, 0, sizeof(backup_cmp.checksum));

                if(memcmp(&primary_cmp, &backup_cmp, sizeof(struct obmafs3_sb)) == 0) { result_ok("Consistency:", ""); }
                else
                {
                    result_bad("Consistency:", "backup differs from primary");
                    errors++;
                    if(ask_fix(auto_yes, auto_no, "Overwrite backup superblock from primary?"))
                    {
                        struct obmafs3_sb tmp = ctx->sb;
                        memset(tmp.checksum, 0, sizeof(tmp.checksum));
                        obmafs3_checksum_block(&tmp, sizeof(tmp), tmp.checksum);
                        off_t   boff = (off_t)(backup_lba * ctx->sb.block_size);
                        ssize_t nn   = pwrite(fd, &tmp, sizeof(tmp), boff);
                        if(nn >= 0 && (size_t)nn == sizeof(tmp))
                        {
                            result_fixed("Backup:", "synced from primary");
                            errors--;
                        }
                        else
                        {
                            fprintf(stderr, "  %sError: could not write backup superblock%s\n", CLR_RED, CLR_RESET);
                        }
                    }
                }
            }
        }
    }
    phase_end();

    /* ---- Catalog tree ---- */
    phase_begin("B+Tree structures"); /* catalog is first */
    printf("\n  %sCatalog tree%s\n", CLR_BOLD, CLR_RESET);
    if(ctx->catalog_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->catalog_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->catalog_hdr.magic);
    {
        int cat_hdr_cs_ok = 0;
        obmafs3_btree_header_read_lenient(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr, &cat_hdr_cs_ok);
        if(cat_hdr_cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
        if(!cat_hdr_cs_ok) errors++;
    }
    result_info("Root node LBA:", "%" PRIu64, ctx->catalog_hdr.root_node_lba);

    if(ctx->catalog_hdr.root_node_lba != 0)
    {
        uint64_t *cat_nodes      = NULL;
        uint64_t  cat_node_count = 0;
        int       wrc = walk_catalog_btree_nodes(ctx, ctx->catalog_hdr.root_node_lba, &cat_nodes, &cat_node_count);
        if(wrc == OBMAFS3_OK)
        {
            uint64_t cat_bad = 0, cat_cs_fix = 0;
            verify_btree_node_checksums(ctx, cat_nodes, cat_node_count, "Catalog", auto_yes, auto_no, &cat_bad,
                                        &cat_cs_fix);
            uint64_t cat_ord = 0, cat_fix = 0;
            verify_btree_ordering(ctx, cat_nodes, cat_node_count, ORD_CATALOG, sizeof(struct catalog_record),
                                  sizeof(struct catalog_index_entry), "Catalog", auto_yes, auto_no, &cat_ord, &cat_fix);
            free(cat_nodes);
            if(cat_bad > 0)
            {
                if(cat_cs_fix > 0)
                    result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", cat_bad, cat_cs_fix);
                else
                    result_bad("Node checksums:", "%" PRIu64 " bad", cat_bad);
                errors += (int)(cat_bad - cat_cs_fix);
            }
            else
            {
                result_ok("Node checksums:", "");
            }
            if(cat_ord > 0)
            {
                if(cat_fix > 0)
                    result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", cat_ord, cat_fix);
                else
                    result_bad("Key ordering:", "%" PRIu64 " bad", cat_ord);
                errors += (int)(cat_ord - cat_fix);
            }
            else
            {
                result_ok("Key ordering:", "");
            }
            verify_fix_total_nodes(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, cat_node_count, "Catalog", "  ",
                                   auto_yes, auto_no, &errors);
            verify_fix_free_nodes(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, "Catalog", "  ", auto_yes, auto_no,
                                  &errors);
            {
                uint64_t sib_bad = 0, sib_fix = 0;
                verify_fix_sibling_links(ctx, ctx->catalog_hdr.root_node_lba, sizeof(struct catalog_index_entry),
                                         __builtin_offsetof(struct catalog_index_entry, child_lba), 1, "Catalog",
                                         auto_yes, auto_no, &sib_bad, &sib_fix);
                if(sib_bad > 0)
                {
                    if(sib_fix > 0)
                        result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                    else
                        result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                    errors += (int)(sib_bad - sib_fix);
                }
                else
                {
                    result_ok("Sibling links:", "");
                }
            }
        }
        else
        {
            result_bad("Node checksums:", "walk failed");
            errors++;
        }
    }

    /* ---- Inode tree ---- */
    printf("\n  %sInode tree%s\n", CLR_BOLD, CLR_RESET);
    if(ctx->inode_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->inode_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->inode_hdr.magic);
    {
        int ino_hdr_cs_ok = 0;
        obmafs3_btree_header_read_lenient(ctx, ctx->sb.inode_lba, &ctx->inode_hdr, &ino_hdr_cs_ok);
        if(ino_hdr_cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
        if(!ino_hdr_cs_ok) errors++;
    }
    result_info("Root node LBA:", "%" PRIu64, ctx->inode_hdr.root_node_lba);

    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint64_t *ino_nodes      = NULL;
        uint64_t  ino_node_count = 0;
        int       wrc = walk_inode_btree_nodes(ctx, ctx->inode_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                               __builtin_offsetof(struct btree_index_entry, child_lba), &ino_nodes,
                                               &ino_node_count, ctx->inode_hdr.total_nodes, "Inode");
        if(wrc == OBMAFS3_OK)
        {
            uint64_t ino_bad = 0, ino_cs_fix = 0;
            verify_btree_node_checksums(ctx, ino_nodes, ino_node_count, "Inode", auto_yes, auto_no, &ino_bad,
                                        &ino_cs_fix);
            uint64_t ino_ord = 0, ino_fix = 0;
            verify_btree_ordering(ctx, ino_nodes, ino_node_count, ORD_UINT64_KEY, sizeof(struct inode_record),
                                  sizeof(struct btree_index_entry), "Inode", auto_yes, auto_no, &ino_ord, &ino_fix);
            free(ino_nodes);
            if(ino_bad > 0)
            {
                if(ino_cs_fix > 0)
                    result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", ino_bad, ino_cs_fix);
                else
                    result_bad("Node checksums:", "%" PRIu64 " bad", ino_bad);
                errors += (int)(ino_bad - ino_cs_fix);
            }
            else
            {
                result_ok("Node checksums:", "");
            }
            if(ino_ord > 0)
            {
                if(ino_fix > 0)
                    result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ino_ord, ino_fix);
                else
                    result_bad("Key ordering:", "%" PRIu64 " bad", ino_ord);
                errors += (int)(ino_ord - ino_fix);
            }
            else
            {
                result_ok("Key ordering:", "");
            }
            verify_fix_total_nodes(ctx, &ctx->inode_hdr, ctx->sb.inode_lba, ino_node_count, "Inode", "  ", auto_yes,
                                   auto_no, &errors);
            verify_fix_free_nodes(ctx, &ctx->inode_hdr, ctx->sb.inode_lba, "Inode", "  ", auto_yes, auto_no, &errors);
            {
                uint64_t sib_bad = 0, sib_fix = 0;
                verify_fix_sibling_links(ctx, ctx->inode_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                         __builtin_offsetof(struct btree_index_entry, child_lba), 1, "Inode", auto_yes,
                                         auto_no, &sib_bad, &sib_fix);
                if(sib_bad > 0)
                {
                    if(sib_fix > 0)
                        result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                    else
                        result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                    errors += (int)(sib_bad - sib_fix);
                }
                else
                {
                    result_ok("Sibling links:", "");
                }
            }
        }
        else
        {
            result_bad("Node checksums:", "could not walk tree");
            errors++;
        }
    }

    /* ---- Overflow tree ---- */
    if(ctx->sb.overflow_lba != 0)
    {
        printf("\n  %sOverflow tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->overflow_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->overflow_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->overflow_hdr.magic);
        {
            int ovf_hdr_cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.overflow_lba, &ctx->overflow_hdr, &ovf_hdr_cs_ok);
            if(ovf_hdr_cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
            if(!ovf_hdr_cs_ok) errors++;
        }

        if(ctx->overflow_hdr.root_node_lba != 0)
        {
            uint64_t *ovf_nodes      = NULL;
            uint64_t  ovf_node_count = 0;
            int wrc = walk_inode_btree_nodes(ctx, ctx->overflow_hdr.root_node_lba, sizeof(struct overflow_index_entry),
                                             __builtin_offsetof(struct overflow_index_entry, child_lba), &ovf_nodes,
                                             &ovf_node_count, ctx->overflow_hdr.total_nodes, "Overflow");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t ovf_bad = 0, ovf_cs_fix = 0;
                verify_btree_node_checksums(ctx, ovf_nodes, ovf_node_count, "Overflow", auto_yes, auto_no, &ovf_bad,
                                            &ovf_cs_fix);
                uint64_t ovf_ord = 0, ovf_fix = 0;
                verify_btree_ordering(ctx, ovf_nodes, ovf_node_count, ORD_OVERFLOW, sizeof(struct overflow_extent),
                                      sizeof(struct overflow_index_entry), "Overflow", auto_yes, auto_no, &ovf_ord,
                                      &ovf_fix);
                free(ovf_nodes);
                if(ovf_bad > 0)
                {
                    if(ovf_cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", ovf_bad, ovf_cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", ovf_bad);
                    errors += (int)(ovf_bad - ovf_cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ovf_ord > 0)
                {
                    if(ovf_fix > 0)
                        result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ovf_ord, ovf_fix);
                    else
                        result_bad("Key ordering:", "%" PRIu64 " bad", ovf_ord);
                    errors += (int)(ovf_ord - ovf_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->overflow_hdr, ctx->sb.overflow_lba, ovf_node_count, "Overflow", "  ",
                                       auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->overflow_hdr, ctx->sb.overflow_lba, "Overflow", "  ", auto_yes,
                                      auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->overflow_hdr.root_node_lba, sizeof(struct overflow_index_entry),
                                             __builtin_offsetof(struct overflow_index_entry, child_lba), 1, "Overflow",
                                             auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Dedup tree list ---- */
    if(ctx->sb.dedup_lba != 0)
    {
        printf("\n  %sDedup tree list%s\n", CLR_BOLD, CLR_RESET);

        uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(list_buf)
        {
            rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
            if(rc == OBMAFS3_OK)
            {
                struct tree_list_header list_hdr;
                memcpy(&list_hdr, list_buf, sizeof(list_hdr));
                if(list_hdr.magic == OBMAFS3_TREELIST_MAGIC)
                    result_ok("Magic:", "0x%016" PRIx64, list_hdr.magic);
                else
                    result_bad("Magic:", "0x%016" PRIx64, list_hdr.magic);
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
                    if(cs_ok)
                        result_ok("Header checksum:", "");
                    else
                        result_bad("Header checksum:", "mismatch");
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
                                int       wrc =
                                    walk_inode_btree_nodes(ctx, thdr.root_node_lba, sizeof(struct btree_index_entry),
                                                           __builtin_offsetof(struct btree_index_entry, child_lba),
                                                           &dd_nodes, &dd_count, thdr.total_nodes, "Dedup");
                                if(wrc == OBMAFS3_OK)
                                {
                                    uint64_t dbad = 0, dcs_fix = 0;
                                    verify_btree_node_checksums(ctx, dd_nodes, dd_count, "Dedup", auto_yes, auto_no,
                                                                &dbad, &dcs_fix);
                                    uint64_t dord = 0, dfix = 0;
                                    verify_btree_ordering(ctx, dd_nodes, dd_count, ORD_UINT64_KEY,
                                                          sizeof(struct dedup_entry), sizeof(struct btree_index_entry),
                                                          "Dedup", auto_yes, auto_no, &dord, &dfix);
                                    free(dd_nodes);
                                    if(dbad > 0)
                                    {
                                        printf("    Node checksums: "
                                               "%" PRIu64 " BAD",
                                               dbad);
                                        if(dcs_fix > 0) printf(" (%" PRIu64 " fixed)", dcs_fix);
                                        printf("\n");
                                        errors += (int)(dbad - dcs_fix);
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
                                    verify_fix_free_nodes(ctx, &thdr, tl_entries[t].tree_lba, "Dedup", "    ", auto_yes,
                                                          auto_no, &errors);
                                    {
                                        uint64_t sib_bad = 0, sib_fix = 0;
                                        verify_fix_sibling_links(
                                            ctx, thdr.root_node_lba, sizeof(struct btree_index_entry),
                                            __builtin_offsetof(struct btree_index_entry, child_lba), 1, "Dedup",
                                            auto_yes, auto_no, &sib_bad, &sib_fix);
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
        printf("\n  %sMedia tag tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->media_tag_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->media_tag_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->media_tag_hdr.magic);
        {
            int mt_hdr_cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr, &mt_hdr_cs_ok);
            if(mt_hdr_cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
            if(!mt_hdr_cs_ok) errors++;
        }

        if(ctx->media_tag_hdr.root_node_lba != 0)
        {
            uint64_t *mt_nodes      = NULL;
            uint64_t  mt_node_count = 0;
            int       wrc =
                walk_inode_btree_nodes(ctx, ctx->media_tag_hdr.root_node_lba, sizeof(struct media_tag_index_entry),
                                       __builtin_offsetof(struct media_tag_index_entry, child_lba), &mt_nodes,
                                       &mt_node_count, ctx->media_tag_hdr.total_nodes, "Media tag");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t mt_bad = 0, mt_cs_fix = 0;
                verify_btree_node_checksums(ctx, mt_nodes, mt_node_count, "Media tag", auto_yes, auto_no, &mt_bad,
                                            &mt_cs_fix);
                uint64_t mt_ord = 0, mt_fix = 0;
                verify_btree_ordering(ctx, mt_nodes, mt_node_count, ORD_MEDIA_TAG, sizeof(struct media_tag_record),
                                      sizeof(struct media_tag_index_entry), "Media tag", auto_yes, auto_no, &mt_ord,
                                      &mt_fix);
                free(mt_nodes);
                if(mt_bad > 0)
                {
                    if(mt_cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", mt_bad, mt_cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", mt_bad);
                    errors += (int)(mt_bad - mt_cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(mt_ord > 0)
                {
                    if(mt_fix > 0)
                        result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", mt_ord, mt_fix);
                    else
                        result_bad("Key ordering:", "%" PRIu64 " bad", mt_ord);
                    errors += (int)(mt_ord - mt_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, mt_node_count, "Media tag",
                                       "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, "Media tag", "  ", auto_yes,
                                      auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->media_tag_hdr.root_node_lba,
                                             sizeof(struct media_tag_index_entry),
                                             __builtin_offsetof(struct media_tag_index_entry, child_lba), 1,
                                             "Media tag", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- CD prefix tree ---- */
    if(ctx->sb.cd_prefix_lba != 0)
    {
        printf("\n  %sCD prefix tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->cd_prefix_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->cd_prefix_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->cd_prefix_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_prefix_lba, &ctx->cd_prefix_hdr, &cs_ok);
            if(cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_prefix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int wrc = walk_inode_btree_nodes(ctx, ctx->cd_prefix_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), &nodes,
                                             &node_count, ctx->cd_prefix_hdr.total_nodes, "CD prefix");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD prefix", auto_yes, auto_no, &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY, sizeof(struct cd_prefix_record),
                                      sizeof(struct btree_index_entry), "CD prefix", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                        result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
                    else
                        result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_prefix_hdr, ctx->sb.cd_prefix_lba, node_count, "CD prefix", "  ",
                                       auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->cd_prefix_hdr, ctx->sb.cd_prefix_lba, "CD prefix", "  ", auto_yes,
                                      auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_prefix_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1, "CD prefix",
                                             auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- CD suffix tree ---- */
    if(ctx->sb.cd_suffix_lba != 0)
    {
        printf("\n  %sCD suffix tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->cd_suffix_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->cd_suffix_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->cd_suffix_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_suffix_lba, &ctx->cd_suffix_hdr, &cs_ok);
            if(cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_suffix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int wrc = walk_inode_btree_nodes(ctx, ctx->cd_suffix_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), &nodes,
                                             &node_count, ctx->cd_suffix_hdr.total_nodes, "CD suffix");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD suffix", auto_yes, auto_no, &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY, sizeof(struct cd_suffix_record),
                                      sizeof(struct btree_index_entry), "CD suffix", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                        result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
                    else
                        result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_suffix_hdr, ctx->sb.cd_suffix_lba, node_count, "CD suffix", "  ",
                                       auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->cd_suffix_hdr, ctx->sb.cd_suffix_lba, "CD suffix", "  ", auto_yes,
                                      auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_suffix_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1, "CD suffix",
                                             auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- CD subchannel tree ---- */
    if(ctx->sb.cd_subchannel_lba != 0)
    {
        printf("\n  %sCD subchannel tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->cd_subchannel_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->cd_subchannel_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->cd_subchannel_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_subchannel_lba, &ctx->cd_subchannel_hdr, &cs_ok);
            if(cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_subchannel_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc =
                walk_inode_btree_nodes(ctx, ctx->cd_subchannel_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                       __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count,
                                       ctx->cd_subchannel_hdr.total_nodes, "CD subchannel");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD subchannel", auto_yes, auto_no, &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY, sizeof(struct cd_subchannel_record),
                                      sizeof(struct btree_index_entry), "CD subchannel", auto_yes, auto_no, &ord,
                                      &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                        result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
                    else
                        result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_subchannel_hdr, ctx->sb.cd_subchannel_lba, node_count,
                                       "CD subchannel", "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->cd_subchannel_hdr, ctx->sb.cd_subchannel_lba, "CD subchannel", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_subchannel_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "CD subchannel", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Metadata tree (per-image key=value) ---- */
    if(ctx->sb.metadata_lba != 0)
    {
        printf("\n  %sMetadata tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->metadata_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->metadata_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->metadata_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr, &cs_ok);
            if(cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
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
                uint64_t bad = 0, cs_fix = 0;
                verify_meta_node_checksums(ctx, nodes, node_count, "Metadata", auto_yes, auto_no, &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_meta_ordering(ctx, nodes, node_count, ORD_METADATA, sizeof(struct metadata_record),
                                     sizeof(struct metadata_index_entry), "Metadata", auto_yes, auto_no, &ord,
                                     &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors++;
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, node_count, "Metadata", "  ",
                                       auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, "Metadata", "  ", auto_yes,
                                      auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->metadata_hdr.root_node_lba, sizeof(struct metadata_index_entry),
                                             __builtin_offsetof(struct metadata_index_entry, child_lba),
                                             METADATA_NODE_BLOCKS, "Metadata", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Metadata index tree (reverse key+value→inode) ---- */
    if(ctx->sb.metadata_idx_lba != 0)
    {
        printf("\n  %sMetadata index tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->metadata_idx_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->metadata_idx_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->metadata_idx_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr, &cs_ok);
            if(cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
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
                uint64_t bad = 0, cs_fix = 0;
                verify_meta_node_checksums(ctx, nodes, node_count, "Metadata index", auto_yes, auto_no, &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_meta_ordering(ctx, nodes, node_count, ORD_METADATA_IDX, sizeof(struct metadata_idx_record),
                                     sizeof(struct metadata_idx_index_entry), "Metadata index", auto_yes, auto_no, &ord,
                                     &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                        result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
                    else
                        result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, node_count,
                                       "Metadata index", "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, "Metadata index", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(
                        ctx, ctx->metadata_idx_hdr.root_node_lba, sizeof(struct metadata_idx_index_entry),
                        __builtin_offsetof(struct metadata_idx_index_entry, child_lba), METADATA_NODE_BLOCKS,
                        "Metadata index", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Metadata bidirectional consistency ---- */
    if(ctx->sb.metadata_lba != 0 && ctx->sb.metadata_idx_lba != 0 && ctx->metadata_hdr.root_node_lba != 0 &&
       ctx->metadata_idx_hdr.root_node_lba != 0)
    {
        check_metadata_bidirectional(ctx, auto_yes, auto_no, &errors);
    }

    /* ---- Refcount tree ---- */
    if(ctx->sb.refcount_lba != 0)
    {
        printf("\n  %sRefcount tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->refcount_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, ctx->refcount_hdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, ctx->refcount_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.refcount_lba, &ctx->refcount_hdr, &cs_ok);
            if(cs_ok)
                result_ok("Header checksum:", "");
            else
                result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->refcount_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int wrc = walk_inode_btree_nodes(ctx, ctx->refcount_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), &nodes,
                                             &node_count, ctx->refcount_hdr.total_nodes, "Refcount");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "Refcount", auto_yes, auto_no, &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY, sizeof(struct refcount_record),
                                      sizeof(struct btree_index_entry), "Refcount", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                        result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
                    else
                        result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                        result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
                    else
                        result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->refcount_hdr, ctx->sb.refcount_lba, node_count, "Refcount", "  ",
                                       auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->refcount_hdr, ctx->sb.refcount_lba, "Refcount", "  ", auto_yes,
                                      auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->refcount_hdr.root_node_lba, sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1, "Refcount",
                                             auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                            result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                        else
                            result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    phase_end();

    /* ---- Phase 3: Cross-references & consistency ---- */
    phase_begin("Cross-references & consistency");

    /* ---- Inode / Catalog cross-reference ---- */
    if(ctx->catalog_hdr.root_node_lba != 0 && ctx->inode_hdr.root_node_lba != 0)
        cross_check_inodes_catalog(ctx, auto_yes, auto_no, &errors);

    /* ---- next_inode_id validation ---- */
    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint64_t *all_ino_ids = NULL;
        uint64_t  all_ino_cnt = 0;
        int       ino_rc      = collect_inode_ids(ctx, &all_ino_ids, &all_ino_cnt);

        printf("\n  %snext_inode_id validation%s\n", CLR_BOLD, CLR_RESET);

        if(ino_rc != OBMAFS3_OK)
        {
            result_bad("Inode tree:", "could not walk (%d)", ino_rc);
            errors++;
        }
        else if(all_ino_cnt == 0) { result_info("Inodes:", "none found"); }
        else
        {
            /* Find the maximum inode ID in use */
            uint64_t max_id = 0;
            for(uint64_t i = 0; i < all_ino_cnt; i++)
            {
                if(all_ino_ids[i] > max_id) max_id = all_ino_ids[i];
            }

            result_info("Highest inode ID:", "%" PRIu64, max_id);
            result_info("next_inode_id:", "%" PRIu64, ctx->sb.next_inode_id);

            if(ctx->sb.next_inode_id <= max_id)
            {
                printf("  ERROR: next_inode_id %" PRIu64 " <= highest inode %" PRIu64 " (would cause ID collisions)\n",
                       ctx->sb.next_inode_id, max_id);
                errors++;

                uint64_t correct = max_id + 1;
                char     prompt[128];
                snprintf(prompt, sizeof(prompt), "  Set next_inode_id to %" PRIu64 "?", correct);

                if(ask_fix(auto_yes, auto_no, prompt))
                {
                    ctx->sb.next_inode_id = correct;

                    /* Recompute superblock checksum and write */
                    memset(ctx->sb.checksum, 0, sizeof(ctx->sb.checksum));
                    obmafs3_checksum_block(&ctx->sb, sizeof(ctx->sb), ctx->sb.checksum);
                    ssize_t nn = pwrite(fd, &ctx->sb, sizeof(ctx->sb), 0);
                    if(nn < 0 || (size_t)nn != sizeof(ctx->sb))
                        fprintf(stderr, "  Error: could not write superblock fix\n");
                    else
                    {
                        printf("  next_inode_id fixed to %" PRIu64 ".\n", correct);
                        errors--;
                        /* Also update the backup superblock */
                        if(ctx->sb.total_bytes > 0 && ctx->sb.block_size > 0)
                        {
                            uint64_t blba = OBMAFS3_BACKUP_SB_LBA(ctx->sb.total_bytes, ctx->sb.block_size);
                            if(blba > 0) pwrite(fd, &ctx->sb, sizeof(ctx->sb), (off_t)(blba * ctx->sb.block_size));
                        }
                    }
                }
            }
            else
            {
                result_ok("Status:", "");
            }
        }

        free(all_ino_ids);
    }

    /* ---- Extent validation ---- */
    if(ctx->inode_hdr.root_node_lba != 0) check_extent_validity(ctx, auto_yes, auto_no, &errors);

    /* ---- Refcount data validation ---- */
    if(ctx->inode_hdr.root_node_lba != 0)
    {
        printf("\n  %sRefcount validation%s\n", CLR_BOLD, CLR_RESET);
        uint64_t rc_bad = 0, rc_fix = 0;
        verify_refcount_tree(ctx, auto_yes, auto_no, &rc_bad, &rc_fix);
        if(rc_bad == 0) { result_ok("Refcounts:", ""); }
        else
        {
            if(rc_fix > 0)
                result_fixed("Refcounts:", "%" PRIu64 " mismatch, %" PRIu64 " fixed", rc_bad, rc_fix);
            else
                result_bad("Refcounts:", "%" PRIu64 " mismatch", rc_bad);
            errors += (int)(rc_bad - rc_fix);
        }
    }

    phase_end();

    /* ---- Phase 4: Allocation bitmap ---- */
    phase_begin("Allocation bitmap");
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

        printf("\n  %sAllocation bitmap%s\n", CLR_BOLD, CLR_RESET);
        if(bhdr.magic == OBMAFS3_BITMAP_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, bhdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, bhdr.magic);
        if(!bhdr_ok || !bitmap_read_ok)
            result_bad("Checksum:", "unreadable");
        else if(checksum_ok)
            result_ok("Checksum:", "");
        else
            result_bad("Checksum:", "mismatch");
        if(!checksum_ok && bitmap_read_ok) errors++;
        result_info("Bitmap LBA:", "%" PRIu64, ctx->sb.bitmap_lba);
        result_info("Bitmap blocks:", "%" PRIu64, ctx->sb.bitmap_blocks);
        result_info("Total blocks:", "%" PRIu64, total_blocks);
        result_info("Allocated:", "%" PRIu64, allocated);
        result_info("Free:", "%" PRIu64, total_blocks - allocated);

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

                if(missing == 0 && extra == 0) { result_ok("Consistency:", ""); }
                else
                {
                    result_bad("Consistency:", "MISMATCH");
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

    phase_end();

    /* ---- Tree defragmentation ---- */
    if(do_defrag)
    {
        phase_begin("Tree defragmentation");
        defrag_all_trees(ctx, auto_yes, auto_no);
        phase_end();
    }

    /* ---- Data block scrub ---- */
    if(do_scrub)
    {
        phase_begin("Data block scrub");
        uint64_t scrub_bad = scrub_data_blocks(ctx);
        if(scrub_bad > 0) errors += (int)scrub_bad;

        uint64_t dedup_bad = scrub_dedup_data_blocks(ctx);
        if(dedup_bad > 0) errors += (int)dedup_bad;

        uint64_t dedup_field_bad = scrub_sector_map_dedup_fields(ctx, auto_yes, auto_no);
        if(dedup_field_bad > 0) errors += (int)dedup_field_bad;
        phase_end();
    }

    /* ---- Hash verification (dedup + CD) ---- */
    if(do_verify_hashes)
    {
        phase_begin("Hash verification");
        uint64_t dedup_hash_bad = verify_dedup_hashes(ctx);
        if(dedup_hash_bad > 0) errors += (int)dedup_hash_bad;

        if(ctx->sb.cd_prefix_lba != 0)
        {
            uint64_t cd_bad = verify_cd_tree_hashes(ctx, &ctx->cd_prefix_hdr, "CD prefix",
                                                    sizeof(struct cd_prefix_record), CD_PREFIX_DATA_SIZE);
            if(cd_bad > 0) errors += (int)cd_bad;
        }

        if(ctx->sb.cd_suffix_lba != 0)
        {
            uint64_t cd_bad = verify_cd_tree_hashes(ctx, &ctx->cd_suffix_hdr, "CD suffix",
                                                    sizeof(struct cd_suffix_record), CD_SUFFIX_DATA_SIZE);
            if(cd_bad > 0) errors += (int)cd_bad;
        }

        if(ctx->sb.cd_subchannel_lba != 0)
        {
            uint64_t cd_bad = verify_cd_tree_hashes(ctx, &ctx->cd_subchannel_hdr, "CD subchannel",
                                                    sizeof(struct cd_subchannel_record), CD_SUBCHANNEL_DATA_SIZE);
            if(cd_bad > 0) errors += (int)cd_bad;
        }
        phase_end();
    }

    /* ---- Dedup statistics ---- */
    if(do_dedup_stats)
    {
        phase_begin("Dedup statistics");
        rc = compute_dedup_stats(ctx);
        if(rc != OBMAFS3_OK) fprintf(stderr, "Warning: could not compute dedup stats: %d\n", rc);
        phase_end();
    }

    /* ---- Summary ---- */
    {
        char            total_dur[32];
        struct timespec now;
        timer_now(&now);
        fmt_duration(timer_elapsed(&g_start_time, &now), total_dur, sizeof(total_dur));
        printf("\n%s── Summary%s\n", CLR_BOLD, CLR_RESET);
        if(errors > 0) { printf("  %s %s%d error(s)%s found.\n", SYM_BAD, CLR_BOLD_RED, errors, CLR_RESET); }
        else
        {
            printf("  %s %sFilesystem is clean.%s\n", SYM_OK, CLR_BOLD_GRN, CLR_RESET);
        }
        printf("  %sCompleted in %s%s\n", CLR_DIM, total_dur, CLR_RESET);
    }

    obmafs3_close(ctx);
    return errors > 0 ? 1 : 0;
}
