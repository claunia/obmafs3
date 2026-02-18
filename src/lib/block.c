/*
 * block.c - OBMAFS3 block I/O, compression, and file data reading/writing
 */
#include "obmafs.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>

/**
 * Compress data using ZSTD.
 *
 * @param cctx      Reusable ZSTD compression context.
 * @param src       Source data buffer.
 * @param src_size  Number of bytes in @p src.
 * @param dst       Destination buffer for compressed data.
 * @param dst_size  On input, capacity of @p dst; on output, compressed size.
 * @param level     ZSTD compression level (1–22).
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on error.
 */
int obmafs3_compress(ZSTD_CCtx *cctx, const void *src, size_t src_size, void *dst, size_t *dst_size, int level)
{
    size_t result = ZSTD_compressCCtx(cctx, dst, *dst_size, src, src_size, level);
    if(ZSTD_isError(result)) return OBMAFS3_ERR_IO;
    *dst_size = result;
    return OBMAFS3_OK;
}

/**
 * Decompress ZSTD-compressed data.
 *
 * @param dctx      Reusable ZSTD decompression context.
 * @param src       Compressed data buffer.
 * @param src_size  Number of compressed bytes.
 * @param dst       Output buffer for decompressed data.
 * @param dst_size  Capacity of @p dst (must be >= original size).
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on error.
 */
int obmafs3_decompress(ZSTD_DCtx *dctx, const void *src, size_t src_size, void *dst, size_t dst_size)
{
    size_t result = ZSTD_decompressDCtx(dctx, dst, dst_size, src, src_size);
    if(ZSTD_isError(result)) return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Overflow extent B+Tree helpers                                     */
/* ------------------------------------------------------------------ */

/** Compute and store the checksum for a btree node block. */
static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr      = (struct btree_node_header *)buf;
    size_t                    data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

/** Maximum overflow_extent records in a leaf node. */
static uint16_t overflow_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct overflow_extent));
}

/** Maximum btree_index_entry entries in an index node. */
static uint16_t overflow_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct btree_index_entry));
}

/**
 * Binary search for (inode_id, logical_offset) in an overflow leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int overflow_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, uint64_t logical_offset)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int                    mid = lo + (hi - lo) / 2;
        struct overflow_extent oe;
        memcpy(&oe, data + (size_t)mid * sizeof(oe), sizeof(oe));

        if(oe.inode_id < inode_id) { lo = mid + 1; }
        else if(oe.inode_id > inode_id) { hi = mid - 1; }
        else if(oe.logical_offset < logical_offset) { lo = mid + 1; }
        else if(oe.logical_offset > logical_offset) { hi = mid - 1; }
        else
        {
            return mid;
        }
    }

    return -(lo + 1);
}

/**
 * Binary search in an overflow index node for the child covering inode_id.
 * Returns the slot index of the child pointer to follow.
 */
static uint16_t overflow_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int      mid = lo + (hi - lo) / 2;
        uint64_t mid_key;
        memcpy(&mid_key, data + (size_t)mid * sizeof(struct btree_index_entry), sizeof(mid_key));
        if(mid_key <= inode_id)
        {
            result = (uint16_t)mid;
            lo     = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    return result;
}

#define OVERFLOW_BTREE_MAX_DEPTH 8

struct overflow_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/**
 * Insert an extent into the overflow B+Tree.
 * Entries are sorted by (inode_id, start_block).
 */
static int overflow_insert(struct obmafs3_ctx *ctx, const struct overflow_extent *entry)
{
    struct btree_header *hdr     = &ctx->overflow_hdr;
    uint64_t             hdr_lba = ctx->sb.overflow_lba;
    size_t               bsz     = (size_t)ctx->sb.block_size;
    int                  rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if(hdr->root_node_lba == 0)
    {
        uint64_t root_lba;
        rc = obmafs3_alloc_block(ctx, &root_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = ctx->node_buf;
        memset(buf, 0, bsz);

        struct btree_node_header nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nhdr.record_type = kBtreeDataTypeExtent;
        nhdr.level       = 0;
        nhdr.node_keys   = 1;
        nhdr.keys_length = (uint16_t)sizeof(struct overflow_extent);
        memcpy(buf, &nhdr, sizeof(nhdr));
        memcpy(buf + sizeof(nhdr), entry, sizeof(*entry));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, root_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        hdr->root_node_lba = root_lba;
        hdr->total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = ctx->node_buf;

    struct overflow_btree_path path[OVERFLOW_BTREE_MAX_DEPTH];
    int                        depth = 0;
    uint64_t                   lba   = hdr->root_node_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        if(nhdr.level == 0) break; /* reached leaf */

        if(depth >= OVERFLOW_BTREE_MAX_DEPTH) return OBMAFS3_ERR_INVAL;

        uint16_t slot    = overflow_index_find(buf, nhdr.node_keys, entry->inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int      idx        = overflow_leaf_find(buf, leaf_hdr.node_keys, entry->inode_id, entry->logical_offset);
    int      insert_pos = (idx >= 0) ? idx : -(idx + 1);
    uint16_t max_leaf   = overflow_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct overflow_extent);

    if(leaf_hdr.node_keys < max_leaf)
    {
        /* Room in leaf — sorted insert */
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if(insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz, data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, entry, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        return rc;
    }

    /* ---- Leaf is full: split ---- */
    uint16_t                total = max_leaf + 1;
    struct overflow_extent *all   = calloc(total, rec_sz);
    if(!all) return OBMAFS3_ERR_NOMEM;

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    /* Build sorted array of all records including the new one */
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    all[insert_pos] = *entry;
    memcpy(&all[insert_pos + 1], leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    /* Rewrite old leaf with left half */
    memset(leaf_data, 0, bsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;

    uint64_t new_leaf_lba;
    rc = obmafs3_alloc_block(ctx, &new_leaf_lba);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        return rc;
    }

    leaf_hdr.node_keys   = left_count;
    leaf_hdr.keys_length = (uint16_t)(left_count * rec_sz);
    leaf_hdr.right_link  = new_leaf_lba;
    memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
    compute_node_checksum(buf);
    rc = obmafs3_block_write(ctx, lba, buf, bsz);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        return rc;
    }

    /* Write new leaf with right half */
    memset(buf, 0, bsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeExtent;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), &all[left_count], (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = obmafs3_block_write(ctx, new_leaf_lba, buf, bsz);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        return rc;
    }

    uint64_t push_key       = all[left_count].inode_id;
    uint64_t push_child     = new_leaf_lba;
    uint64_t left_first_key = all[0].inode_id;
    uint64_t left_lba       = lba;

    free(all);
    hdr->total_nodes++;

    /* ---- Propagate split upward through index nodes ---- */
    while(depth > 0)
    {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = obmafs3_block_read(ctx, parent_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = overflow_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct btree_index_entry);

        if(phdr.node_keys < max_idx)
        {
            /* Room in parent — insert */
            uint8_t *id = buf + sizeof(struct btree_node_header);

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz, id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);

            struct btree_index_entry ne;
            ne.key       = push_key;
            ne.child_lba = push_child;
            memcpy(id + (size_t)idx_insert * ie_sz, &ne, sizeof(ne));

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            if(rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        }

        /* Parent is full — split the index node */
        uint16_t                  idx_total = max_idx + 1;
        struct btree_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie) return OBMAFS3_ERR_NOMEM;

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        aie[idx_insert].key       = push_key;
        aie[idx_insert].child_lba = push_child;
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        /* Rewrite old index with left half */
        memset(id, 0, bsz - sizeof(struct btree_node_header));
        memcpy(id, aie, (size_t)il * ie_sz);
        phdr.node_keys   = il;
        phdr.keys_length = (uint16_t)(il * ie_sz);
        memcpy(buf, &phdr, sizeof(phdr));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        uint64_t new_idx_lba;
        rc = obmafs3_alloc_block(ctx, &new_idx_lba);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        memset(buf, 0, bsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeExtent;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, new_idx_lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        push_key       = aie[il].key;
        push_child     = new_idx_lba;
        left_first_key = aie[0].key;
        left_lba       = parent_lba;

        free(aie);
        hdr->total_nodes++;
    }

    /* ---- Create new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_alloc_block(ctx, &new_root_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Read old root to get its level */
    rc = obmafs3_block_read(ctx, left_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, bsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeExtent;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * sizeof(struct btree_index_entry));
    memcpy(buf, &rh, sizeof(rh));

    struct btree_index_entry roots[2];
    roots[0].key       = left_first_key;
    roots[0].child_lba = left_lba;
    roots[1].key       = push_key;
    roots[1].child_lba = push_child;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    hdr->root_node_lba = new_root_lba;
    hdr->total_nodes++;
    return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
}

/* ------------------------------------------------------------------ */
/*  Extent descriptor — unified inline + overflow representation       */
/* ------------------------------------------------------------------ */

struct extent_descriptor
{
    uint64_t logical_start; /* first logical block covered */
    uint64_t logical_count; /* number of logical blocks    */
    uint64_t phys_start;    /* first physical LBA          */
    uint64_t phys_count;    /* number of physical blocks   */
};

/* ------------------------------------------------------------------ */
/*  Overflow extent helpers (variable-length extents)                  */
/* ------------------------------------------------------------------ */

/**
 * Count total logical blocks covered by inline extents.
 */
static uint64_t count_inline_logical(const struct inode_record *inode)
{
    uint64_t total = 0;
    for(int i = 0; i < 8; i++) total += inode->extents[i].logical_blocks;
    return total;
}

/**
 * Search the overflow B+Tree for the extent covering @p logical_block.
 *
 * Each overflow_extent now stores an absolute logical_offset, so we
 * simply look for the entry whose range contains the target block.
 *
 * @return 1 if found (out filled), 0 if not found.
 */
static int overflow_find_extent(struct obmafs3_ctx *ctx, uint64_t inode_id, uint64_t logical_block,
                                struct extent_descriptor *out)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return 0;

    uint8_t *buf = ctx->node_buf;
    uint64_t lba = hdr->root_node_lba;

    /* Traverse index levels to reach the leaf */
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return 0;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return 0;
        if(nhdr.level == 0) break;

        uint16_t                 slot = overflow_index_find(buf, nhdr.node_keys, inode_id);
        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf nodes for the inode's extents */
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return 0;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            past    = 0;
        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));

            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }

            if(logical_block >= oe.logical_offset && logical_block < oe.logical_offset + oe.logical_count)
            {
                out->logical_start = oe.logical_offset;
                out->logical_count = oe.logical_count;
                out->phys_start    = oe.start_block;
                out->phys_count    = oe.block_count;
                return 1;
            }
        }
        if(past) break;
        lba = nhdr.right_link;
    }

    return 0;
}

