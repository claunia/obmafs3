/*
 * block.c - OBMAFS3 block I/O, compression, and file data reading/writing
 */
#include "obmafs.h"

#include <zstd.h>
#include <stdlib.h>
#include <string.h>

int obmafs3_compress(const void *src, size_t src_size,
                     void *dst, size_t *dst_size, int level)
{
    size_t result = ZSTD_compress(dst, *dst_size, src, src_size, level);
    if (ZSTD_isError(result))
        return OBMAFS3_ERR_IO;
    *dst_size = result;
    return OBMAFS3_OK;
}

int obmafs3_decompress(const void *src, size_t src_size,
                       void *dst, size_t dst_size)
{
    size_t result = ZSTD_decompress(dst, dst_size, src, src_size);
    if (ZSTD_isError(result))
        return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Overflow extent tree helpers                                       */
/* ------------------------------------------------------------------ */

/** Compute and store the checksum for a btree node block. */
static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr = (struct btree_node_header *)buf;
    size_t data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

/** Maximum number of overflow_extent records that fit in one node. */
static uint16_t overflow_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct overflow_extent));
}

/**
 * Insert an extent into the overflow tree for a given inode.
 * Uses multi-key nodes linked via right_link, same pattern as dedup nodes.
 */
static int overflow_insert(struct obmafs3_ctx *ctx,
                           const struct overflow_extent *entry)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    uint64_t hdr_lba = ctx->sb.overflow_lba;
    uint16_t max_keys = overflow_max_keys(ctx);
    int rc;

    /* If the tree has no root node yet, allocate one */
    if (hdr->root_node_lba == 0) {
        uint64_t root_lba;
        rc = obmafs3_alloc_block(ctx, &root_lba);
        if (rc != OBMAFS3_OK)
            return rc;

        uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
        if (!node_buf)
            return OBMAFS3_ERR_NOMEM;

        struct btree_node_header nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nhdr.record_type = kBtreeDataTypeExtent;
        nhdr.node_keys   = 1;
        nhdr.keys_length = (uint16_t)sizeof(struct overflow_extent);
        memcpy(node_buf, &nhdr, sizeof(nhdr));
        memcpy(node_buf + sizeof(nhdr), entry, sizeof(*entry));
        compute_node_checksum(node_buf);

        rc = obmafs3_block_write(ctx, root_lba, node_buf,
                                 (size_t)ctx->sb.block_size);
        free(node_buf);
        if (rc != OBMAFS3_OK)
            return rc;

        hdr->root_node_lba = root_lba;
        hdr->total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
    }

    /* Walk to the last node in the linked list */
    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!node_buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = hdr->root_node_lba;
    uint64_t last_lba = lba;

    while (lba != 0) {
        rc = obmafs3_block_read(ctx, lba, node_buf,
                                (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(node_buf);
            return rc;
        }
        last_lba = lba;

        struct btree_node_header nhdr;
        memcpy(&nhdr, node_buf, sizeof(nhdr));
        if (nhdr.right_link == 0)
            break;
        lba = nhdr.right_link;
    }

    /* Check if the last node has room */
    struct btree_node_header nhdr;
    memcpy(&nhdr, node_buf, sizeof(nhdr));

    if (nhdr.node_keys < max_keys) {
        /* Append in place */
        size_t offset = sizeof(struct btree_node_header)
                        + nhdr.node_keys * sizeof(struct overflow_extent);
        memcpy(node_buf + offset, entry, sizeof(*entry));
        nhdr.node_keys++;
        nhdr.keys_length = (uint16_t)(nhdr.node_keys *
                                      sizeof(struct overflow_extent));
        memcpy(node_buf, &nhdr, sizeof(nhdr));
        compute_node_checksum(node_buf);
        rc = obmafs3_block_write(ctx, last_lba, node_buf,
                                 (size_t)ctx->sb.block_size);
        free(node_buf);
        return rc;
    }

    /* Last node full — allocate a new one */
    uint64_t new_lba;
    rc = obmafs3_alloc_block(ctx, &new_lba);
    if (rc != OBMAFS3_OK) {
        free(node_buf);
        return rc;
    }

    uint8_t *new_buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!new_buf) {
        free(node_buf);
        return OBMAFS3_ERR_NOMEM;
    }

    struct btree_node_header new_hdr;
    memset(&new_hdr, 0, sizeof(new_hdr));
    new_hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    new_hdr.record_type = kBtreeDataTypeExtent;
    new_hdr.node_keys   = 1;
    new_hdr.keys_length = (uint16_t)sizeof(struct overflow_extent);
    memcpy(new_buf, &new_hdr, sizeof(new_hdr));
    memcpy(new_buf + sizeof(new_hdr), entry, sizeof(*entry));
    compute_node_checksum(new_buf);

    rc = obmafs3_block_write(ctx, new_lba, new_buf,
                             (size_t)ctx->sb.block_size);
    free(new_buf);
    if (rc != OBMAFS3_OK) {
        free(node_buf);
        return rc;
    }

    /* Link the previous last node to the new one */
    nhdr.right_link = new_lba;
    memcpy(node_buf, &nhdr, sizeof(nhdr));
    compute_node_checksum(node_buf);
    rc = obmafs3_block_write(ctx, last_lba, node_buf,
                             (size_t)ctx->sb.block_size);
    free(node_buf);
    if (rc != OBMAFS3_OK)
        return rc;

    hdr->total_nodes++;
    return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
}

