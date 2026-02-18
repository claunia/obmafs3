/*
 * btree.c - OBMAFS3 common B+Tree operations
 *
 * Header read/write and block allocation helpers shared by all
 * B+Tree implementations (catalog, inode, media_tag, cd_btree,
 * metadata).
 */
#include "btree_internal.h"

/**
 * Read a B+Tree header block from disk (strict).
 *
 * Reads the header and verifies its checksum.  Returns an error if the
 * checksum does not match.
 *
 * @param ctx  Filesystem context.
 * @param lba  Logical block address of the header.
 * @param hdr  Output header structure.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_CHECKSUM on bad
 *         checksum, or another error code on failure.
 */
int obmafs3_btree_header_read(struct obmafs3_ctx *ctx, uint64_t lba, struct btree_header *hdr)
{
    int cs_ok = 0;
    int rc    = obmafs3_btree_header_read_lenient(ctx, lba, hdr, &cs_ok);
    if(rc != OBMAFS3_OK) return rc;
    return cs_ok ? OBMAFS3_OK : OBMAFS3_ERR_CHECKSUM;
}

/**
 * Read a B+Tree header block from disk (lenient).
 *
 * Reads the header and validates the magic number.  The checksum is
 * verified but a mismatch is reported via @p checksum_ok rather than
 * causing an error return.
 *
 * @param ctx          Filesystem context.
 * @param lba          Logical block address of the header.
 * @param hdr          Output header structure.
 * @param checksum_ok  Set to 1 if the checksum matches, 0 otherwise.
 * @return @c OBMAFS3_OK on success, or an error code on read/magic failure.
 */
int obmafs3_btree_header_read_lenient(struct obmafs3_ctx *ctx, uint64_t lba, struct btree_header *hdr, int *checksum_ok)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }

    memcpy(hdr, buf, sizeof(*hdr));
    free(buf);

    if(hdr->magic != OBMAFS3_BTREE_HDR_MAGIC) return OBMAFS3_ERR_BADMAGIC;

    /* Verify checksum: save stored checksum, zero field, recompute */
    uint8_t stored[32];
    memcpy(stored, hdr->checksum, 32);
    memset(hdr->checksum, 0, 32);
    uint8_t computed[32];
    obmafs3_checksum_block(hdr, sizeof(*hdr), computed);
    memcpy(hdr->checksum, stored, 32);
    *checksum_ok = (memcmp(stored, computed, 32) == 0);

    return OBMAFS3_OK;
}

/**
 * Write a B+Tree header block to disk.
 *
 * Computes a fresh checksum over the header structure and writes the
 * entire block to the given LBA.
 *
 * @param ctx  Filesystem context.
 * @param lba  Logical block address to write to.
 * @param hdr  Pointer to the header structure to write.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_btree_header_write(struct obmafs3_ctx *ctx, uint64_t lba, struct btree_header *hdr)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    memcpy(buf, hdr, sizeof(*hdr));

    /* Compute checksum: zero field, hash the struct, store result */
    struct btree_header *hdr_buf = (struct btree_header *)buf;
    memset(hdr_buf->checksum, 0, sizeof(hdr_buf->checksum));
    obmafs3_checksum_block(buf, sizeof(*hdr), hdr_buf->checksum);

    /* Copy the computed checksum back so the caller stays in sync */
    memcpy(hdr->checksum, hdr_buf->checksum, sizeof(hdr->checksum));

    int rc = obmafs3_block_write(ctx, lba, buf, (size_t)ctx->sb.block_size);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Block allocation                                                   */
/* ------------------------------------------------------------------ */

/**
 * Allocate a single block from the free-space bitmap.
 *
 * @param ctx  Filesystem context.
 * @param lba  Output LBA of the allocated block.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_alloc_block(struct obmafs3_ctx *ctx, uint64_t *lba) { return obmafs3_alloc_blocks(ctx, 1, lba); }

/**
 * Allocate a contiguous range of blocks from the free-space bitmap.
 *
 * Finds @p count contiguous free blocks, marks them as allocated in the
 * bitmap, persists the bitmap and superblock to disk, and updates the
 * @c next_free_lba hint.
 *
 * @param ctx        Filesystem context.
 * @param count      Number of contiguous blocks to allocate.
 * @param start_lba  Output LBA of the first allocated block.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_alloc_blocks(struct obmafs3_ctx *ctx, uint64_t count, uint64_t *start_lba)
{
    /* Find contiguous free blocks via the bitmap */
    int rc = obmafs3_bitmap_find_free(ctx, count, start_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Mark them as allocated (in-memory only; bitmap is persisted on
     * flush/release, superblock on unmount). */
    obmafs3_bitmap_set(ctx, *start_lba, count);

    /* Keep next_free_lba as a hint for future allocations */
    if(*start_lba + count > ctx->next_free_lba) ctx->next_free_lba = *start_lba + count;

    return OBMAFS3_OK;
}

/**
 * Allocate a new inode ID.
 *
 * Returns the current @c next_inode_id from the superblock and
 * increments it for the next allocation.
 *
 * @param ctx  Filesystem context.
 * @return The newly allocated inode ID.
 */
uint64_t obmafs3_alloc_inode_id(struct obmafs3_ctx *ctx)
{
    uint64_t id = ctx->sb.next_inode_id;
    ctx->sb.next_inode_id++;
    /* Superblock is persisted on unmount, not per inode allocation. */
    return id;
}