/**
 * Count the total number of LOGICAL blocks stored in overflow extents
 * for an inode.
 */
static uint64_t overflow_count_logical(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return 0;

    uint8_t *buf = ctx->node_buf;
    uint64_t lba = hdr->root_node_lba;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return 0;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return 0;
        if(nhdr.level == 0) break;

        uint16_t                 slot = overflow_index_find(buf, nhdr.node_keys, inode_id);
        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    uint64_t total = 0;
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            past    = 0;
        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));

            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }
            total += oe.logical_count;
        }
        if(past) break;
        lba = nhdr.right_link;
    }

    return total;
}

/**
 * Find the extent covering @p logical_block in the inode's full extent
 * map (inline extents first, then overflow B+Tree).
 *
 * @return 1 if found (out filled), 0 if not found.
 */
static int find_extent(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t logical_block,
                       struct extent_descriptor *out)
{
    uint64_t base = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count == 0 && inode->extents[i].logical_blocks == 0) continue;
        uint64_t lc = inode->extents[i].logical_blocks;
        if(logical_block >= base && logical_block < base + lc)
        {
            out->logical_start = base;
            out->logical_count = lc;
            out->phys_start    = inode->extents[i].start_block;
            out->phys_count    = inode->extents[i].block_count;
            return 1;
        }
        base += lc;
    }

    return overflow_find_extent(ctx, inode->inode_id, logical_block, out);
}

/* ------------------------------------------------------------------ */
/*  File data reading                                                  */
/* ------------------------------------------------------------------ */

/**
 * Read logical blocks from a single extent into @p out_buf.
 *
 * For uncompressed extents (logical_count == phys_count) the blocks
 * are read directly.  For compressed extents the physical blocks are
 * read, the block_header is parsed, and the data is decompressed.
 *
 * When reading the full extent (offset_in_ext == 0 && logical_count ==
 * extent logical_count), decompression goes directly into @p out_buf.
 * For partial reads, io_buf2 is used as a temporary decompression
 * target.
 *
 * @param ctx            Filesystem context.
 * @param ext            Extent descriptor.
 * @param first_logical  First logical block to read (absolute).
 * @param logical_count  Number of logical blocks to read.
 * @param out_buf        Output buffer (must be >= logical_count * block_size).
 * @return OBMAFS3_OK on success.
 */
