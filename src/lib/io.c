/*
 * io.c - OBMAFS3 context management, block I/O, creation, and checking
 */
#include "obmafs.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void generate_guid(uint8_t *guid)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, guid, 16) != 16)
            memset(guid, 0, 16);
        close(fd);
    } else {
        memset(guid, 0, 16);
    }
    /* RFC 4122 version 4 */
    guid[6] = (guid[6] & 0x0F) | 0x40;
    guid[8] = (guid[8] & 0x3F) | 0x80;
}

/* ------------------------------------------------------------------ */
/*  Block I/O                                                          */
/* ------------------------------------------------------------------ */

int obmafs3_block_read(struct obmafs3_ctx *ctx, uint64_t lba,
                       void *buf, size_t size)
{
    off_t offset = (off_t)(lba * ctx->sb.block_size);
    ssize_t n = pread(ctx->fd, buf, size, offset);
    if (n < 0 || (size_t)n != size)
        return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

int obmafs3_block_write(struct obmafs3_ctx *ctx, uint64_t lba,
                        const void *buf, size_t size)
{
    off_t offset = (off_t)(lba * ctx->sb.block_size);
    ssize_t n = pwrite(ctx->fd, buf, size, offset);
    if (n < 0 || (size_t)n != size)
        return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Context open / close                                               */
/* ------------------------------------------------------------------ */

int obmafs3_open(const char *path, struct obmafs3_ctx **ctx)
{
    return obmafs3_open_flags(path, 0, ctx);
}

int obmafs3_open_flags(const char *path, int flags, struct obmafs3_ctx **ctx)
{
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return OBMAFS3_ERR_IO;

    struct obmafs3_ctx *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return OBMAFS3_ERR_NOMEM;
    }

    c->fd = fd;
    c->compression = 1;     /* compression on by default */
    c->zstd_level  = 15;    /* ZSTD level 15 by default */

    int rc = obmafs3_sb_read(fd, &c->sb);
    if (rc != OBMAFS3_OK) { close(fd); free(c); return rc; }

    rc = obmafs3_sb_validate(&c->sb);
    if (rc != OBMAFS3_OK) { close(fd); free(c); return rc; }

    if (flags & OBMAFS3_OPEN_LENIENT) {
        int cs_ok;
        rc = obmafs3_btree_header_read_lenient(c, c->sb.catalog_lba,
                                               &c->catalog_hdr, &cs_ok);
        if (rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM) {
            close(fd); free(c); return rc;
        }

        rc = obmafs3_btree_header_read_lenient(c, c->sb.inode_lba,
                                               &c->inode_hdr, &cs_ok);
        if (rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM) {
            close(fd); free(c); return rc;
        }

        if (c->sb.overflow_lba != 0) {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.overflow_lba,
                                                   &c->overflow_hdr,
                                                   &cs_ok);
            if (rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM) {
                close(fd); free(c); return rc;
            }
        }
    } else {
        rc = obmafs3_btree_header_read(c, c->sb.catalog_lba,
                                       &c->catalog_hdr);
        if (rc != OBMAFS3_OK) { close(fd); free(c); return rc; }

        rc = obmafs3_btree_header_read(c, c->sb.inode_lba, &c->inode_hdr);
        if (rc != OBMAFS3_OK) { close(fd); free(c); return rc; }

        if (c->sb.overflow_lba != 0) {
            rc = obmafs3_btree_header_read(c, c->sb.overflow_lba,
                                           &c->overflow_hdr);
            if (rc != OBMAFS3_OK) { close(fd); free(c); return rc; }
        }
    }

    /* Load allocation bitmap (skip when asked, e.g. for fsck) */
    if (!(flags & OBMAFS3_OPEN_SKIP_BITMAP) &&
        c->sb.bitmap_lba != 0 && c->sb.bitmap_blocks != 0) {
        rc = obmafs3_bitmap_read(c);
        if (rc != OBMAFS3_OK) { close(fd); free(c); return rc; }
    }

    *ctx = c;
    return OBMAFS3_OK;
}

void obmafs3_close(struct obmafs3_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->bitmap)
        free(ctx->bitmap);
    if (ctx->fd >= 0)
        close(ctx->fd);
    free(ctx);
}

/* ------------------------------------------------------------------ */
/*  Filesystem creation (mkobmafs)                                     */
/* ------------------------------------------------------------------ */

