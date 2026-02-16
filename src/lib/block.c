/*
 * block.c - OBMAFS3 block I/O, compression, and file data reading/writing
 */
#include "obmafs.h"

#include <zstd.h>
#include <stdlib.h>
#include <string.h>

int obmafs3_compress(const void *src, size_t src_size,
                     void *dst, size_t *dst_size)
{
    size_t result = ZSTD_compress(dst, *dst_size, src, src_size, 3);
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

int obmafs3_read_file_data(struct obmafs3_ctx *ctx,
                           const struct btree_node_inode *inode,
                           uint64_t offset, void *buf, size_t size)
{
    uint64_t block_size = ctx->sb.block_size;
    size_t bytes_read = 0;
    uint8_t *block_buf;
    uint8_t *decomp_buf;
    int i;

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

    for (i = 0; i < 8 && bytes_read < size; i++) {
        const struct extent_run *ext = &inode->extents[i];
        uint64_t b;

        if (ext->block_count == 0)
            continue;

        for (b = 0; b < ext->block_count && bytes_read < size; b++) {
            uint64_t block_lba = ext->start_block + b;
            uint64_t block_byte_start = block_lba * block_size;
            struct block_header bhdr;
            size_t copy_offset = 0;
            size_t copy_size;
            int rc;

            if (block_byte_start + block_size <= offset)
                continue;

            rc = obmafs3_block_read(ctx, block_lba, block_buf,
                                    (size_t)block_size);
            if (rc != OBMAFS3_OK) {
                free(block_buf);
                free(decomp_buf);
                return rc;
            }

            memcpy(&bhdr, block_buf, sizeof(bhdr));

            if (bhdr.magic != OBMAFS3_BLOCK_MAGIC) {
                /* Raw data block (no header) */
                if (offset > block_byte_start)
                    copy_offset = (size_t)(offset - block_byte_start);
                copy_size = (size_t)block_size - copy_offset;
                if (copy_size > size - bytes_read)
                    copy_size = size - bytes_read;
                memcpy((uint8_t *)buf + bytes_read,
                       block_buf + copy_offset, copy_size);
                bytes_read += copy_size;
                continue;
            }

            /* Data block with header */
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

                if (offset > block_byte_start)
                    copy_offset = (size_t)(offset - block_byte_start);
                copy_size = (size_t)bhdr.original_size - copy_offset;
                if (copy_size > size - bytes_read)
                    copy_size = size - bytes_read;
                memcpy((uint8_t *)buf + bytes_read,
                       decomp_buf + copy_offset, copy_size);
            } else {
                if (offset > block_byte_start)
                    copy_offset = (size_t)(offset - block_byte_start);
                copy_size = (size_t)bhdr.original_size - copy_offset;
                if (copy_size > size - bytes_read)
                    copy_size = size - bytes_read;
                memcpy((uint8_t *)buf + bytes_read,
                       block_buf + sizeof(bhdr) + copy_offset,
                       copy_size);
            }

            bytes_read += copy_size;
        }
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

    /* Count existing allocated blocks */
    uint64_t existing_blocks = 0;
    int i;
    for (i = 0; i < 8; i++)
        existing_blocks += inode->extents[i].block_count;

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
        if (!added)
            return OBMAFS3_ERR_NOSPC; /* No room in inode extents */
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
        bhdr.flags = 0; /* no compression on write */
        uint64_t block_data_end = offset_in_block + to_write;
        /* Track the maximum data in this block */
        struct block_header existing_hdr;
        memcpy(&existing_hdr, block_buf, sizeof(existing_hdr));
        if (existing_hdr.original_size > block_data_end)
            block_data_end = existing_hdr.original_size;
        bhdr.original_size = block_data_end;
        bhdr.compressed_size = block_data_end;

        /* Compute checksum of the data */
        obmafs3_checksum_block(
            block_buf + sizeof(struct block_header),
            (size_t)block_data_end, bhdr.checksum);
        memcpy(block_buf, &bhdr, sizeof(bhdr));

        rc = obmafs3_block_write(ctx, phys_lba, block_buf,
                                 (size_t)block_size);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            return rc;
        }

        bytes_written += to_write;
    }

    free(block_buf);
    return OBMAFS3_OK;
}