static int read_extent_blocks(struct obmafs3_ctx *ctx, const struct extent_descriptor *ext, uint64_t first_logical,
                              uint64_t logical_count, uint8_t *out_buf)
{
    uint64_t block_size    = ctx->sb.block_size;
    uint64_t offset_in_ext = first_logical - ext->logical_start;

    if(ext->logical_count == ext->phys_count)
    {
        /* Uncompressed: read blocks directly into out_buf */
        for(uint64_t i = 0; i < logical_count; i++)
        {
            int rc = obmafs3_block_read(ctx, ext->phys_start + offset_in_ext + i,
                                        out_buf + i * (size_t)block_size, (size_t)block_size);
            if(rc != OBMAFS3_OK) return rc;
        }
    }
    else
    {
        /* Compressed extent: read all physical blocks into io_buf */
        uint8_t *phys_buf = ctx->io_buf;
        for(uint64_t b = 0; b < ext->phys_count; b++)
        {
            int rc =
                obmafs3_block_read(ctx, ext->phys_start + b, phys_buf + b * (size_t)block_size, (size_t)block_size);
            if(rc != OBMAFS3_OK) return rc;
        }

        /* Parse block_header at the start */
        struct block_header bhdr;
        memcpy(&bhdr, phys_buf, sizeof(bhdr));
        if(bhdr.magic != OBMAFS3_BLOCK_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        /* Verify checksum */
        size_t  check_size = (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? (size_t)bhdr.compressed_size
                                                                          : (size_t)bhdr.original_size;
        uint8_t computed[32];
        obmafs3_checksum_block(phys_buf + sizeof(bhdr), check_size, computed);
        if(memcmp(computed, bhdr.checksum, 32) != 0) return OBMAFS3_ERR_CHECKSUM;

        if(offset_in_ext == 0 && logical_count == ext->logical_count)
        {
            /* Full extent read → decompress directly into out_buf */
            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                int rc = obmafs3_decompress(ctx->zstd_dctx, phys_buf + sizeof(bhdr), (size_t)bhdr.compressed_size,
                                            out_buf, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK) return rc;
            }
            else
            {
                memcpy(out_buf, phys_buf + sizeof(bhdr), (size_t)bhdr.original_size);
            }
        }
        else
        {
            /* Partial read → decompress into io_buf2, copy the portion */
            uint8_t *decomp = ctx->io_buf2;
            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                int rc = obmafs3_decompress(ctx->zstd_dctx, phys_buf + sizeof(bhdr), (size_t)bhdr.compressed_size,
                                            decomp, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK) return rc;
            }
            else
            {
                memcpy(decomp, phys_buf + sizeof(bhdr), (size_t)bhdr.original_size);
            }

            size_t start_off  = (size_t)(offset_in_ext * block_size);
            size_t copy_bytes = (size_t)(logical_count * block_size);
            if(start_off + copy_bytes <= (size_t)bhdr.original_size)
            {
                memcpy(out_buf, decomp + start_off, copy_bytes);
            }
            else
            {
                size_t avail = (start_off < (size_t)bhdr.original_size) ? (size_t)bhdr.original_size - start_off : 0;
                if(avail > 0) memcpy(out_buf, decomp + start_off, avail);
                if(avail < copy_bytes) memset(out_buf + avail, 0, copy_bytes - avail);
            }
        }
    }

    return OBMAFS3_OK;
}

/**
 * Read file data from the filesystem.
 *
 * Resolves logical block offsets through the inode's inline extents and
 * overflow extent tree.  Uncompressed extents are read directly;
 * compressed extents are decompressed transparently with caching of the
 * last decompressed group to avoid redundant decompression for
 * sequential reads.
 *
 * @param ctx     Filesystem context.
 * @param inode   Inode record describing the file.
 * @param offset  Byte offset within the file to start reading.
 * @param buf     Output buffer.
 * @param size    Number of bytes to read.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_read_file_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                           size_t size)
{
    uint64_t block_size = ctx->sb.block_size;
    size_t   bytes_read = 0;

    if(offset >= inode->file_size) return OBMAFS3_OK;
    if(offset + size > inode->file_size) size = (size_t)(inode->file_size - offset);

    /* Cache last decompressed compressed extent to avoid re-decompressing
     * for sequential reads within the same group. */
    struct extent_descriptor cached_ext;
    memset(&cached_ext, 0, sizeof(cached_ext));
    int      cached_valid = 0;
    uint8_t *decomp_buf   = ctx->io_buf2;

    while(bytes_read < size)
    {
        uint64_t read_pos     = offset + bytes_read;
        uint64_t logical_blk  = read_pos / block_size;
        size_t   off_in_block = (size_t)(read_pos % block_size);

        struct extent_descriptor ext;
        if(!find_extent(ctx, inode, logical_blk, &ext)) return OBMAFS3_ERR_IO;

        if(ext.logical_count == ext.phys_count)
        {
            /* Uncompressed extent — direct per-block read */
            uint64_t phys_lba  = ext.phys_start + (logical_blk - ext.logical_start);
            uint8_t *block_buf = ctx->io_buf;
            int      rc        = obmafs3_block_read(ctx, phys_lba, block_buf, (size_t)block_size);
            if(rc != OBMAFS3_OK) return rc;

            size_t avail   = (size_t)block_size - off_in_block;
            size_t to_copy = (size - bytes_read < avail) ? size - bytes_read : avail;
            memcpy((uint8_t *)buf + bytes_read, block_buf + off_in_block, to_copy);
            bytes_read += to_copy;
        }
        else
        {
            /* Compressed extent — decompress once, serve multiple blocks */
            if(!cached_valid || cached_ext.phys_start != ext.phys_start)
            {
                /* Read all physical blocks of the extent */
                uint8_t *phys_buf = ctx->io_buf;
                for(uint64_t b = 0; b < ext.phys_count; b++)
                {
                    int rc = obmafs3_block_read(ctx, ext.phys_start + b, phys_buf + b * (size_t)block_size,
                                                (size_t)block_size);
                    if(rc != OBMAFS3_OK) return rc;
                }

                struct block_header bhdr;
                memcpy(&bhdr, phys_buf, sizeof(bhdr));
                if(bhdr.magic != OBMAFS3_BLOCK_MAGIC) return OBMAFS3_ERR_BADMAGIC;

                size_t  check_size = (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? (size_t)bhdr.compressed_size
                                                                                  : (size_t)bhdr.original_size;
                uint8_t computed[32];
                obmafs3_checksum_block(phys_buf + sizeof(bhdr), check_size, computed);
                if(memcmp(computed, bhdr.checksum, 32) != 0) return OBMAFS3_ERR_CHECKSUM;

                if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                {
                    int rc = obmafs3_decompress(ctx->zstd_dctx, phys_buf + sizeof(bhdr),
                                                (size_t)bhdr.compressed_size, decomp_buf,
                                                (size_t)bhdr.original_size);
                    if(rc != OBMAFS3_OK) return rc;
                }
                else
                {
                    memcpy(decomp_buf, phys_buf + sizeof(bhdr), (size_t)bhdr.original_size);
                }
                cached_ext   = ext;
                cached_valid = 1;
            }

            /* Extract data from decompressed buffer */
            uint64_t block_in_ext  = logical_blk - ext.logical_start;
            size_t   decomp_offset = (size_t)(block_in_ext * block_size + off_in_block);
            size_t   avail         = (size_t)block_size - off_in_block;
            size_t   to_copy       = (size - bytes_read < avail) ? size - bytes_read : avail;
            memcpy((uint8_t *)buf + bytes_read, decomp_buf + decomp_offset, to_copy);
            bytes_read += to_copy;
        }
    }

    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Overflow helpers for extent management                             */
/* ------------------------------------------------------------------ */

/**
 * Collect all overflow extents for @a inode_id as extent_descriptors.
 * The caller must free @c *out_exts when done.
 */
static int overflow_collect_extents(struct obmafs3_ctx *ctx, uint64_t inode_id, struct extent_descriptor **out_exts,
                                    uint64_t *out_count)
{
    *out_exts  = NULL;
    *out_count = 0;

    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct extent_descriptor *exts = NULL;
    uint64_t                  count = 0, cap = 0;

    /* Navigate to first relevant leaf */
    uint64_t lba = hdr->root_node_lba;
    for(;;)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.level == 0) break;
        uint16_t                 slot = overflow_index_find(buf, nhdr.node_keys, inode_id);
        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaves */
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            past    = 0;

        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));
            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }

            if(count >= cap)
            {
                cap                          = (cap == 0) ? 64 : cap * 2;
                struct extent_descriptor *tmp = realloc(exts, (size_t)cap * sizeof(*tmp));
                if(!tmp)
                {
                    free(exts);
                    free(buf);
                    return OBMAFS3_ERR_NOMEM;
                }
                exts = tmp;
            }
            exts[count].logical_start = oe.logical_offset;
            exts[count].logical_count = oe.logical_count;
            exts[count].phys_start    = oe.start_block;
            exts[count].phys_count    = oe.block_count;
            count++;
        }
        if(past) break;
        lba = nhdr.right_link;
    }

    free(buf);
    *out_exts  = exts;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Remove every overflow extent entry for @a inode_id from the
 * overflow B+Tree.  Entries for other inodes are left intact.
 *
 * The tree is @b not rebalanced; leaves may become underfull or
 * empty.  This is acceptable for a subsequent rebuild via
 * overflow_insert().
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode whose overflow entries are removed.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int overflow_clear_inode(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Navigate to the first leaf that may contain this inode */
    uint64_t lba = hdr->root_node_lba;
    for(;;)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.level == 0) break;
        uint16_t                 slot = overflow_index_find(buf, nhdr.node_keys, inode_id);
        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Walk leaves, compacting out entries that match inode_id */
    const size_t rec_sz = sizeof(struct overflow_extent);
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        uint8_t *entries  = buf + sizeof(struct btree_node_header);
        int      modified = 0, past = 0;
        uint16_t wp = 0;

        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * rec_sz, rec_sz);
            if(oe.inode_id == inode_id)
            {
                modified = 1;
                continue; /* skip / remove */
            }
            if(oe.inode_id > inode_id) past = 1;
            if(wp != i) memmove(entries + (size_t)wp * rec_sz, entries + (size_t)i * rec_sz, rec_sz);
            wp++;
        }

        if(modified)
        {
            if(wp < nhdr.node_keys) memset(entries + (size_t)wp * rec_sz, 0, ((size_t)nhdr.node_keys - wp) * rec_sz);
            nhdr.node_keys   = wp;
            nhdr.keys_length = (uint16_t)(wp * rec_sz);
            memcpy(buf, &nhdr, sizeof(nhdr));
            compute_node_checksum(buf);
            rc = obmafs3_block_write(ctx, lba, buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK)
            {
                free(buf);
                return rc;
            }
        }

        if(past) break;
        lba = nhdr.right_link;
    }

    free(buf);
    return OBMAFS3_OK;
}