/**
 * Search the overflow tree for extents belonging to the given inode
 * and map a logical block number to a physical LBA.
 *
 * Inline extents cover logical blocks 0..inline_block_count-1.
 * Overflow extents continue from there: the first overflow extent
 * covers logical blocks starting at inline_block_count.
 *
 * Returns 1 if found (phys_lba set), 0 if not found.
 */
static int overflow_find_phys(struct obmafs3_ctx *ctx,
                              uint64_t inode_id,
                              uint64_t logical_block,
                              uint64_t inline_block_count,
                              uint64_t *phys_lba)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if (hdr->root_node_lba == 0)
        return 0;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return 0;

    uint64_t lba = hdr->root_node_lba;
    uint64_t ovf_block_count = inline_block_count;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return 0;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if (nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return 0;
        }

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        for (uint16_t i = 0; i < nhdr.node_keys; i++) {
            struct overflow_extent oe;
            memcpy(&oe, entries + i * sizeof(struct overflow_extent),
                   sizeof(oe));
            if (oe.inode_id != inode_id)
                continue;

            if (logical_block >= ovf_block_count &&
                logical_block < ovf_block_count + oe.block_count) {
                *phys_lba = oe.start_block +
                            (logical_block - ovf_block_count);
                free(buf);
                return 1;
            }
            ovf_block_count += oe.block_count;
        }

        lba = nhdr.right_link;
    }

    free(buf);
    return 0;
}

/**
 * Count the total number of blocks stored in overflow extents for an inode.
 */
static uint64_t overflow_count_blocks(struct obmafs3_ctx *ctx,
                                      uint64_t inode_id)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if (hdr->root_node_lba == 0)
        return 0;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return 0;

    uint64_t total = 0;
    uint64_t lba = hdr->root_node_lba;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK)
            break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if (nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
            break;

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        for (uint16_t i = 0; i < nhdr.node_keys; i++) {
            struct overflow_extent oe;
            memcpy(&oe, entries + i * sizeof(struct overflow_extent),
                   sizeof(oe));
            if (oe.inode_id == inode_id)
                total += oe.block_count;
        }

        lba = nhdr.right_link;
    }

    free(buf);
    return total;
}

/* ------------------------------------------------------------------ */
/*  File data reading                                                  */
/* ------------------------------------------------------------------ */

