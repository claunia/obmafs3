/*
 * obmafsck - Check and validate an OBMAFS3 filesystem
 */
#include "obmafs.h"

#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Options                                                            */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <device-or-file>\n"
            "\n"
            "Options:\n"
            "  -y            Assume 'yes' to all repair questions\n"
            "  -n            Assume 'no' to all repair questions\n"
            "  -h, --help    Show this help message\n",
            prog);
}

/* Return value: 1 = yes, 0 = no */
static int ask_fix(int auto_yes, int auto_no, const char *prompt)
{
    if (auto_yes) return 1;
    if (auto_no)  return 0;

    fprintf(stdout, "%s [y/n] ", prompt);
    fflush(stdout);

    int ch = fgetc(stdin);
    /* consume rest of line */
    int c2;
    while ((c2 = fgetc(stdin)) != '\n' && c2 != EOF)
        ;
    return (ch == 'y' || ch == 'Y');
}

/* ------------------------------------------------------------------ */
/*  Walk a btree linked list, collecting the LBAs of all nodes         */
/* ------------------------------------------------------------------ */

static int walk_tree_nodes(struct obmafs3_ctx *ctx,
                           uint64_t root_lba,
                           uint64_t **out_lbas,
                           uint64_t *out_count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas = NULL;
    uint64_t count = 0;
    uint64_t cap = 0;
    uint64_t lba = root_lba;

    while (lba != 0) {
        /* grow array */
        if (count >= cap) {
            cap = (cap == 0) ? 64 : cap * 2;
            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
            if (!tmp) { free(buf); free(lbas); return OBMAFS3_ERR_NOMEM; }
            lbas = tmp;
        }
        lbas[count++] = lba;

        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        lba = hdr.right_link;
    }

    free(buf);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Collect data-block LBAs from all inodes                            */
/* ------------------------------------------------------------------ */

static int collect_inode_data_blocks(struct obmafs3_ctx *ctx,
                                     uint64_t inode_root_lba,
                                     uint64_t **out_lbas,
                                     uint64_t *out_count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas = NULL;
    uint64_t count = 0;
    uint64_t cap = 0;
    uint64_t lba = inode_root_lba;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf); free(lbas); return rc;
        }

        struct btree_node_inode node;
        memcpy(&node, buf, sizeof(node));

        if (node.header.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); free(lbas); return OBMAFS3_ERR_BADMAGIC;
        }

        /* collect extent blocks */
        for (int e = 0; e < 8; e++) {
            if (node.extents[e].block_count == 0)
                continue;
            for (uint64_t b = 0; b < node.extents[e].block_count; b++) {
                if (count >= cap) {
                    cap = (cap == 0) ? 128 : cap * 2;
                    uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
                    if (!tmp) {
                        free(buf); free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    lbas = tmp;
                }
                lbas[count++] = node.extents[e].start_block + b;
            }
        }

        lba = node.header.right_link;
    }

    free(buf);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Build expected bitmap                                              */
/* ------------------------------------------------------------------ */

