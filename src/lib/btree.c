// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : btree.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 common B+Tree operations.
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

/*
 * Header read/write and block allocation helpers shared by all
 * B+Tree implementations (catalog, inode, media_tag, cd_btree,
 * metadata).
 */
#include "btree_internal.h"
#include "debug.h"

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
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->hdr_buf;

    int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK) return rc;

    memcpy(hdr, buf, sizeof(*hdr));

    if(hdr->magic != OBMAFS3_BTREE_HDR_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

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
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->hdr_buf;
    memset(buf, 0, (size_t)ctx->sb.block_size);

    memcpy(buf, hdr, sizeof(*hdr));

    /* Compute checksum: zero field, hash the struct, store result */
    struct btree_header *hdr_buf = (struct btree_header *)buf;
    memset(hdr_buf->checksum, 0, sizeof(hdr_buf->checksum));
    obmafs3_checksum_block(buf, sizeof(*hdr), hdr_buf->checksum);

    /* Copy the computed checksum back so the caller stays in sync */
    memcpy(hdr->checksum, hdr_buf->checksum, sizeof(hdr->checksum));

    return obmafs3_block_write(ctx, lba, buf, (size_t)ctx->sb.block_size);
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

/* ------------------------------------------------------------------ */
/*  Clump-aware B+Tree node allocation                                 */
/* ------------------------------------------------------------------ */

/**
 * Allocate a B+Tree node using the free-node list with clump growth.
 *
 * If the tree header's free-node list is non-empty, pops the first free
 * node.  Otherwise, allocates a contiguous clump of @c clump_size nodes
 * (falling back to smaller sizes if contiguous space is unavailable),
 * links all but the first into the free-node list, and returns the
 * first.
 *
 * The btree header (@p hdr) is updated in memory but NOT written to
 * disk — the caller is responsible for persisting it.
 *
 * For multi-block nodes (node_size > block_size), each "node" occupies
 * node_size/block_size contiguous blocks.
 *
 * @param ctx       Filesystem context.
 * @param hdr       In-memory btree header (updated on return).
 * @param hdr_lba   LBA where the btree header is stored on disk (unused today, reserved).
 * @param node_lba  Output: LBA of the newly allocated node.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_btree_alloc_node(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, uint64_t *node_lba)
{
    (void)hdr_lba;
    size_t   bsz             = (size_t)ctx->sb.block_size;
    uint64_t blocks_per_node = hdr->node_size / ctx->sb.block_size;
    if(blocks_per_node == 0) blocks_per_node = 1;

    /* ---- Pop from free list ---- */
    if(hdr->free_node_lba != 0 && hdr->free_nodes > 0)
    {
        *node_lba = hdr->free_node_lba;

        /* Read the free node to get the next pointer (stored as uint64_t at offset 0) */
        uint8_t *buf = obmafs3_get_thread_bufs(ctx)->hdr_buf;
        int      rc  = obmafs3_block_read(ctx, hdr->free_node_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        uint64_t next_free;
        memcpy(&next_free, buf, sizeof(next_free));

        hdr->free_node_lba = next_free;
        hdr->free_nodes--;

        return OBMAFS3_OK;
    }

    /* ---- Free list empty: allocate a clump ---- */
    uint32_t clump = (hdr->tree_type == kBtreeTypeDeduplication) ? ctx->sb.dedup_clump_size : ctx->sb.btree_clump_size;
    if(clump == 0)
        clump = (hdr->tree_type == kBtreeTypeDeduplication) ? OBMAFS3_DEDUP_CLUMP_SIZE : OBMAFS3_DEFAULT_CLUMP_SIZE;

    uint64_t alloc_blocks = (uint64_t)clump * blocks_per_node;
    uint64_t start_lba;
    int      rc;

    /* Try the full clump first; if that fails, halve until 1 */
    while(alloc_blocks > blocks_per_node)
    {
        rc = obmafs3_alloc_blocks(ctx, alloc_blocks, &start_lba);
        if(rc == OBMAFS3_OK) goto clump_allocated;
        alloc_blocks /= 2;
        /* Round down to a multiple of blocks_per_node */
        alloc_blocks = (alloc_blocks / blocks_per_node) * blocks_per_node;
        if(alloc_blocks < blocks_per_node) alloc_blocks = blocks_per_node;
    }

    /* Last resort: single node */
    rc = obmafs3_alloc_blocks(ctx, blocks_per_node, &start_lba);
    if(rc != OBMAFS3_OK) return rc;

clump_allocated:;
    uint64_t nodes_in_clump = alloc_blocks / blocks_per_node;

    /* Return the first node */
    *node_lba = start_lba;

    /* Link remaining nodes into the free list */
    if(nodes_in_clump > 1)
    {
        uint8_t *buf = calloc(1, bsz);
        if(!buf) return OBMAFS3_OK; /* first node is usable even without free list */

        /* Build chain: node[1] -> node[2] -> ... -> node[N-1] -> old_free_head */
        uint64_t old_head = hdr->free_node_lba;

        for(uint64_t i = nodes_in_clump - 1; i >= 1; i--)
        {
            uint64_t this_lba = start_lba + i * blocks_per_node;
            uint64_t next_ptr = (i == nodes_in_clump - 1) ? old_head : (start_lba + (i + 1) * blocks_per_node);

            memset(buf, 0, bsz);
            memcpy(buf, &next_ptr, sizeof(next_ptr));

            rc = obmafs3_block_write(ctx, this_lba, buf, bsz);
            if(rc != OBMAFS3_OK)
            {
                free(buf);
                return OBMAFS3_OK; /* first node still usable */
            }
        }

        free(buf);

        hdr->free_node_lba = start_lba + blocks_per_node; /* points to node[1] */
        hdr->free_nodes += (uint32_t)(nodes_in_clump - 1);
    }

    return OBMAFS3_OK;
}

/**
 * Return a B+Tree node to the tree's free-node list.
 *
 * Pushes the node onto the head of the free list.  Updates @p hdr in
 * memory but does NOT write it to disk — the caller is responsible for
 * persisting it.
 *
 * @param ctx       Filesystem context.
 * @param hdr       In-memory btree header (updated on return).
 * @param hdr_lba   LBA where the btree header is stored on disk (unused today, reserved).
 * @param node_lba  LBA of the node to free.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_btree_free_node(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, uint64_t node_lba)
{
    (void)hdr_lba;
    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Write the current free list head as the next pointer */
    uint64_t next_ptr = hdr->free_node_lba;
    memcpy(buf, &next_ptr, sizeof(next_ptr));

    int rc = obmafs3_block_write(ctx, node_lba, buf, bsz);
    free(buf);
    if(rc != OBMAFS3_OK) return rc;

    hdr->free_node_lba = node_lba;
    hdr->free_nodes++;

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