int obmafs3_read_file_data(struct obmafs3_ctx *ctx,
                           const struct btree_node_inode *inode,
                           uint64_t offset, void *buf, size_t size)
{
    uint64_t block_size = ctx->sb.block_size;
    size_t data_capacity = (size_t)block_size - sizeof(struct block_header);
    size_t bytes_read = 0;
    uint8_t *block_buf;
    uint8_t *decomp_buf;

    if (offset >= inode->file_size)
        return OBMAFS3_OK;

    if (offset + size > inode->file_size)
        size = (size_t)(inode->file_size - offset);

    block_buf = malloc((size_t)block_size);
    decomp_buf = malloc((size_t)block_size);
    if (!block_buf || !decomp_buf) {
        free(block_buf);
        free(decomp_buf);
        return OBMAFS3_ERR_NOMEM;
    }

    while (bytes_read < size) {
        uint64_t read_pos = offset + bytes_read;
        uint64_t logical_block = read_pos / data_capacity;
        size_t offset_in_block = (size_t)(read_pos % data_capacity);

        /* Map logical block to physical LBA via extents */
        uint64_t phys_lba = 0;
        uint64_t block_count_so_far = 0;
        int found = 0;
        int i;
        for (i = 0; i < 8; i++) {
            if (inode->extents[i].block_count == 0)
                continue;
            if (logical_block < block_count_so_far +
                                    inode->extents[i].block_count) {
                phys_lba = inode->extents[i].start_block +
                           (logical_block - block_count_so_far);
                found = 1;
                break;
            }
            block_count_so_far += inode->extents[i].block_count;
        }

        /* If not found in inline extents, check overflow tree */
        if (!found) {
            found = overflow_find_phys(ctx, inode->inode_id,
                                       logical_block, block_count_so_far,
                                       &phys_lba);
        }

        if (!found) {
            free(block_buf);
            free(decomp_buf);
            return OBMAFS3_ERR_IO;
        }

        int rc = obmafs3_block_read(ctx, phys_lba, block_buf,
                                    (size_t)block_size);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            free(decomp_buf);
            return rc;
        }

        struct block_header bhdr;
        memcpy(&bhdr, block_buf, sizeof(bhdr));

        uint8_t *data_ptr;
        size_t data_len;

        if (bhdr.magic != OBMAFS3_BLOCK_MAGIC) {
            /* Raw data block (no header) — shouldn't happen with
             * the current write path but handle gracefully */
            data_ptr = block_buf;
            data_len = (size_t)block_size;
        } else {
            /* Verify checksum over on-disk data after header */
            size_t check_size = (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                                    ? (size_t)bhdr.compressed_size
                                    : (size_t)bhdr.original_size;
            uint8_t computed[32];
            obmafs3_checksum_block(block_buf + sizeof(bhdr),
                                   check_size, computed);
            if (memcmp(computed, bhdr.checksum, 32) != 0) {
                free(block_buf);
                free(decomp_buf);
                return OBMAFS3_ERR_CHECKSUM;
            }

            if (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) {
                rc = obmafs3_decompress(
                    block_buf + sizeof(bhdr),
                    (size_t)bhdr.compressed_size,
                    decomp_buf,
                    (size_t)bhdr.original_size);
                if (rc != OBMAFS3_OK) {
                    free(block_buf);
                    free(decomp_buf);
                    return rc;
                }
                data_ptr = decomp_buf;
                data_len = (size_t)bhdr.original_size;
            } else {
                data_ptr = block_buf + sizeof(bhdr);
                data_len = (size_t)bhdr.original_size;
            }
        }

        /* Copy data from the block at the correct offset */
        size_t avail = data_len > offset_in_block
                           ? data_len - offset_in_block : 0;
        size_t to_copy = (size - bytes_read < avail)
                             ? size - bytes_read : avail;
        if (to_copy == 0)
            break;

        memcpy((uint8_t *)buf + bytes_read,
               data_ptr + offset_in_block, to_copy);
        bytes_read += to_copy;
    }

    free(block_buf);
    free(decomp_buf);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  File data writing                                                  */
/* ------------------------------------------------------------------ */

int obmafs3_write_file_data(struct obmafs3_ctx *ctx,
                            struct btree_node_inode *inode,
                            uint64_t offset, const void *buf, size_t size)
{
    uint64_t block_size = ctx->sb.block_size;
    size_t data_capacity = (size_t)block_size - sizeof(struct block_header);
    size_t bytes_written = 0;
    int rc;

    /* Calculate the total number of data blocks needed for the file
     * after this write */
    uint64_t new_end = offset + size;
    if (new_end > inode->file_size)
        inode->file_size = new_end;

    uint64_t total_blocks_needed =
        (inode->file_size + data_capacity - 1) / data_capacity;

    /* Count existing allocated blocks (inline + overflow) */
    uint64_t existing_blocks = 0;
    int i;
    for (i = 0; i < 8; i++)
        existing_blocks += inode->extents[i].block_count;
    existing_blocks += overflow_count_blocks(ctx, inode->inode_id);

    /* Allocate additional blocks if needed */
    if (total_blocks_needed > existing_blocks) {
        uint64_t new_blocks = total_blocks_needed - existing_blocks;
        uint64_t new_start;
        rc = obmafs3_alloc_blocks(ctx, new_blocks, &new_start);
        if (rc != OBMAFS3_OK)
            return rc;

        /* Initialize new blocks with empty block headers */
        uint8_t *zero_block = calloc(1, (size_t)block_size);
        if (!zero_block)
            return OBMAFS3_ERR_NOMEM;
        struct block_header empty_hdr;
        memset(&empty_hdr, 0, sizeof(empty_hdr));
        empty_hdr.magic = OBMAFS3_BLOCK_MAGIC;
        empty_hdr.flags = 0;
        empty_hdr.original_size = 0;
        empty_hdr.compressed_size = 0;
        /* Checksum of zero-length data */
        obmafs3_checksum_block(zero_block + sizeof(empty_hdr), 0,
                               empty_hdr.checksum);
        memcpy(zero_block, &empty_hdr, sizeof(empty_hdr));
        for (uint64_t b = 0; b < new_blocks; b++) {
            rc = obmafs3_block_write(ctx, new_start + b, zero_block,
                                     (size_t)block_size);
            if (rc != OBMAFS3_OK) {
                free(zero_block);
                return rc;
            }
        }
        free(zero_block);

        /* Add the new extent to the inode */
        int added = 0;
        for (i = 0; i < 8; i++) {
            if (inode->extents[i].block_count == 0) {
                inode->extents[i].start_block = new_start;
                inode->extents[i].block_count = new_blocks;
                added = 1;
                break;
            }
            /* Try to extend an adjacent extent */
            if (inode->extents[i].start_block +
                    inode->extents[i].block_count == new_start) {
                inode->extents[i].block_count += new_blocks;
                added = 1;
                break;
            }
        }
        if (!added) {
            /* Inline extents full — store in overflow tree */
            struct overflow_extent oe;
            oe.inode_id    = inode->inode_id;
            oe.start_block = new_start;
            oe.block_count = new_blocks;
            rc = overflow_insert(ctx, &oe);
            if (rc != OBMAFS3_OK)
                return rc;
        }
    }

    /* Now write the data into the appropriate blocks */
    uint8_t *block_buf = malloc((size_t)block_size);
    if (!block_buf)
        return OBMAFS3_ERR_NOMEM;

    while (bytes_written < size) {
        /* Determine which logical data block this offset falls into */
        uint64_t write_pos = offset + bytes_written;
        uint64_t logical_block = write_pos / data_capacity;
        size_t offset_in_block = (size_t)(write_pos % data_capacity);

        /* Map logical block to physical LBA via extents */
        uint64_t phys_lba = 0;
        uint64_t block_count_so_far = 0;
        int found = 0;
        for (i = 0; i < 8; i++) {
            if (inode->extents[i].block_count == 0)
                continue;
            if (logical_block < block_count_so_far +
                                    inode->extents[i].block_count) {
                phys_lba = inode->extents[i].start_block +
                           (logical_block - block_count_so_far);
                found = 1;
                break;
            }
            block_count_so_far += inode->extents[i].block_count;
        }

        /* If not found in inline extents, check overflow tree */
        if (!found) {
            found = overflow_find_phys(ctx, inode->inode_id,
                                       logical_block, block_count_so_far,
                                       &phys_lba);
        }

        if (!found) {
            free(block_buf);
            return OBMAFS3_ERR_IO;
        }

        /* Read the existing block */
        rc = obmafs3_block_read(ctx, phys_lba, block_buf,
                                (size_t)block_size);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            return rc;
        }

        /* Determine how much to write into this block */
        size_t space = data_capacity - offset_in_block;
        size_t to_write = (size - bytes_written < space)
                              ? size - bytes_written
                              : space;

        /* Write data after the block header */
        memcpy(block_buf + sizeof(struct block_header) + offset_in_block,
               (const uint8_t *)buf + bytes_written, to_write);

        /* Update block header (uncompressed for writes) */
        struct block_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        bhdr.magic = OBMAFS3_BLOCK_MAGIC;
        uint64_t block_data_end = offset_in_block + to_write;
        /* Track the maximum data in this block */
        struct block_header existing_hdr;
        memcpy(&existing_hdr, block_buf, sizeof(existing_hdr));
        if (existing_hdr.original_size > block_data_end)
            block_data_end = existing_hdr.original_size;
        bhdr.original_size = block_data_end;

        /* Try to compress if enabled */
        uint8_t *write_buf = block_buf;
        uint8_t *comp_block = NULL;
        if (ctx->compression && block_data_end > 0) {
            size_t comp_bound = ZSTD_compressBound((size_t)block_data_end);
            size_t comp_buf_size = sizeof(struct block_header) + comp_bound;
            if (comp_buf_size < (size_t)block_size)
                comp_buf_size = (size_t)block_size;
            comp_block = calloc(1, comp_buf_size);
            if (comp_block) {
                size_t comp_size = comp_bound;
                int crc = obmafs3_compress(
                    block_buf + sizeof(struct block_header),
                    (size_t)block_data_end,
                    comp_block + sizeof(struct block_header),
                    &comp_size, ctx->zstd_level);
                if (crc == OBMAFS3_OK &&
                    comp_size < block_data_end) {
                    /* Compression saved space — use compressed block */
                    bhdr.flags = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                    bhdr.compression_type = kCompressionZstd;
                    bhdr.compressed_size = comp_size;
                    obmafs3_checksum_block(
                        comp_block + sizeof(struct block_header),
                        comp_size, bhdr.checksum);
                    memcpy(comp_block, &bhdr, sizeof(bhdr));
                    /* Zero-fill remainder of the block */
                    size_t used = sizeof(struct block_header) + comp_size;
                    if (used < (size_t)block_size)
                        memset(comp_block + used, 0,
                               (size_t)block_size - used);
                    write_buf = comp_block;
                } else {
                    free(comp_block);
                    comp_block = NULL;
                }
            }
        }

        if (!comp_block) {
            /* Store uncompressed */
            bhdr.flags = 0;
            bhdr.compressed_size = block_data_end;
            obmafs3_checksum_block(
                block_buf + sizeof(struct block_header),
                (size_t)block_data_end, bhdr.checksum);
            memcpy(block_buf, &bhdr, sizeof(bhdr));
        }

        rc = obmafs3_block_write(ctx, phys_lba, write_buf,
                                 (size_t)block_size);
        free(comp_block);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            return rc;
        }

        bytes_written += to_write;
    }

    free(block_buf);
    return OBMAFS3_OK;
}