/**
 * Collect ALL extents (inline + overflow) for an inode into a flat
 * array sorted by logical_start.  The caller must free @c *out.
 */
static int collect_all_extents(struct obmafs3_ctx *ctx, const struct inode_record *inode,
                               struct extent_descriptor **out, uint64_t *out_count)
{
    /* Count inline extents */
    uint64_t inline_count = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count > 0 || inode->extents[i].logical_blocks > 0) inline_count++;
    }

    /* Collect overflow extents */
    struct extent_descriptor *ovf_exts  = NULL;
    uint64_t                  ovf_count = 0;
    int rc = overflow_collect_extents(ctx, inode->inode_id, &ovf_exts, &ovf_count);
    if(rc != OBMAFS3_OK) return rc;

    uint64_t total = inline_count + ovf_count;
    if(total == 0)
    {
        free(ovf_exts);
        *out       = NULL;
        *out_count = 0;
        return OBMAFS3_OK;
    }

    struct extent_descriptor *list = calloc((size_t)total, sizeof(*list));
    if(!list)
    {
        free(ovf_exts);
        return OBMAFS3_ERR_NOMEM;
    }

    /* Add inline extents */
    uint64_t idx = 0, base = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count == 0 && inode->extents[i].logical_blocks == 0) continue;
        list[idx].logical_start = base;
        list[idx].logical_count = inode->extents[i].logical_blocks;
        list[idx].phys_start    = inode->extents[i].start_block;
        list[idx].phys_count    = inode->extents[i].block_count;
        base += inode->extents[i].logical_blocks;
        idx++;
    }

    /* Add overflow extents */
    if(ovf_count > 0)
    {
        memcpy(list + idx, ovf_exts, (size_t)ovf_count * sizeof(*list));
        idx += ovf_count;
    }
    free(ovf_exts);

    *out       = list;
    *out_count = idx;
    return OBMAFS3_OK;
}

/**
 * Free a single physical block, respecting refcounts.
 */
static void free_single_phys_block(struct obmafs3_ctx *ctx, uint64_t lba)
{
    uint32_t ref = 1;
    obmafs3_refcount_get(ctx, lba, &ref);
    if(ref > 1)
        obmafs3_refcount_dec(ctx, lba, NULL);
    else
        obmafs3_free_block(ctx, lba);
}

/**
 * Free all physical blocks of an extent, respecting refcounts.
 */
static void free_extent_phys(struct obmafs3_ctx *ctx, const struct extent_descriptor *ext)
{
    for(uint64_t b = 0; b < ext->phys_count; b++) free_single_phys_block(ctx, ext->phys_start + b);
}

/**
 * Write an extent list back to the inode's inline slots + overflow tree.
 *
 * Clears existing overflow entries, merges adjacent uncompressed
 * extents, fills inline slots, and spills the rest to overflow.
 * The input list must be sorted by logical_start.
 */
