/*
 * bitmap.c - OBMAFS3 allocation bitmap management
 *
 * Each bit in the bitmap represents one standard block.
 * Bit = 1 means allocated, bit = 0 means free.
 * Deduplication blocks that span multiple standard blocks occupy
 * several consecutive bits.
 *
 * The first bitmap block starts with a bitmap_header (magic + checksum),
 * followed by bitmap data. Subsequent blocks contain only bitmap data.
 */
#include "obmafs.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Bitmap I/O                                                         */
/* ------------------------------------------------------------------ */

/**
 * Read the allocation bitmap from disk.
 *
 * Reads the bitmap header (first block) and all subsequent bitmap blocks,
 * validates the magic number and checksum, and stores the bitmap data in
 * the context.
 *
 * @param ctx  Filesystem context.
 * @return @c OBMAFS3_OK on success, or an appropriate error code.
 */
int obmafs3_bitmap_read(struct obmafs3_ctx *ctx)
{
    uint64_t block_size = ctx->sb.block_size;
    uint64_t total_blocks = ctx->sb.total_bytes / block_size;
    uint64_t bitmap_bytes = (total_blocks + 7) / 8;
    size_t hdr_size = sizeof(struct bitmap_header);
    int rc;

    if (ctx->bitmap) {
        free(ctx->bitmap);
        ctx->bitmap = NULL;
    }

    ctx->bitmap = calloc(1, (size_t)bitmap_bytes);
    if (!ctx->bitmap)
        return OBMAFS3_ERR_NOMEM;
    ctx->bitmap_size = bitmap_bytes;

    uint8_t *block_buf = malloc((size_t)block_size);
    if (!block_buf) {
        free(ctx->bitmap);
        ctx->bitmap = NULL;
        return OBMAFS3_ERR_NOMEM;
    }

    uint64_t bytes_remaining = bitmap_bytes;
    uint64_t offset = 0;

    for (uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++) {
        rc = obmafs3_block_read(ctx, ctx->sb.bitmap_lba + i,
                                block_buf, (size_t)block_size);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            free(ctx->bitmap);
            ctx->bitmap = NULL;
            return rc;
        }

        if (i == 0) {
            /* First block: validate header */
            struct bitmap_header bhdr;
            memcpy(&bhdr, block_buf, hdr_size);

            if (bhdr.magic != OBMAFS3_BITMAP_MAGIC) {
                free(block_buf);
                free(ctx->bitmap);
                ctx->bitmap = NULL;
                return OBMAFS3_ERR_BADMAGIC;
            }

            /* Copy bitmap data after the header */
            size_t avail = (size_t)block_size - hdr_size;
            size_t copy_size = (bytes_remaining < avail)
                                   ? (size_t)bytes_remaining
                                   : avail;
            memcpy(ctx->bitmap, block_buf + hdr_size, copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        } else {
            size_t copy_size = (bytes_remaining < block_size)
                                   ? (size_t)bytes_remaining
                                   : (size_t)block_size;
            memcpy(ctx->bitmap + offset, block_buf, copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        }
    }

    /* Verify checksum */
    {
        struct bitmap_header bhdr;
        rc = obmafs3_block_read(ctx, ctx->sb.bitmap_lba,
                                block_buf, (size_t)block_size);
        if (rc == OBMAFS3_OK) {
            memcpy(&bhdr, block_buf, hdr_size);
            uint8_t computed[32];
            obmafs3_checksum_block(ctx->bitmap,
                                  (size_t)bitmap_bytes, computed);
            if (memcmp(bhdr.checksum, computed, 32) != 0) {
                free(block_buf);
                free(ctx->bitmap);
                ctx->bitmap = NULL;
                return OBMAFS3_ERR_CHECKSUM;
            }
        }
    }

    free(block_buf);
    return OBMAFS3_OK;
}

/**
 * Write the in-memory allocation bitmap to disk.
 *
 * Builds a fresh bitmap header with an updated checksum and writes all
 * bitmap blocks to their on-disk locations.
 *
 * @param ctx  Filesystem context.
 * @return @c OBMAFS3_OK on success, or an appropriate error code.
 */
int obmafs3_bitmap_write(struct obmafs3_ctx *ctx)
{
    if (!ctx->bitmap)
        return OBMAFS3_ERR_INVAL;

    uint64_t block_size = ctx->sb.block_size;
    size_t hdr_size = sizeof(struct bitmap_header);
    uint8_t *block_buf = calloc(1, (size_t)block_size);
    if (!block_buf)
        return OBMAFS3_ERR_NOMEM;

    /* Build header with fresh checksum */
    struct bitmap_header bhdr;
    memset(&bhdr, 0, sizeof(bhdr));
    bhdr.magic = OBMAFS3_BITMAP_MAGIC;
    bhdr.total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
    obmafs3_checksum_block(ctx->bitmap,
                           (size_t)ctx->bitmap_size, bhdr.checksum);

    uint64_t bytes_remaining = ctx->bitmap_size;
    uint64_t offset = 0;
    int rc;

    for (uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++) {
        memset(block_buf, 0, (size_t)block_size);

        if (i == 0) {
            /* First block: header + bitmap data */
            memcpy(block_buf, &bhdr, hdr_size);
            size_t avail = (size_t)block_size - hdr_size;
            size_t copy_size = (bytes_remaining < avail)
                                   ? (size_t)bytes_remaining
                                   : avail;
            memcpy(block_buf + hdr_size, ctx->bitmap + offset,
                   copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        } else {
            size_t copy_size = (bytes_remaining < block_size)
                                   ? (size_t)bytes_remaining
                                   : (size_t)block_size;
            memcpy(block_buf, ctx->bitmap + offset, copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        }

        rc = obmafs3_block_write(ctx, ctx->sb.bitmap_lba + i,
                                 block_buf, (size_t)block_size);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            return rc;
        }
    }

    free(block_buf);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Bit manipulation                                                   */
/* ------------------------------------------------------------------ */

/**
 * Mark a contiguous range of blocks as allocated in the bitmap.
 *
 * @param ctx    Filesystem context.
 * @param lba    Starting logical block address.
 * @param count  Number of blocks to set.
 */
void obmafs3_bitmap_set(struct obmafs3_ctx *ctx, uint64_t lba,
                        uint64_t count)
{
    if (!ctx->bitmap)
        return;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t bit = lba + i;
        if (bit / 8 < ctx->bitmap_size)
            ctx->bitmap[bit / 8] |= (1u << (bit % 8));
    }
}

/**
 * Mark a contiguous range of blocks as free in the bitmap.
 *
 * @param ctx    Filesystem context.
 * @param lba    Starting logical block address.
 * @param count  Number of blocks to clear.
 */
void obmafs3_bitmap_clear(struct obmafs3_ctx *ctx, uint64_t lba,
                          uint64_t count)
{
    if (!ctx->bitmap)
        return;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t bit = lba + i;
        if (bit / 8 < ctx->bitmap_size)
            ctx->bitmap[bit / 8] &= ~(1u << (bit % 8));
    }
}

/**
 * Test whether a single block is allocated.
 *
 * @param ctx  Filesystem context.
 * @param lba  Logical block address to test.
 * @return 1 if the block is allocated, 0 if free or out of range.
 */
int obmafs3_bitmap_is_set(struct obmafs3_ctx *ctx, uint64_t lba)
{
    if (!ctx->bitmap)
        return 0;
    if (lba / 8 >= ctx->bitmap_size)
        return 0;
    return (ctx->bitmap[lba / 8] >> (lba % 8)) & 1;
}

/* ------------------------------------------------------------------ */
/*  Free block search                                                  */
/* ------------------------------------------------------------------ */

/**
 * Find a contiguous run of free blocks in the bitmap.
 *
 * Scans the bitmap for @p count consecutive unallocated blocks.
 *
 * @param ctx        Filesystem context.
 * @param count      Number of contiguous free blocks required.
 * @param start_lba  Output LBA of the first free block in the run.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_NOSPC if no
 *         sufficiently large run exists.
 */
int obmafs3_bitmap_find_free(struct obmafs3_ctx *ctx, uint64_t count,
                             uint64_t *start_lba)
{
    if (!ctx->bitmap)
        return OBMAFS3_ERR_INVAL;

    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
    uint64_t run_start = 0;
    uint64_t run_len = 0;

    for (uint64_t bit = 0; bit < total_blocks; bit++) {
        if (!obmafs3_bitmap_is_set(ctx, bit)) {
            if (run_len == 0)
                run_start = bit;
            run_len++;
            if (run_len >= count) {
                *start_lba = run_start;
                return OBMAFS3_OK;
            }
        } else {
            run_len = 0;
        }
    }

    return OBMAFS3_ERR_NOSPC;
}

/* ------------------------------------------------------------------ */
/*  Block freeing                                                      */
/* ------------------------------------------------------------------ */

/**
 * Free a single block and persist the bitmap.
 *
 * @param ctx  Filesystem context.
 * @param lba  Logical block address to free.
 * @return @c OBMAFS3_OK on success, or an error code on I/O failure.
 */
int obmafs3_free_block(struct obmafs3_ctx *ctx, uint64_t lba)
{
    return obmafs3_free_blocks(ctx, lba, 1);
}

/**
 * Free a contiguous range of blocks and persist the bitmap.
 *
 * @param ctx    Filesystem context.
 * @param lba    Starting logical block address.
 * @param count  Number of blocks to free.
 * @return @c OBMAFS3_OK on success, or an error code on I/O failure.
 */
int obmafs3_free_blocks(struct obmafs3_ctx *ctx, uint64_t lba,
                        uint64_t count)
{
    obmafs3_bitmap_clear(ctx, lba, count);
    return obmafs3_bitmap_write(ctx);
}