static uint8_t *build_expected_bitmap(struct obmafs3_ctx *ctx,
                                      uint64_t total_blocks,
                                      uint64_t bitmap_bytes,
                                      int *out_error)
{
    uint8_t *expected = calloc(1, (size_t)bitmap_bytes);
    if (!expected) { *out_error = 1; return NULL; }

    /* Helper to set a bit */
    #define MARK(blk) do { \
        if ((blk) < total_blocks) \
            expected[(blk) / 8] |= (1u << ((blk) % 8)); \
    } while (0)

    /* Block 0: superblock */
    MARK(0);

    /* Catalog tree: header + nodes */
    MARK(ctx->sb.catalog_lba);
    {
        uint64_t *cat_nodes = NULL;
        uint64_t cat_count = 0;
        int rc = walk_tree_nodes(ctx, ctx->catalog_hdr.root_node_lba,
                                 &cat_nodes, &cat_count);
        if (rc == OBMAFS3_OK) {
            for (uint64_t i = 0; i < cat_count; i++)
                MARK(cat_nodes[i]);
            free(cat_nodes);
        } else {
            fprintf(stderr, "Warning: could not walk catalog tree nodes\n");
        }
    }

    /* Inode tree: header + nodes */
    MARK(ctx->sb.inode_lba);
    {
        uint64_t *ino_nodes = NULL;
        uint64_t ino_count = 0;
        int rc = walk_tree_nodes(ctx, ctx->inode_hdr.root_node_lba,
                                 &ino_nodes, &ino_count);
        if (rc == OBMAFS3_OK) {
            for (uint64_t i = 0; i < ino_count; i++)
                MARK(ino_nodes[i]);
            free(ino_nodes);
        } else {
            fprintf(stderr, "Warning: could not walk inode tree nodes\n");
        }
    }

    /* Overflow tree header (if present) */
    if (ctx->sb.overflow_lba != 0) {
        MARK(ctx->sb.overflow_lba);
        if (ctx->overflow_hdr.root_node_lba != 0) {
            uint64_t *ovf_nodes = NULL;
            uint64_t ovf_count = 0;
            int rc = walk_tree_nodes(ctx,
                                     ctx->overflow_hdr.root_node_lba,
                                     &ovf_nodes, &ovf_count);
            if (rc == OBMAFS3_OK) {
                for (uint64_t i = 0; i < ovf_count; i++)
                    MARK(ovf_nodes[i]);
                free(ovf_nodes);
            }
        }
    }

    /* Dedup tree list header */
    if (ctx->sb.dedup_lba != 0)
        MARK(ctx->sb.dedup_lba);

    /* Bitmap blocks */
    for (uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++)
        MARK(ctx->sb.bitmap_lba + i);

    /* File data blocks from inode extents */
    if (ctx->inode_hdr.root_node_lba != 0) {
        uint64_t *data_lbas = NULL;
        uint64_t data_count = 0;
        int rc = collect_inode_data_blocks(ctx,
                                           ctx->inode_hdr.root_node_lba,
                                           &data_lbas, &data_count);
        if (rc == OBMAFS3_OK) {
            for (uint64_t i = 0; i < data_count; i++)
                MARK(data_lbas[i]);
            free(data_lbas);
        } else {
            fprintf(stderr,
                    "Warning: could not collect inode data blocks\n");
        }
    }

    #undef MARK

    *out_error = 0;
    return expected;
}

/* ------------------------------------------------------------------ */
/*  Verify btree node checksums while walking the tree                 */
/* ------------------------------------------------------------------ */