static int write_extent_list(struct obmafs3_ctx *ctx, struct inode_record *inode, const struct extent_descriptor *list,
                             uint64_t count)
{
    memset(inode->extents, 0, sizeof(inode->extents));

    int rc = overflow_clear_inode(ctx, inode->inode_id);
    if(rc != OBMAFS3_OK) return rc;

    if(count == 0) return OBMAFS3_OK;

    /* Coalesce adjacent uncompressed extents */
    struct extent_descriptor *merged = calloc((size_t)count, sizeof(*merged));
    if(!merged) return OBMAFS3_ERR_NOMEM;

    uint64_t mc = 0;
    merged[0]   = list[0];
    for(uint64_t i = 1; i < count; i++)
    {
        struct extent_descriptor *prev = &merged[mc];
        /* Merge only if both are uncompressed and physically contiguous */
        if(prev->logical_count == prev->phys_count && list[i].logical_count == list[i].phys_count &&
           prev->phys_start + prev->phys_count == list[i].phys_start &&
           prev->logical_start + prev->logical_count == list[i].logical_start)
        {
            prev->logical_count += list[i].logical_count;
            prev->phys_count += list[i].phys_count;
        }
        else
        {
            mc++;
            merged[mc] = list[i];
        }
    }
    mc++;

    /* First 8 extent runs go to inline slots */
    uint64_t inline_limit = (mc < 8) ? mc : 8;
    for(uint64_t i = 0; i < inline_limit; i++)
    {
        inode->extents[i].start_block    = merged[i].phys_start;
        inode->extents[i].block_count    = merged[i].phys_count;
        inode->extents[i].logical_blocks = merged[i].logical_count;
    }

    /* Remaining extents go to overflow tree */
    for(uint64_t i = 8; i < mc; i++)
    {
        struct overflow_extent oe;
        oe.inode_id       = inode->inode_id;
        oe.logical_offset = merged[i].logical_start;
        oe.start_block    = merged[i].phys_start;
        oe.block_count    = merged[i].phys_count;
        oe.logical_count  = merged[i].logical_count;
        rc                = overflow_insert(ctx, &oe);
        if(rc != OBMAFS3_OK)
        {
            free(merged);
            return rc;
        }
    }

    free(merged);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  File data writing                                                  */
/* ------------------------------------------------------------------ */

/**
 * Write file data to the filesystem.
 *
 * Data is organized into compression groups of
 * OBMAFS3_COMPRESS_GROUP_BLOCKS logical blocks (default 16 × 4 KiB =
 * 64 KiB).  Full compression groups are ZSTD-compressed as a unit
 * into variable-length physical extents.  Partial groups (at the tail
 * of a file) are stored uncompressed.  Individual blocks never carry
 * a block_header; only compressed extents do.
 *
 * @param ctx     Filesystem context.
 * @param inode   Inode record to update (modified in place).
 * @param offset  Byte offset within the file to start writing.
 * @param buf     Data buffer to write.
 * @param size    Number of bytes to write.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_write_file_data(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t offset, const void *buf,
                            size_t size)
{
    uint64_t block_size  = ctx->sb.block_size;
    uint64_t group_size  = OBMAFS3_COMPRESS_GROUP_BLOCKS;
    uint64_t group_bytes = block_size * group_size;
    int      rc;

    /* Update file size */
    uint64_t new_end = offset + size;
    if(new_end > inode->file_size) inode->file_size = new_end;

    uint64_t total_logical = (inode->file_size + block_size - 1) / block_size;

    /* Determine affected logical block range */
    uint64_t first_block = offset / block_size;
    uint64_t last_block  = (offset + size > 0) ? (offset + size - 1) / block_size : first_block;

    /* Determine affected compression groups */
    uint64_t first_group = first_block / group_size;
    uint64_t last_group  = last_block / group_size;

    /* Collect all existing extents */
    struct extent_descriptor *ext_list = NULL;
    uint64_t                  ext_count = 0;
    rc = collect_all_extents(ctx, inode, &ext_list, &ext_count);
    if(rc != OBMAFS3_OK) return rc;

    /* Pre-size the extent list for the new extents we'll add */
    uint64_t ext_cap = ext_count + (last_group - first_group + 1) + 16;
    {
        struct extent_descriptor *tmp = realloc(ext_list, (size_t)ext_cap * sizeof(*tmp));
        if(!tmp)
        {
            free(ext_list);
            return OBMAFS3_ERR_NOMEM;
        }
        ext_list = tmp;
    }

    /* Allocate a separate group assembly buffer (up to 64 KiB).
     * This must be separate from io_buf / io_buf2 which are used
     * internally by read_extent_blocks(). */
    uint8_t *group_data = malloc((size_t)group_bytes);
    if(!group_data)
    {
        free(ext_list);
        return OBMAFS3_ERR_NOMEM;
    }

    for(uint64_t g = first_group; g <= last_group; g++)
    {
        uint64_t grp_start = g * group_size;
        uint64_t grp_end   = (g + 1) * group_size;
        if(grp_end > total_logical) grp_end = total_logical;
        uint64_t grp_count = grp_end - grp_start;
        size_t   grp_bytes = (size_t)(grp_count * block_size);

        /* Zero-fill the group assembly buffer */
        memset(group_data, 0, grp_bytes);

        /* Read existing data from extents that overlap this group */
        for(uint64_t ei = 0; ei < ext_count; ei++)
        {
            struct extent_descriptor *e = &ext_list[ei];
            uint64_t                  e_end = e->logical_start + e->logical_count;

            /* Check for overlap with [grp_start, grp_end) */
            if(e_end <= grp_start || e->logical_start >= grp_end) continue;

            /* Compute overlap range */
            uint64_t overlap_start = (e->logical_start > grp_start) ? e->logical_start : grp_start;
            uint64_t overlap_end   = (e_end < grp_end) ? e_end : grp_end;
            uint64_t overlap_count = overlap_end - overlap_start;

            /* Read the overlapping blocks into group_data at the right offset */
            uint8_t *dest = group_data + (size_t)((overlap_start - grp_start) * block_size);
            rc            = read_extent_blocks(ctx, e, overlap_start, overlap_count, dest);
            if(rc != OBMAFS3_OK)
            {
                free(group_data);
                free(ext_list);
                return rc;
            }
        }

        /* Overlay new write data */
        uint64_t grp_byte_start = grp_start * block_size;
        uint64_t grp_byte_end   = grp_start * block_size + grp_bytes;
        uint64_t write_start    = (offset > grp_byte_start) ? offset : grp_byte_start;
        uint64_t write_end      = (offset + size < grp_byte_end) ? offset + size : grp_byte_end;
        if(write_end > write_start)
        {
            size_t dest_off = (size_t)(write_start - grp_byte_start);
            size_t src_off  = (size_t)(write_start - offset);
            size_t nbytes   = (size_t)(write_end - write_start);
            memcpy(group_data + dest_off, (const uint8_t *)buf + src_off, nbytes);
        }

        /* Remove overlapping extents from the list, freeing their physical blocks.
         * Uncompressed extents that straddle the group boundary are trimmed.
         * Compressed extents that straddle (shouldn't happen by design) are freed entirely. */
        for(uint64_t ei = 0; ei < ext_count;)
        {
            struct extent_descriptor *e = &ext_list[ei];
            uint64_t                  e_end = e->logical_start + e->logical_count;

            if(e_end <= grp_start || e->logical_start >= grp_end)
            {
                ei++;
                continue;
            }

            /* Extent entirely within the group */
            if(e->logical_start >= grp_start && e_end <= grp_end)
            {
                free_extent_phys(ctx, e);
                memmove(e, e + 1, (size_t)(ext_count - ei - 1) * sizeof(*e));
                ext_count--;
                continue;
            }

            /* Uncompressed extent straddling only the left boundary */
            if(e->logical_start < grp_start && e_end <= grp_end && e->logical_count == e->phys_count)
            {
                uint64_t keep = grp_start - e->logical_start;
                for(uint64_t b = keep; b < e->phys_count; b++) free_single_phys_block(ctx, e->phys_start + b);
                e->logical_count = keep;
                e->phys_count    = keep;
                ei++;
                continue;
            }

            /* Uncompressed extent straddling only the right boundary */
            if(e->logical_start >= grp_start && e_end > grp_end && e->logical_count == e->phys_count)
            {
                uint64_t skip = grp_end - e->logical_start;
                for(uint64_t b = 0; b < skip; b++) free_single_phys_block(ctx, e->phys_start + b);
                e->logical_start += skip;
                e->logical_count -= skip;
                e->phys_start += skip;
                e->phys_count -= skip;
                ei++;
                continue;
            }

            /* Uncompressed extent fully containing the group */
            if(e->logical_start < grp_start && e_end > grp_end && e->logical_count == e->phys_count)
            {
                uint64_t left_count    = grp_start - e->logical_start;
                uint64_t right_start_l = grp_end;
                uint64_t right_count   = e_end - grp_end;
                uint64_t right_phys    = e->phys_start + (grp_end - e->logical_start);

                for(uint64_t b = left_count; b < left_count + grp_count; b++)
                    free_single_phys_block(ctx, e->phys_start + b);

                e->logical_count = left_count;
                e->phys_count    = left_count;

                /* Insert the right residual part */
                if(ext_count >= ext_cap)
                {
                    ext_cap                       = ext_cap * 2 + 16;
                    struct extent_descriptor *tmp2 = realloc(ext_list, (size_t)ext_cap * sizeof(*tmp2));
                    if(!tmp2)
                    {
                        free(group_data);
                        free(ext_list);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    ext_list = tmp2;
                    e        = &ext_list[ei]; /* pointer may have moved */
                }
                memmove(&ext_list[ei + 2], &ext_list[ei + 1], (size_t)(ext_count - ei - 1) * sizeof(*ext_list));
                ext_list[ei + 1].logical_start = right_start_l;
                ext_list[ei + 1].logical_count = right_count;
                ext_list[ei + 1].phys_start    = right_phys;
                ext_list[ei + 1].phys_count    = right_count;
                ext_count++;
                ei += 2; /* skip the left and right residuals */
                continue;
            }

            /* Compressed extent overlapping (shouldn't cross group boundaries
             * by design, but handle gracefully by freeing entirely) */
            free_extent_phys(ctx, e);
            memmove(e, e + 1, (size_t)(ext_count - ei - 1) * sizeof(*e));
            ext_count--;
        }

        /* ---- Compress and write the group ---- */
        uint64_t phys_start    = 0;
        uint64_t phys_count    = 0;
        int      use_compressed = 0;

        /* Only attempt compression for full groups */
        if(ctx->compression && grp_count == group_size && grp_bytes > 0)
        {
            uint8_t *comp_out     = ctx->comp_buf;
            size_t   comp_cap     = ctx->comp_buf_size - sizeof(struct block_header);
            size_t   comp_size    = comp_cap;

            rc = obmafs3_compress(ctx->zstd_cctx, group_data, grp_bytes, comp_out + sizeof(struct block_header),
                                  &comp_size, ctx->zstd_level);
            if(rc == OBMAFS3_OK)
            {
                uint64_t total_on_disk = sizeof(struct block_header) + comp_size;
                uint64_t phys_needed   = (total_on_disk + block_size - 1) / block_size;
                if(phys_needed < grp_count)
                {
                    /* Compression saves space — use it */
                    use_compressed = 1;
                    rc             = obmafs3_alloc_blocks(ctx, phys_needed, &phys_start);
                    if(rc != OBMAFS3_OK)
                    {
                        free(group_data);
                        free(ext_list);
                        return rc;
                    }

                    /* Build block_header */
                    struct block_header bhdr;
                    memset(&bhdr, 0, sizeof(bhdr));
                    bhdr.magic            = OBMAFS3_BLOCK_MAGIC;
                    bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                    bhdr.compression_type = kCompressionZstd;
                    bhdr.original_size    = grp_bytes;
                    bhdr.compressed_size  = comp_size;
                    obmafs3_checksum_block(comp_out + sizeof(bhdr), comp_size, bhdr.checksum);
                    memcpy(comp_out, &bhdr, sizeof(bhdr));

                    /* Zero-pad the last physical block */
                    size_t used            = sizeof(bhdr) + comp_size;
                    size_t total_phys_bytes = (size_t)(phys_needed * block_size);
                    if(used < total_phys_bytes) memset(comp_out + used, 0, total_phys_bytes - used);

                    /* Write all physical blocks */
                    for(uint64_t b = 0; b < phys_needed; b++)
                    {
                        rc = obmafs3_block_write(ctx, phys_start + b, comp_out + b * (size_t)block_size,
                                                 (size_t)block_size);
                        if(rc != OBMAFS3_OK)
                        {
                            free(group_data);
                            free(ext_list);
                            return rc;
                        }
                    }
                    phys_count = phys_needed;
                }
            }
        }

        if(!use_compressed)
        {
            /* Write uncompressed: one physical block per logical block,
             * no block_header per block. */
            rc = obmafs3_alloc_blocks(ctx, grp_count, &phys_start);
            if(rc != OBMAFS3_OK)
            {
                free(group_data);
                free(ext_list);
                return rc;
            }

            for(uint64_t b = 0; b < grp_count; b++)
            {
                rc = obmafs3_block_write(ctx, phys_start + b, group_data + b * (size_t)block_size, (size_t)block_size);
                if(rc != OBMAFS3_OK)
                {
                    free(group_data);
                    free(ext_list);
                    return rc;
                }
            }
            phys_count = grp_count;
        }

        /* Add the new extent to the list (sorted insert) */
        if(ext_count >= ext_cap)
        {
            ext_cap                       = ext_cap * 2 + 16;
            struct extent_descriptor *tmp2 = realloc(ext_list, (size_t)ext_cap * sizeof(*tmp2));
            if(!tmp2)
            {
                free(group_data);
                free(ext_list);
                return OBMAFS3_ERR_NOMEM;
            }
            ext_list = tmp2;
        }

        uint64_t ins = 0;
        while(ins < ext_count && ext_list[ins].logical_start < grp_start) ins++;
        if(ins < ext_count)
            memmove(&ext_list[ins + 1], &ext_list[ins], (size_t)(ext_count - ins) * sizeof(*ext_list));
        ext_list[ins].logical_start = grp_start;
        ext_list[ins].logical_count = grp_count;
        ext_list[ins].phys_start    = phys_start;
        ext_list[ins].phys_count    = phys_count;
        ext_count++;
    }

    free(group_data);

    /* Write back the extent list to the inode + overflow tree */
    rc = write_extent_list(ctx, inode, ext_list, ext_count);
    free(ext_list);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Refcount-aware block freeing                                       */
/* ------------------------------------------------------------------ */

/**
 * Free all data blocks owned by an inode, respecting refcounts.
 *
 * For each physical block in the inode's inline extents and overflow
 * B+Tree entries, the block refcount is checked.  Shared blocks
 * (refcount > 1) have their refcount decremented; unshared blocks
 * are freed to the bitmap.  All inline extent slots and overflow
 * entries are cleared afterwards.
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode whose data blocks are freed (modified in place).
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_free_file_blocks(struct obmafs3_ctx *ctx, struct inode_record *inode)
{
    /* Free inline extent blocks */
    for(int i = 0; i < 8; i++)
    {
        for(uint64_t j = 0; j < inode->extents[i].block_count; j++)
            free_single_phys_block(ctx, inode->extents[i].start_block + j);
        inode->extents[i].start_block    = 0;
        inode->extents[i].block_count    = 0;
        inode->extents[i].logical_blocks = 0;
    }

    /* Free overflow extent blocks */
    struct extent_descriptor *ovf_exts  = NULL;
    uint64_t                  ovf_count = 0;
    int rc = overflow_collect_extents(ctx, inode->inode_id, &ovf_exts, &ovf_count);
    if(rc != OBMAFS3_OK) return rc;

    for(uint64_t i = 0; i < ovf_count; i++) free_extent_phys(ctx, &ovf_exts[i]);
    free(ovf_exts);

    if(ovf_count > 0) overflow_clear_inode(ctx, inode->inode_id);

    return OBMAFS3_OK;
}

/**
 * Truncate an inode's data blocks to @a new_block_count logical blocks.
 *
 * Blocks beyond @a new_block_count are freed (refcount-aware).  The
 * remaining extent map is rebuilt.  Compressed extents at the boundary
 * are decompressed and rewritten as uncompressed.
 *
 * @param ctx             Filesystem context.
 * @param inode           Inode record to update (modified in place).
 * @param new_block_count Number of logical data blocks to keep.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_truncate_file_blocks(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t new_block_count)
{
    struct extent_descriptor *list  = NULL;
    uint64_t                  count = 0;
    int rc = collect_all_extents(ctx, inode, &list, &count);
    if(rc != OBMAFS3_OK) return rc;

    /* Sum total logical blocks */
    uint64_t total_logical = 0;
    for(uint64_t i = 0; i < count; i++) total_logical += list[i].logical_count;

    if(new_block_count >= total_logical)
    {
        free(list);
        return OBMAFS3_OK; /* nothing to free */
    }

    /* Walk extents, trim at the truncation point */
    uint64_t keep_count = 0;
    uint64_t logical_pos = 0;

    for(uint64_t i = 0; i < count; i++)
    {
        uint64_t ext_end = logical_pos + list[i].logical_count;

        if(ext_end <= new_block_count)
        {
            /* Keep entirely */
            logical_pos = ext_end;
            keep_count  = i + 1;
            continue;
        }

        if(logical_pos >= new_block_count)
        {
            /* Free entirely */
            free_extent_phys(ctx, &list[i]);
            continue;
        }

        /* This extent straddles the truncation point */
        uint64_t keep_logical = new_block_count - logical_pos;

        if(list[i].logical_count == list[i].phys_count)
        {
            /* Uncompressed: simply trim the right side */
            for(uint64_t b = keep_logical; b < list[i].phys_count; b++)
                free_single_phys_block(ctx, list[i].phys_start + b);
            list[i].logical_count = keep_logical;
            list[i].phys_count    = keep_logical;
            keep_count            = i + 1;
        }
        else
        {
            /* Compressed extent at truncation boundary:
             * decompress, keep the first keep_logical blocks as uncompressed */
            uint8_t *temp = malloc((size_t)(keep_logical * ctx->sb.block_size));
            if(!temp)
            {
                free(list);
                return OBMAFS3_ERR_NOMEM;
            }

            rc = read_extent_blocks(ctx, &list[i], logical_pos, keep_logical, temp);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(list);
                return rc;
            }

            /* Free old compressed extent's physical blocks */
            free_extent_phys(ctx, &list[i]);

            /* Allocate new uncompressed blocks */
            uint64_t new_start;
            rc = obmafs3_alloc_blocks(ctx, keep_logical, &new_start);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(list);
                return rc;
            }

            for(uint64_t b = 0; b < keep_logical; b++)
            {
                rc = obmafs3_block_write(ctx, new_start + b, temp + b * (size_t)ctx->sb.block_size,
                                         (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK)
                {
                    free(temp);
                    free(list);
                    return rc;
                }
            }
            free(temp);

            list[i].phys_start    = new_start;
            list[i].phys_count    = keep_logical;
            list[i].logical_count = keep_logical;
            keep_count            = i + 1;
        }
        logical_pos = new_block_count;
    }

    rc = write_extent_list(ctx, inode, list, keep_count);
    free(list);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Clone / reflink file range                                         */
