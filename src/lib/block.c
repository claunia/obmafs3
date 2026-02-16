/*
 * block.c - OBMAFS3 block I/O, compression, and file data reading
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