static int verify_tree_node_checksums(struct obmafs3_ctx *ctx,
                                      uint64_t root_lba,
                                      const char *tree_name,
                                      uint64_t *bad_count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = root_lba;
    uint64_t bad = 0;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            fprintf(stderr,
                    "  %s node at LBA %" PRIu64 ": bad magic\n",
                    tree_name, lba);
            bad++;
            break; /* can't follow right_link if magic is bad */
        }

        /* Verify node checksum */
        size_t data_size = sizeof(struct btree_node_header) +
                           hdr.keys_length;
        uint8_t stored[32];
        memcpy(stored, hdr.checksum, 32);
        memset(buf + __builtin_offsetof(struct btree_node_header, checksum),
               0, 32);
        uint8_t computed[32];
        obmafs3_checksum_block(buf, data_size, computed);
        /* restore */
        memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum),
               stored, 32);

        if (memcmp(stored, computed, 32) != 0) {
            fprintf(stderr,
                    "  %s node at LBA %" PRIu64 ": checksum mismatch\n",
                    tree_name, lba);
            bad++;
        }

        lba = hdr.right_link;
    }

    free(buf);
    *bad_count = bad;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int auto_yes = 0;
    int auto_no  = 0;

    static struct option long_opts[] = {
        { "help", no_argument, NULL, 'h' },
        { NULL,   0,           NULL,  0  }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "ynh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'y': auto_yes = 1; break;
        case 'n': auto_no  = 1; break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (auto_yes && auto_no) {
        fprintf(stderr, "Error: -y and -n are mutually exclusive\n");
        return 1;
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[optind];

    /* ---- Open the filesystem (skip bitmap so we can check it ourselves) ---- */
    struct obmafs3_ctx *ctx;
    int rc = obmafs3_open_flags(path, OBMAFS3_OPEN_SKIP_BITMAP, &ctx);
    if (rc != OBMAFS3_OK) {
        fprintf(stderr, "Error: failed to open filesystem: %d\n", rc);
        return 1;
    }

    int errors = 0;

    /* ---- Superblock ---- */
    printf("Superblock:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n",
           ctx->sb.magic,
           ctx->sb.magic == OBMAFS3_SB_MAGIC ? "OK" : "BAD");
    printf("  Block size:       %" PRIu64 "\n", ctx->sb.block_size);
    printf("  Dedup block size: %" PRIu64 "\n", ctx->sb.dedup_block_size);
    printf("  Total bytes:      %" PRIu64 "\n", ctx->sb.total_bytes);
    printf("  Volume label:     %s\n", ctx->sb.volume_label);

    if (ctx->sb.magic != OBMAFS3_SB_MAGIC) errors++;

    /* ---- Catalog tree ---- */
    printf("\nCatalog tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n",
           ctx->catalog_hdr.magic,
           ctx->catalog_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC
               ? "OK" : "BAD");
    printf("  Header checksum:  OK\n");
    printf("  Root node LBA:    %" PRIu64 "\n",
           ctx->catalog_hdr.root_node_lba);
    printf("  Total nodes:      %u\n", ctx->catalog_hdr.total_nodes);

    if (ctx->catalog_hdr.root_node_lba != 0) {
        uint64_t cat_bad = 0;
        verify_tree_node_checksums(ctx, ctx->catalog_hdr.root_node_lba,
                                   "Catalog", &cat_bad);
        if (cat_bad > 0) {
            printf("  Node checksums:   %" PRIu64 " BAD\n", cat_bad);
            errors++;
        } else {
            printf("  Node checksums:   OK\n");
        }
    }

    /* ---- Inode tree ---- */
    printf("\nInode tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n",
           ctx->inode_hdr.magic,
           ctx->inode_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC
               ? "OK" : "BAD");
    printf("  Header checksum:  OK\n");
    printf("  Root node LBA:    %" PRIu64 "\n",
           ctx->inode_hdr.root_node_lba);
    printf("  Total nodes:      %u\n", ctx->inode_hdr.total_nodes);

    if (ctx->inode_hdr.root_node_lba != 0) {
        uint64_t ino_bad = 0;
        verify_tree_node_checksums(ctx, ctx->inode_hdr.root_node_lba,
                                   "Inode", &ino_bad);
        if (ino_bad > 0) {
            printf("  Node checksums:   %" PRIu64 " BAD\n", ino_bad);
            errors++;
        } else {
            printf("  Node checksums:   OK\n");
        }
    }

    /* ---- Overflow tree ---- */
    if (ctx->sb.overflow_lba != 0) {
        printf("\nOverflow tree:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n",
               ctx->overflow_hdr.magic,
               ctx->overflow_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC
                   ? "OK" : "BAD");
        printf("  Header checksum:  OK\n");

        if (ctx->overflow_hdr.root_node_lba != 0) {
            uint64_t ovf_bad = 0;
            verify_tree_node_checksums(ctx,
                                       ctx->overflow_hdr.root_node_lba,
                                       "Overflow", &ovf_bad);
            if (ovf_bad > 0) {
                printf("  Node checksums:   %" PRIu64 " BAD\n",
                       ovf_bad);
                errors++;
            } else {
                printf("  Node checksums:   OK\n");
            }
        }
    }

    /* ---- Allocation bitmap ---- */
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;

    if (ctx->sb.bitmap_lba != 0 && ctx->sb.bitmap_blocks != 0) {
        uint64_t bitmap_bytes = (total_blocks + 7) / 8;
        size_t hdr_size = sizeof(struct bitmap_header);

        /* Read bitmap header from first bitmap block */
        uint8_t *bhdr_buf = malloc((size_t)ctx->sb.block_size);
        struct bitmap_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        int bhdr_ok = 0;
        if (bhdr_buf) {
            if (obmafs3_block_read(ctx, ctx->sb.bitmap_lba, bhdr_buf,
                                   (size_t)ctx->sb.block_size) == OBMAFS3_OK) {
                memcpy(&bhdr, bhdr_buf, hdr_size);
                bhdr_ok = 1;
            }
            free(bhdr_buf);
        }

        /* Read the raw bitmap data from disk (manually, since we skipped it) */
        uint8_t *disk_bitmap = calloc(1, (size_t)bitmap_bytes);
        int bitmap_read_ok = 0;
        if (disk_bitmap) {
            uint8_t *blk = malloc((size_t)ctx->sb.block_size);
            if (blk) {
                uint64_t remaining = bitmap_bytes;
                uint64_t offset = 0;
                bitmap_read_ok = 1;
                for (uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++) {
                    if (obmafs3_block_read(ctx, ctx->sb.bitmap_lba + i,
                                           blk, (size_t)ctx->sb.block_size) != OBMAFS3_OK) {
                        bitmap_read_ok = 0;
                        break;
                    }
                    if (i == 0) {
                        size_t avail = (size_t)ctx->sb.block_size - hdr_size;
                        size_t copy = remaining < avail ? (size_t)remaining : avail;
                        memcpy(disk_bitmap, blk + hdr_size, copy);
                        offset += copy;
                        remaining -= copy;
                    } else {
                        size_t copy = remaining < ctx->sb.block_size
                                          ? (size_t)remaining
                                          : (size_t)ctx->sb.block_size;
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
        if (bitmap_read_ok && bhdr_ok) {
            uint8_t computed[32];
            obmafs3_checksum_block(disk_bitmap, (size_t)bitmap_bytes, computed);
            checksum_ok = (memcmp(bhdr.checksum, computed, 32) == 0);
        }

        uint64_t allocated = 0;
        if (bitmap_read_ok) {
            for (uint64_t b = 0; b < total_blocks; b++) {
                if ((disk_bitmap[b / 8] >> (b % 8)) & 1)
                    allocated++;
            }
        }

        printf("\nAllocation bitmap:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n",
               bhdr.magic,
               bhdr.magic == OBMAFS3_BITMAP_MAGIC ? "OK" : "BAD");
        if (!bhdr_ok || !bitmap_read_ok)
            printf("  Checksum:         UNREADABLE\n");
        else
            printf("  Checksum:         %s\n", checksum_ok ? "OK" : "BAD");
        if (!checksum_ok && bitmap_read_ok)
            errors++;
        printf("  Bitmap LBA:       %" PRIu64 "\n", ctx->sb.bitmap_lba);
        printf("  Bitmap blocks:    %" PRIu64 "\n",
               ctx->sb.bitmap_blocks);
        printf("  Total blocks:     %" PRIu64 "\n", total_blocks);
        printf("  Allocated blocks: %" PRIu64 "\n", allocated);
        printf("  Free blocks:      %" PRIu64 "\n",
               total_blocks - allocated);

        /* ---- Build expected bitmap and compare ---- */
        if (bitmap_read_ok) {
            /* Set ctx->bitmap temporarily so build_expected_bitmap helpers work */
            ctx->bitmap = disk_bitmap;
            ctx->bitmap_size = bitmap_bytes;

            int build_err = 0;
            uint8_t *expected = build_expected_bitmap(ctx, total_blocks,
                                                      bitmap_bytes,
                                                      &build_err);
            if (expected && !build_err) {
                /* Compare on-disk bitmap with expected */
                uint64_t missing = 0;
                uint64_t extra   = 0;
                uint64_t first_missing = 0;
                uint64_t first_extra   = 0;

                for (uint64_t b = 0; b < total_blocks; b++) {
                    int on_disk     = (disk_bitmap[b / 8] >> (b % 8)) & 1;
                    int in_expected = (expected[b / 8] >> (b % 8)) & 1;

                    if (in_expected && !on_disk) {
                        if (missing == 0) first_missing = b;
                        missing++;
                    }
                    if (!in_expected && on_disk) {
                        if (extra == 0) first_extra = b;
                        extra++;
                    }
                }

                if (missing == 0 && extra == 0) {
                    printf("  Consistency:      OK\n");
                } else {
                    printf("  Consistency:      MISMATCH\n");
                    errors++;

                    if (missing > 0)
                        printf("    %" PRIu64
                               " block(s) used but not marked allocated"
                               " (first: LBA %" PRIu64 ")\n",
                               missing, first_missing);
                    if (extra > 0)
                        printf("    %" PRIu64
                               " block(s) marked allocated but not used"
                               " (first: LBA %" PRIu64 ")\n",
                               extra, first_extra);

                    uint64_t expected_alloc = 0;
                    for (uint64_t b = 0; b < total_blocks; b++) {
                        if ((expected[b / 8] >> (b % 8)) & 1)
                            expected_alloc++;
                    }
                    printf("    Expected allocated: %" PRIu64
                           ", on-disk allocated: %" PRIu64 "\n",
                           expected_alloc, allocated);

                    if (ask_fix(auto_yes, auto_no,
                                "Fix allocation bitmap?")) {
                        memcpy(ctx->bitmap, expected,
                               (size_t)bitmap_bytes);
                        rc = obmafs3_bitmap_write(ctx);
                        if (rc == OBMAFS3_OK) {
                            printf("  Bitmap repaired.\n");
                            errors--;   /* checksum error */
                            if (missing > 0 || extra > 0)
                                errors--;  /* consistency error */
                        } else {
                            fprintf(stderr,
                                    "  Error: failed to write bitmap: %d\n",
                                    rc);
                        }
                    }
                }
                free(expected);
            } else {
                fprintf(stderr,
                        "Warning: could not build expected bitmap\n");
            }

            /* Clear temporary bitmap pointer (obmafs3_close will free) */
        } else {
            fprintf(stderr, "Warning: could not read bitmap data\n");
        }

        if (!bitmap_read_ok && disk_bitmap) {
            free(disk_bitmap);
            ctx->bitmap = NULL;
        }
    }

    /* ---- Summary ---- */
    printf("\n");
    if (errors > 0)
        printf("Filesystem check completed with %d error(s).\n", errors);
    else
        printf("Filesystem check passed.\n");

    obmafs3_close(ctx);
    return errors > 0 ? 1 : 0;
}