/* ------------------------------------------------------------------ */

/**
 * Clone a range of data blocks from one inode to another by sharing
 * physical blocks and incrementing their refcounts.
 *
 * Both offsets and length must be aligned to the filesystem block size.
 * Source blocks may reside in inline extents or the overflow B+Tree.
 * The destination's existing blocks in the target range are freed
 * (refcount-aware), and the resulting extent map is rebuilt.
 *
 * Compressed extents that are fully within the clone range are shared
 * as-is (all their physical blocks get a refcount bump).  Compressed
 * extents that only partially overlap the clone range are decompressed
 * and the relevant portion is written as a private uncompressed copy.
 *
 * @param ctx         Filesystem context.
 * @param src_inode   Source inode (read-only).
 * @param src_offset  Byte offset into the source file (block-aligned).
 * @param dst_inode   Destination inode (modified in place).
 * @param dst_offset  Byte offset into the destination file (block-aligned).
 * @param length      Number of bytes to clone (block-aligned).
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_clone_file_range(struct obmafs3_ctx *ctx, const struct inode_record *src_inode, uint64_t src_offset,
                             struct inode_record *dst_inode, uint64_t dst_offset, uint64_t length)
{
    uint64_t block_size = ctx->sb.block_size;
    int      rc;

    /* Validate alignment to block_size (no per-block header any more) */
    if(src_offset % block_size || dst_offset % block_size || length % block_size || length == 0)
        return OBMAFS3_ERR_INVAL;

    if(src_inode->inode_id == dst_inode->inode_id) return OBMAFS3_ERR_INVAL;

    uint64_t num_logical   = length / block_size;
    uint64_t src_log_start = src_offset / block_size;
    uint64_t dst_log_start = dst_offset / block_size;

    /* ---- Collect source extents ---- */
    struct extent_descriptor *src_exts     = NULL;
    uint64_t                  src_ext_count = 0;
    rc = collect_all_extents(ctx, src_inode, &src_exts, &src_ext_count);
    if(rc != OBMAFS3_OK) return rc;

    /* ---- Collect destination extents ---- */
    struct extent_descriptor *dst_exts     = NULL;
    uint64_t                  dst_ext_count = 0;
    rc = collect_all_extents(ctx, dst_inode, &dst_exts, &dst_ext_count);
    if(rc != OBMAFS3_OK)
    {
        free(src_exts);
        return rc;
    }

    /* ---- Build cloned extents from source ---- */
    uint64_t                  clone_cap   = 64;
    struct extent_descriptor *clone_exts  = calloc((size_t)clone_cap, sizeof(*clone_exts));
    uint64_t                  clone_count = 0;
    if(!clone_exts)
    {
        free(src_exts);
        free(dst_exts);
        return OBMAFS3_ERR_NOMEM;
    }

    uint64_t src_range_end = src_log_start + num_logical;

    for(uint64_t i = 0; i < src_ext_count; i++)
    {
        struct extent_descriptor *se = &src_exts[i];
        uint64_t                  se_end = se->logical_start + se->logical_count;

        if(se_end <= src_log_start || se->logical_start >= src_range_end) continue;

        uint64_t overlap_start = (se->logical_start > src_log_start) ? se->logical_start : src_log_start;
        uint64_t overlap_end   = (se_end < src_range_end) ? se_end : src_range_end;
        uint64_t overlap_count = overlap_end - overlap_start;

        /* Remap to destination logical space */
        uint64_t dst_log = dst_log_start + (overlap_start - src_log_start);

        if(clone_count >= clone_cap)
        {
            clone_cap                     = clone_cap * 2;
            struct extent_descriptor *tmp = realloc(clone_exts, (size_t)clone_cap * sizeof(*tmp));
            if(!tmp)
            {
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                return OBMAFS3_ERR_NOMEM;
            }
            clone_exts = tmp;
        }

        if(se->logical_count == se->phys_count)
        {
            /* Uncompressed: share the overlapping physical blocks */
            uint64_t phys_off = overlap_start - se->logical_start;
            clone_exts[clone_count].logical_start = dst_log;
            clone_exts[clone_count].logical_count = overlap_count;
            clone_exts[clone_count].phys_start    = se->phys_start + phys_off;
            clone_exts[clone_count].phys_count    = overlap_count;

            for(uint64_t b = 0; b < overlap_count; b++)
            {
                rc = obmafs3_refcount_inc(ctx, se->phys_start + phys_off + b);
                if(rc != OBMAFS3_OK)
                {
                    free(clone_exts);
                    free(src_exts);
                    free(dst_exts);
                    return rc;
                }
            }
            clone_count++;
        }
        else if(overlap_start == se->logical_start && overlap_count == se->logical_count)
        {
            /* Compressed extent fully within range: share it whole */
            clone_exts[clone_count].logical_start = dst_log;
            clone_exts[clone_count].logical_count = se->logical_count;
            clone_exts[clone_count].phys_start    = se->phys_start;
            clone_exts[clone_count].phys_count    = se->phys_count;

            for(uint64_t b = 0; b < se->phys_count; b++)
            {
                rc = obmafs3_refcount_inc(ctx, se->phys_start + b);
                if(rc != OBMAFS3_OK)
                {
                    free(clone_exts);
                    free(src_exts);
                    free(dst_exts);
                    return rc;
                }
            }
            clone_count++;
        }
        else
        {
            /* Partial compressed extent: decompress and write
             * private uncompressed copy of the overlapping portion */
            uint8_t *temp = malloc((size_t)(overlap_count * block_size));
            if(!temp)
            {
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                return OBMAFS3_ERR_NOMEM;
            }

            rc = read_extent_blocks(ctx, se, overlap_start, overlap_count, temp);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                return rc;
            }

            uint64_t new_start;
            rc = obmafs3_alloc_blocks(ctx, overlap_count, &new_start);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                return rc;
            }

            for(uint64_t b = 0; b < overlap_count; b++)
            {
                rc = obmafs3_block_write(ctx, new_start + b, temp + b * (size_t)block_size, (size_t)block_size);
                if(rc != OBMAFS3_OK)
                {
                    free(temp);
                    free(clone_exts);
                    free(src_exts);
                    free(dst_exts);
                    return rc;
                }
            }
            free(temp);

            clone_exts[clone_count].logical_start = dst_log;
            clone_exts[clone_count].logical_count = overlap_count;
            clone_exts[clone_count].phys_start    = new_start;
            clone_exts[clone_count].phys_count    = overlap_count;
            clone_count++;
        }
    }
    free(src_exts);

    /* ---- Free destination blocks in the clone range ---- */
    uint64_t dst_range_end = dst_log_start + num_logical;
    for(uint64_t i = 0; i < dst_ext_count; i++)
    {
        struct extent_descriptor *de = &dst_exts[i];
        uint64_t                  de_end = de->logical_start + de->logical_count;

        if(de_end <= dst_log_start || de->logical_start >= dst_range_end) continue;

        if(de->logical_count == de->phys_count)
        {
            /* Uncompressed: free only the overlapping physical blocks */
            uint64_t overlap_start = (de->logical_start > dst_log_start) ? de->logical_start : dst_log_start;
            uint64_t overlap_end   = (de_end < dst_range_end) ? de_end : dst_range_end;
            uint64_t phys_off      = overlap_start - de->logical_start;

            for(uint64_t b = 0; b < overlap_end - overlap_start; b++)
                free_single_phys_block(ctx, de->phys_start + phys_off + b);
        }
        else
        {
            /* Compressed dest extent overlapping the clone range: free entirely */
            free_extent_phys(ctx, de);
        }
    }

    /* ---- Build new destination extent list ---- */
    uint64_t                  new_cap  = dst_ext_count + clone_count + 16;
    struct extent_descriptor *new_list = calloc((size_t)new_cap, sizeof(*new_list));
    if(!new_list)
    {
        free(clone_exts);
        free(dst_exts);
        return OBMAFS3_ERR_NOMEM;
    }
    uint64_t new_count = 0;

    /* Add surviving destination extents before the clone range */
    for(uint64_t i = 0; i < dst_ext_count; i++)
    {
        struct extent_descriptor *de = &dst_exts[i];
        uint64_t                  de_end = de->logical_start + de->logical_count;

        if(de_end <= dst_log_start)
        {
            new_list[new_count++] = *de;
            continue;
        }

        /* Keep the left part of an uncompressed extent that straddles the start */
        if(de->logical_start < dst_log_start && de->logical_count == de->phys_count)
        {
            uint64_t keep                     = dst_log_start - de->logical_start;
            new_list[new_count]               = *de;
            new_list[new_count].logical_count = keep;
            new_list[new_count].phys_count    = keep;
            new_count++;
        }
    }

    /* Add cloned extents */
    for(uint64_t i = 0; i < clone_count; i++) new_list[new_count++] = clone_exts[i];

    /* Add surviving destination extents after the clone range */
    for(uint64_t i = 0; i < dst_ext_count; i++)
    {
        struct extent_descriptor *de = &dst_exts[i];
        uint64_t                  de_end = de->logical_start + de->logical_count;

        if(de->logical_start >= dst_range_end)
        {
            new_list[new_count++] = *de;
            continue;
        }

        /* Keep the right part of an uncompressed extent that straddles the end */
        if(de_end > dst_range_end && de->logical_count == de->phys_count)
        {
            uint64_t skip                         = dst_range_end - de->logical_start;
            new_list[new_count].logical_start = dst_range_end;
            new_list[new_count].logical_count = de->logical_count - skip;
            new_list[new_count].phys_start    = de->phys_start + skip;
            new_list[new_count].phys_count    = de->phys_count - skip;
            new_count++;
        }
    }

    free(clone_exts);
    free(dst_exts);

    rc = write_extent_list(ctx, dst_inode, new_list, new_count);
    free(new_list);
    if(rc != OBMAFS3_OK) return rc;

    uint64_t new_end = dst_offset + length;
    if(new_end > dst_inode->file_size) dst_inode->file_size = new_end;

    return OBMAFS3_OK;
}
