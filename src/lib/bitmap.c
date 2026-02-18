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
    uint64_t block_size   = ctx->sb.block_size;
    uint64_t total_blocks = ctx->sb.total_bytes / block_size;
    uint64_t bitmap_bytes = (total_blocks + 7) / 8;
    size_t   hdr_size     = sizeof(struct bitmap_header);
    int      rc;

    if(ctx->bitmap)
    {
        free(ctx->bitmap);
        ctx->bitmap = NULL;
    }

    ctx->bitmap = calloc(1, (size_t)bitmap_bytes);
    if(!ctx->bitmap) return OBMAFS3_ERR_NOMEM;
    ctx->bitmap_size = bitmap_bytes;

    uint8_t *block_buf = malloc((size_t)block_size);
    if(!block_buf)
    {
        free(ctx->bitmap);
        ctx->bitmap = NULL;
        return OBMAFS3_ERR_NOMEM;
    }

    uint64_t bytes_remaining = bitmap_bytes;
    uint64_t offset          = 0;

    for(uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++)
    {
        rc = obmafs3_block_read(ctx, ctx->sb.bitmap_lba + i, block_buf, (size_t)block_size);
        if(rc != OBMAFS3_OK)
        {
            free(block_buf);
            free(ctx->bitmap);
            ctx->bitmap = NULL;
            return rc;
        }

        if(i == 0)
        {
            /* First block: validate header */
            struct bitmap_header bhdr;
            memcpy(&bhdr, block_buf, hdr_size);

            if(bhdr.magic != OBMAFS3_BITMAP_MAGIC)
            {
                free(block_buf);
                free(ctx->bitmap);
                ctx->bitmap = NULL;
                return OBMAFS3_ERR_BADMAGIC;
            }

            ctx->next_free_lba = bhdr.next_free_lba;

            /* Copy bitmap data after the header */
            size_t avail     = (size_t)block_size - hdr_size;
            size_t copy_size = (bytes_remaining < avail) ? (size_t)bytes_remaining : avail;
            memcpy(ctx->bitmap, block_buf + hdr_size, copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        }
        else
        {
            size_t copy_size = (bytes_remaining < block_size) ? (size_t)bytes_remaining : (size_t)block_size;
            memcpy(ctx->bitmap + offset, block_buf, copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        }
    }

    /* Verify checksum */
    {
        struct bitmap_header bhdr;
        rc = obmafs3_block_read(ctx, ctx->sb.bitmap_lba, block_buf, (size_t)block_size);
        if(rc == OBMAFS3_OK)
        {
            memcpy(&bhdr, block_buf, hdr_size);
            uint8_t computed[32];
            obmafs3_checksum_block(ctx->bitmap, (size_t)bitmap_bytes, computed);
            if(memcmp(bhdr.checksum, computed, 32) != 0)
            {
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
    if(!ctx->bitmap) return OBMAFS3_ERR_INVAL;

    uint64_t block_size = ctx->sb.block_size;
    size_t   hdr_size   = sizeof(struct bitmap_header);
    uint8_t *block_buf  = calloc(1, (size_t)block_size);
    if(!block_buf) return OBMAFS3_ERR_NOMEM;

    /* Build header with fresh checksum */
    struct bitmap_header bhdr;
    memset(&bhdr, 0, sizeof(bhdr));
    bhdr.magic         = OBMAFS3_BITMAP_MAGIC;
    bhdr.total_blocks  = ctx->sb.total_bytes / ctx->sb.block_size;
    bhdr.next_free_lba = ctx->next_free_lba;
    obmafs3_checksum_block(ctx->bitmap, (size_t)ctx->bitmap_size, bhdr.checksum);

    uint64_t bytes_remaining = ctx->bitmap_size;
    uint64_t offset          = 0;
    int      rc;

    for(uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++)
    {
        memset(block_buf, 0, (size_t)block_size);

        if(i == 0)
        {
            /* First block: header + bitmap data */
            memcpy(block_buf, &bhdr, hdr_size);
            size_t avail     = (size_t)block_size - hdr_size;
            size_t copy_size = (bytes_remaining < avail) ? (size_t)bytes_remaining : avail;
            memcpy(block_buf + hdr_size, ctx->bitmap + offset, copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        }
        else
        {
            size_t copy_size = (bytes_remaining < block_size) ? (size_t)bytes_remaining : (size_t)block_size;
            memcpy(block_buf, ctx->bitmap + offset, copy_size);
            offset += copy_size;
            bytes_remaining -= copy_size;
        }

        rc = obmafs3_block_write(ctx, ctx->sb.bitmap_lba + i, block_buf, (size_t)block_size);
        if(rc != OBMAFS3_OK)
        {
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
void obmafs3_bitmap_set(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t count)
{
    if(!ctx->bitmap) return;
    for(uint64_t i = 0; i < count; i++)
    {
        uint64_t bit = lba + i;
        if(bit / 8 < ctx->bitmap_size) ctx->bitmap[bit / 8] |= (1u << (bit % 8));
    }
}

/**
 * Mark a contiguous range of blocks as free in the bitmap.
 *
 * @param ctx    Filesystem context.
 * @param lba    Starting logical block address.
 * @param count  Number of blocks to clear.
 */
void obmafs3_bitmap_clear(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t count)
{
    if(!ctx->bitmap) return;
    for(uint64_t i = 0; i < count; i++)
    {
        uint64_t bit = lba + i;
        if(bit / 8 < ctx->bitmap_size) ctx->bitmap[bit / 8] &= ~(1u << (bit % 8));
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
    if(!ctx->bitmap) return 0;
    if(lba / 8 >= ctx->bitmap_size) return 0;
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
int obmafs3_bitmap_find_free(struct obmafs3_ctx *ctx, uint64_t count, uint64_t *start_lba)
{
    if(!ctx->bitmap) return OBMAFS3_ERR_INVAL;

    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
    if(total_blocks == 0) return OBMAFS3_ERR_NOSPC;

    /* Clamp the hint so it stays inside the volume */
    uint64_t hint = ctx->next_free_lba;
    if(hint >= total_blocks) hint = 0;

    /*
     * Word-level scan: cast the bitmap to 64-bit words so we can skip
     * 64 allocated bits at a time.  A word value of 0xFFFFFFFFFFFFFFFF
     * means all 64 bits are allocated and can be skipped entirely.
     */
    const uint64_t *words      = (const uint64_t *)ctx->bitmap;
    uint64_t        total_bits = total_blocks;

    /* We do two passes: [hint .. end) then [0 .. hint).  The second
     * pass is only needed when the hint is non-zero and the first
     * pass failed. */
    uint64_t pass_starts[2] = {hint, 0};
    uint64_t pass_ends[2]   = {total_bits, hint};
    int      passes         = (hint > 0) ? 2 : 1;

    for(int p = 0; p < passes; p++)
    {
        uint64_t scan_start = pass_starts[p];
        uint64_t scan_end   = pass_ends[p];
        if(scan_start >= scan_end) continue;

        uint64_t run_start = 0;
        uint64_t run_len   = 0;
        uint64_t bit       = scan_start;

        while(bit < scan_end)
        {
            uint64_t word_idx    = bit / 64;
            unsigned bit_in_word = (unsigned)(bit % 64);

            uint64_t word = words[word_idx];

            /* Mask out bits below our current position within the word */
            if(bit_in_word != 0)
            {
                /* Set lower bits so they look "allocated" and are skipped */
                word |= ((uint64_t)1 << bit_in_word) - 1;
            }

            /* Mask out bits beyond total_blocks in the last word */
            uint64_t word_end = (word_idx + 1) * 64;
            if(word_end > scan_end)
            {
                unsigned excess = (unsigned)(word_end - scan_end);
                word |= ~(((uint64_t)1 << (64 - excess)) - 1);
            }

            if(word == UINT64_MAX)
            {
                /* Entire word is allocated – skip it */
                run_len = 0;
                bit = (word_idx + 1) * 64;
                continue;
            }

            /* Scan individual free bits within this word using __builtin_ctzll */
            uint64_t free_mask = ~word; /* 1-bits mark free blocks */
            while(free_mask)
            {
                unsigned pos = (unsigned)__builtin_ctzll(free_mask);
                uint64_t abs_bit = word_idx * 64 + pos;

                if(abs_bit >= scan_end) break;

                if(run_len == 0 || abs_bit != run_start + run_len)
                {
                    /* Not contiguous – restart the run */
                    run_start = abs_bit;
                    run_len   = 1;
                }
                else
                {
                    run_len++;
                }

                if(run_len >= count)
                {
                    *start_lba = run_start;
                    return OBMAFS3_OK;
                }

                free_mask &= free_mask - 1; /* clear lowest set bit */
            }

            bit = (word_idx + 1) * 64;
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
int obmafs3_free_block(struct obmafs3_ctx *ctx, uint64_t lba) { return obmafs3_free_blocks(ctx, lba, 1); }

/**
 * Free a contiguous range of blocks and persist the bitmap.
 *
 * @param ctx    Filesystem context.
 * @param lba    Starting logical block address.
 * @param count  Number of blocks to free.
 * @return @c OBMAFS3_OK on success, or an error code on I/O failure.
 */
int obmafs3_free_blocks(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t count)
{
    obmafs3_bitmap_clear(ctx, lba, count);
    /* Bitmap is persisted on flush/release, not per free. */
    return OBMAFS3_OK;
}