static int write_block(int fd, uint64_t block_size, uint64_t lba,
                       const void *data, size_t data_size)
{
    uint8_t *block = calloc(1, (size_t)block_size);
    if (!block)
        return OBMAFS3_ERR_NOMEM;

    if (data_size > (size_t)block_size)
        data_size = (size_t)block_size;
    memcpy(block, data, data_size);

    off_t offset = (off_t)(lba * block_size);
    ssize_t n = pwrite(fd, block, (size_t)block_size, offset);
    free(block);

    if (n < 0 || (size_t)n != (size_t)block_size)
        return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

int obmafs3_create(const char *path, uint64_t total_size,
                   uint64_t block_size, uint64_t dedup_block_size,
                   const char *label, const uint8_t *guid)
{
    struct stat st;
    int is_blkdev = 0;
    int fd;

    if (stat(path, &st) == 0 && S_ISBLK(st.st_mode)) {
        is_blkdev = 1;
        fd = open(path, O_RDWR);
    } else {
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0)
        return OBMAFS3_ERR_IO;

    if (!is_blkdev && ftruncate(fd, (off_t)total_size) < 0) {
        close(fd);
        return OBMAFS3_ERR_IO;
    }

    int rc;

    /* --- Block 0: Superblock --- */
    struct obmafs3_sb sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic           = OBMAFS3_SB_MAGIC;
    if (guid)
        memcpy(sb.guid, guid, 16);
    else
        generate_guid(sb.guid);
    sb.block_size      = block_size;
    sb.dedup_block_size = dedup_block_size;
    sb.total_bytes     = total_size;
    sb.catalog_lba     = 1;   /* block 1 */
    sb.inode_lba       = 3;   /* block 3 */
    sb.overflow_lba    = 5;   /* block 5 */
    sb.dedup_lba       = 6;   /* block 6 */
    sb.metadata_lba    = 0;   /* reserved */
    sb.media_tag_lba   = 0;   /* reserved */
    sb.checksum_type   = kChecksumTypeXXH64;
    sb.creation_time   = (uint64_t)time(NULL);

    /* Calculate allocation bitmap size */
    uint64_t total_blocks = total_size / block_size;
    uint64_t bitmap_bytes = (total_blocks + 7) / 8;
    size_t   hdr_size = sizeof(struct bitmap_header);
    /* First block holds header + data, subsequent blocks are pure data */
    uint64_t first_block_capacity = block_size - hdr_size;
    uint64_t bitmap_blks;
    if (bitmap_bytes <= first_block_capacity)
        bitmap_blks = 1;
    else
        bitmap_blks = 1 + (bitmap_bytes - first_block_capacity +
                           block_size - 1) / block_size;

    sb.bitmap_lba      = 7;                   /* bitmap starts at block 7 */
    sb.bitmap_blocks   = bitmap_blks;
    sb.next_free_lba   = 7 + bitmap_blks;     /* first block after bitmap */
    sb.next_inode_id   = 3;                    /* root inode is 2, next is 3 */
    strncpy((char *)sb.volume_label, label,
            sizeof(sb.volume_label) - 1);

    rc = write_block(fd, block_size, 0, &sb, sizeof(sb));
    if (rc != OBMAFS3_OK) { close(fd); return rc; }

    /* --- Block 1: Catalog tree header --- */
    struct btree_header cat_hdr;
    memset(&cat_hdr, 0, sizeof(cat_hdr));
    cat_hdr.magic         = OBMAFS3_BTREE_HDR_MAGIC;
    cat_hdr.data_type     = kBtreeDataTypeFilename;
    cat_hdr.root_node_lba = 2;
    cat_hdr.node_size     = (uint16_t)block_size;
    cat_hdr.total_nodes   = 1;
    cat_hdr.tree_type     = kBtreeTypeCatalog;
    /* checksum is already zeroed; hash entire struct, store result */
    obmafs3_checksum_block(&cat_hdr, sizeof(cat_hdr), cat_hdr.checksum);

    rc = write_block(fd, block_size, 1, &cat_hdr, sizeof(cat_hdr));
    if (rc != OBMAFS3_OK) { close(fd); return rc; }

    /* --- Block 2: Catalog root node (root directory entry) --- */
    struct btree_node_filename root_cat;
    memset(&root_cat, 0, sizeof(root_cat));
    root_cat.header.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    root_cat.header.record_type = kBtreeDataTypeFilename;
    root_cat.header.node_keys   = 1;
    root_cat.header.keys_length = (uint16_t)(sizeof(root_cat) -
                                             sizeof(root_cat.header));
    root_cat.inode_id       = OBMAFS3_ROOT_INODE_ID;
    root_cat.parent_id      = OBMAFS3_ROOT_INODE_ID;
    root_cat.directory_flag = 1;
    strncpy(root_cat.name, "/", sizeof(root_cat.name) - 1);
    /* checksum is already zeroed; hash entire node struct, store result */
    obmafs3_checksum_block(&root_cat, sizeof(root_cat),
                           root_cat.header.checksum);

    rc = write_block(fd, block_size, 2, &root_cat, sizeof(root_cat));
    if (rc != OBMAFS3_OK) { close(fd); return rc; }

    /* --- Block 3: Inode tree header --- */
    struct btree_header ino_hdr;
    memset(&ino_hdr, 0, sizeof(ino_hdr));
    ino_hdr.magic         = OBMAFS3_BTREE_HDR_MAGIC;
    ino_hdr.data_type     = kBtreeDataTypeInode;
    ino_hdr.root_node_lba = 4;
    ino_hdr.node_size     = (uint16_t)block_size;
    ino_hdr.total_nodes   = 1;
    ino_hdr.tree_type     = kBtreeTypeInode;
    obmafs3_checksum_block(&ino_hdr, sizeof(ino_hdr), ino_hdr.checksum);

    rc = write_block(fd, block_size, 3, &ino_hdr, sizeof(ino_hdr));
    if (rc != OBMAFS3_OK) { close(fd); return rc; }

    /* --- Block 4: Root directory inode (B+Tree leaf with one record) --- */
    uint8_t *ino_buf = calloc(1, block_size);
    if (!ino_buf) { close(fd); return OBMAFS3_ERR_NOMEM; }

    struct btree_node_header ino_node_hdr;
    memset(&ino_node_hdr, 0, sizeof(ino_node_hdr));
    ino_node_hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    ino_node_hdr.record_type = kBtreeDataTypeInode;
    ino_node_hdr.level       = 0;
    ino_node_hdr.node_keys   = 1;
    ino_node_hdr.keys_length = (uint16_t)sizeof(struct inode_record);
    memcpy(ino_buf, &ino_node_hdr, sizeof(ino_node_hdr));

    struct inode_record root_ino;
    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.inode_id          = OBMAFS3_ROOT_INODE_ID;
    root_ino.uid               = 0;
    root_ino.gid               = 0;
    root_ino.mode              = 0755;
    root_ino.creation_time     = sb.creation_time;
    root_ino.modification_time = sb.creation_time;
    root_ino.access_time       = sb.creation_time;
    root_ino.file_size         = 0;
    root_ino.file_type         = kFileTypeDirectory;
    memcpy(ino_buf + sizeof(ino_node_hdr), &root_ino, sizeof(root_ino));

    /* Compute checksum the same way btree.c does:
       zero the checksum field, hash header + keys_length bytes */
    {
        struct btree_node_header *nhdr = (struct btree_node_header *)ino_buf;
        size_t data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
        memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
        obmafs3_checksum_block(ino_buf, data_size, nhdr->checksum);
    }

    rc = write_block(fd, block_size, 4, ino_buf, block_size);
    free(ino_buf);
    if (rc != OBMAFS3_OK) { close(fd); return rc; }

    /* --- Block 5: Overflow tree header (empty) --- */
    struct btree_header ovf_hdr;
    memset(&ovf_hdr, 0, sizeof(ovf_hdr));
    ovf_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    ovf_hdr.data_type = kBtreeDataTypeExtent;
    ovf_hdr.node_size = (uint16_t)block_size;
    ovf_hdr.tree_type = kBtreeTypeOverflow;
    obmafs3_checksum_block(&ovf_hdr, sizeof(ovf_hdr), ovf_hdr.checksum);

    rc = write_block(fd, block_size, 5, &ovf_hdr, sizeof(ovf_hdr));
    if (rc != OBMAFS3_OK) { close(fd); return rc; }

    /* --- Block 6: Dedup tree list header (empty) --- */
    struct tree_list_header dedup_list;
    memset(&dedup_list, 0, sizeof(dedup_list));
    dedup_list.magic      = OBMAFS3_TREELIST_MAGIC;
    dedup_list.tree_count = 0;
    obmafs3_checksum_block(&dedup_list, sizeof(dedup_list),
                           dedup_list.checksum);

    rc = write_block(fd, block_size, 6, &dedup_list, sizeof(dedup_list));
    if (rc != OBMAFS3_OK) { close(fd); return rc; }

    /* --- Blocks 7..7+N-1: Allocation bitmap --- */
    {
        /* Build the flat bitmap data */
        uint8_t *bitmap = calloc(1, (size_t)bitmap_bytes);
        if (!bitmap) { close(fd); return OBMAFS3_ERR_NOMEM; }

        /* Mark blocks 0 through (7 + bitmap_blks - 1) as allocated */
        uint64_t reserved = 7 + bitmap_blks;
        for (uint64_t b = 0; b < reserved; b++)
            bitmap[b / 8] |= (1u << (b % 8));

        /* Build the bitmap header with checksum over bitmap data */
        struct bitmap_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        bhdr.magic = OBMAFS3_BITMAP_MAGIC;
        bhdr.total_blocks = total_blocks;
        obmafs3_checksum_block(bitmap, (size_t)bitmap_bytes,
                               bhdr.checksum);

        /* Write bitmap blocks: first block = header + data */
        uint8_t *blk = calloc(1, (size_t)block_size);
        if (!blk) { free(bitmap); close(fd); return OBMAFS3_ERR_NOMEM; }

        uint64_t data_offset = 0;
        uint64_t data_remaining = bitmap_bytes;

        for (uint64_t i = 0; i < bitmap_blks; i++) {
            memset(blk, 0, (size_t)block_size);

            if (i == 0) {
                memcpy(blk, &bhdr, hdr_size);
                size_t avail = (size_t)(block_size - hdr_size);
                size_t copy = (data_remaining < avail)
                                  ? (size_t)data_remaining : avail;
                memcpy(blk + hdr_size, bitmap + data_offset, copy);
                data_offset += copy;
                data_remaining -= copy;
            } else {
                size_t copy = (data_remaining < block_size)
                                  ? (size_t)data_remaining
                                  : (size_t)block_size;
                memcpy(blk, bitmap + data_offset, copy);
                data_offset += copy;
                data_remaining -= copy;
            }

            rc = write_block(fd, block_size, 7 + i, blk,
                             (size_t)block_size);
            if (rc != OBMAFS3_OK) {
                free(blk);
                free(bitmap);
                close(fd);
                return rc;
            }
        }
        free(blk);
        free(bitmap);
    }

    /* Flush and close */
    if (fsync(fd) < 0) {
        close(fd);
        return OBMAFS3_ERR_IO;
    }

    close(fd);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Filesystem checking (obmafsck)                                     */
/* ------------------------------------------------------------------ */

int obmafs3_check(const char *path)
{
    struct obmafs3_ctx *ctx;
    int rc = obmafs3_open(path, &ctx);
    if (rc != OBMAFS3_OK) {
        fprintf(stderr, "Error: failed to open filesystem: %d\n", rc);
        return rc;
    }

    printf("Superblock:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n",
           ctx->sb.magic,
           ctx->sb.magic == OBMAFS3_SB_MAGIC ? "OK" : "BAD");
    printf("  Block size:       %" PRIu64 "\n", ctx->sb.block_size);
    printf("  Dedup block size: %" PRIu64 "\n", ctx->sb.dedup_block_size);
    printf("  Total bytes:      %" PRIu64 "\n", ctx->sb.total_bytes);
    printf("  Volume label:     %s\n", ctx->sb.volume_label);

    /* Bitmap statistics */
    if (ctx->sb.bitmap_lba != 0 && ctx->bitmap) {
        /* Read the header for display */
        uint8_t *bhdr_buf = malloc((size_t)ctx->sb.block_size);
        struct bitmap_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        if (bhdr_buf) {
            if (obmafs3_block_read(ctx, ctx->sb.bitmap_lba, bhdr_buf,
                                   (size_t)ctx->sb.block_size) == OBMAFS3_OK)
                memcpy(&bhdr, bhdr_buf, sizeof(bhdr));
            free(bhdr_buf);
        }

        uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
        uint64_t allocated = 0;
        for (uint64_t b = 0; b < total_blocks; b++) {
            if (obmafs3_bitmap_is_set(ctx, b))
                allocated++;
        }

        printf("\nAllocation bitmap:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n",
               bhdr.magic,
               bhdr.magic == OBMAFS3_BITMAP_MAGIC ? "OK" : "BAD");
        printf("  Checksum:         %s\n",
               ctx->bitmap ? "OK" : "BAD");  /* bitmap_read would have failed */
        printf("  Bitmap LBA:       %" PRIu64 "\n", ctx->sb.bitmap_lba);
        printf("  Bitmap blocks:    %" PRIu64 "\n", ctx->sb.bitmap_blocks);
        printf("  Total blocks:     %" PRIu64 "\n", total_blocks);
        printf("  Allocated blocks: %" PRIu64 "\n", allocated);
        printf("  Free blocks:      %" PRIu64 "\n",
               total_blocks - allocated);
    }

    printf("\nCatalog tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n",
           ctx->catalog_hdr.magic,
           ctx->catalog_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC
               ? "OK" : "BAD");
    printf("  Checksum:         OK\n");  /* btree_header_read already validated */
    printf("  Root node LBA:    %" PRIu64 "\n",
           ctx->catalog_hdr.root_node_lba);
    printf("  Total nodes:      %u\n", ctx->catalog_hdr.total_nodes);

    printf("\nInode tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n",
           ctx->inode_hdr.magic,
           ctx->inode_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC
               ? "OK" : "BAD");
    printf("  Checksum:         OK\n");  /* btree_header_read already validated */
    printf("  Root node LBA:    %" PRIu64 "\n",
           ctx->inode_hdr.root_node_lba);
    printf("  Total nodes:      %u\n", ctx->inode_hdr.total_nodes);

    obmafs3_close(ctx);
    printf("\nFilesystem check passed.\n");
    return OBMAFS3_OK;
}
