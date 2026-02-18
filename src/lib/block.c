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
 * @param src       Source data buffer.
 * @param src_size  Number of bytes in @p src.
 * @param dst       Destination buffer for compressed data.
 * @param dst_size  On input, capacity of @p dst; on output, compressed size.
 * @param level     ZSTD compression level (1–22).
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on error.
 */
int obmafs3_compress(const void *src, size_t src_size, void *dst, size_t *dst_size, int level)
{
    size_t result = ZSTD_compress(dst, *dst_size, src, src_size, level);
    if(ZSTD_isError(result)) return OBMAFS3_ERR_IO;
    *dst_size = result;
    return OBMAFS3_OK;
}

/**
 * Decompress ZSTD-compressed data.
 *
 * @param src       Compressed data buffer.
 * @param src_size  Number of compressed bytes.
 * @param dst       Output buffer for decompressed data.
 * @param dst_size  Capacity of @p dst (must be >= original size).
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on error.
 */
int obmafs3_decompress(const void *src, size_t src_size, void *dst, size_t dst_size)
{
    size_t result = ZSTD_decompress(dst, dst_size, src, src_size);
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
 * Binary search for (inode_id, start_block) in an overflow leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int overflow_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, uint64_t start_block)
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
        else if(oe.start_block < start_block) { lo = mid + 1; }
        else if(oe.start_block > start_block) { hi = mid - 1; }
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

    int      idx        = overflow_leaf_find(buf, leaf_hdr.node_keys, entry->inode_id, entry->start_block);
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

/**
 * Search the overflow B+Tree for extents belonging to the given inode
 * and map a logical block number to a physical LBA.
 *
 * Inline extents cover logical blocks 0..inline_block_count-1.
 * Overflow extents continue from there: the first overflow extent
 * covers logical blocks starting at inline_block_count.
 *
 * Returns 1 if found (phys_lba set), 0 if not found.
 */
static int overflow_find_phys(struct obmafs3_ctx *ctx, uint64_t inode_id, uint64_t logical_block,
                              uint64_t inline_block_count, uint64_t *phys_lba)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return 0;

    uint8_t *buf = ctx->node_buf;

    /* Traverse index levels to reach the leaf */
    uint64_t lba = hdr->root_node_lba;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return 0;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return 0;

        if(nhdr.level == 0) break; /* reached leaf */

        uint16_t                 slot = overflow_index_find(buf, nhdr.node_keys, inode_id);
        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan the leaf (and follow right_link for entries that span leaves) */
    uint64_t ovf_block_count = inline_block_count;

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
            memcpy(&oe, entries + i * sizeof(struct overflow_extent), sizeof(oe));

            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }

            if(logical_block >= ovf_block_count && logical_block < ovf_block_count + oe.block_count)
            {
                *phys_lba = oe.start_block + (logical_block - ovf_block_count);
                return 1;
            }
            ovf_block_count += oe.block_count;
        }

        if(past) break;
        lba = nhdr.right_link;
    }

    return 0;
}

/**
 * Count the total number of blocks stored in overflow extents for an inode.
 */
static uint64_t overflow_count_blocks(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return 0;

    uint8_t *buf = ctx->node_buf;

    /* Traverse index levels to reach the leaf */
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

    /* Scan the leaf (and follow right_link for entries that span leaves) */
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
            memcpy(&oe, entries + i * sizeof(struct overflow_extent), sizeof(oe));

            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }
            total += oe.block_count;
        }

        if(past) break;
        lba = nhdr.right_link;
    }

    return total;
}

/* ------------------------------------------------------------------ */
/*  File data reading                                                  */
/* ------------------------------------------------------------------ */

/**
 * Read file data from the filesystem.
 *
 * Resolves logical block offsets through the inode's inline extents and
 * overflow extent tree, handling block-level ZSTD decompression
 * transparently.
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
    uint64_t block_size    = ctx->sb.block_size;
    size_t   data_capacity = (size_t)block_size - sizeof(struct block_header);
    size_t   bytes_read    = 0;
    uint8_t *block_buf;
    uint8_t *decomp_buf;

    if(offset >= inode->file_size) return OBMAFS3_OK;

    if(offset + size > inode->file_size) size = (size_t)(inode->file_size - offset);

    block_buf  = ctx->io_buf;
    decomp_buf = ctx->io_buf2;

    while(bytes_read < size)
    {
        uint64_t read_pos        = offset + bytes_read;
        uint64_t logical_block   = read_pos / data_capacity;
        size_t   offset_in_block = (size_t)(read_pos % data_capacity);

        /* Map logical block to physical LBA via extents */
        uint64_t phys_lba           = 0;
        uint64_t block_count_so_far = 0;
        int      found              = 0;
        int      i;
        for(i = 0; i < 8; i++)
        {
            if(inode->extents[i].block_count == 0) continue;
            if(logical_block < block_count_so_far + inode->extents[i].block_count)
            {
                phys_lba = inode->extents[i].start_block + (logical_block - block_count_so_far);
                found    = 1;
                break;
            }
            block_count_so_far += inode->extents[i].block_count;
        }

        /* If not found in inline extents, check overflow tree */
        if(!found) { found = overflow_find_phys(ctx, inode->inode_id, logical_block, block_count_so_far, &phys_lba); }

        if(!found)
        {
            return OBMAFS3_ERR_IO;
        }

        int rc = obmafs3_block_read(ctx, phys_lba, block_buf, (size_t)block_size);
        if(rc != OBMAFS3_OK)
        {
            return rc;
        }

        struct block_header bhdr;
        memcpy(&bhdr, block_buf, sizeof(bhdr));

        uint8_t *data_ptr;
        size_t   data_len;

        if(bhdr.magic != OBMAFS3_BLOCK_MAGIC)
        {
            /* Raw data block (no header) — shouldn't happen with
             * the current write path but handle gracefully */
            data_ptr = block_buf;
            data_len = (size_t)block_size;
        }
        else
        {
            /* Verify checksum over on-disk data after header */
            size_t  check_size = (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? (size_t)bhdr.compressed_size
                                                                              : (size_t)bhdr.original_size;
            uint8_t computed[32];
            obmafs3_checksum_block(block_buf + sizeof(bhdr), check_size, computed);
            if(memcmp(computed, bhdr.checksum, 32) != 0)
            {
                return OBMAFS3_ERR_CHECKSUM;
            }

            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                rc = obmafs3_decompress(block_buf + sizeof(bhdr), (size_t)bhdr.compressed_size, decomp_buf,
                                        (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK)
                {
                    return rc;
                }
                data_ptr = decomp_buf;
                data_len = (size_t)bhdr.original_size;
            }
            else
            {
                data_ptr = block_buf + sizeof(bhdr);
                data_len = (size_t)bhdr.original_size;
            }
        }

        /* Copy data from the block at the correct offset */
        size_t avail   = data_len > offset_in_block ? data_len - offset_in_block : 0;
        size_t to_copy = (size - bytes_read < avail) ? size - bytes_read : avail;
        if(to_copy == 0) break;

        memcpy((uint8_t *)buf + bytes_read, data_ptr + offset_in_block, to_copy);
        bytes_read += to_copy;
    }

    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Copy-on-Write helpers                                              */
/* ------------------------------------------------------------------ */

/**
 * Compact and merge adjacent inline extents that are physically contiguous.
 *
 * Removes gaps (zero-count entries between used ones) and merges
 * neighbouring extents whose physical LBAs are consecutive.
 * This reclaims inline extent slots after CoW splits.
 *
 * @param inode  Inode whose inline extents are coalesced in place.
 */
static void coalesce_inline_extents(struct inode_record *inode)
{
    /* Compact: move all non-empty entries to the front */
    int wp = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count > 0)
        {
            if(wp != i) inode->extents[wp] = inode->extents[i];
            wp++;
        }
    }
    for(int i = wp; i < 8; i++)
    {
        inode->extents[i].start_block = 0;
        inode->extents[i].block_count = 0;
    }

    /* Merge contiguous */
    for(int i = 0; i < 7; i++)
    {
        if(inode->extents[i].block_count == 0) break;
        if(inode->extents[i + 1].block_count == 0) break;
        if(inode->extents[i].start_block + inode->extents[i].block_count == inode->extents[i + 1].start_block)
        {
            inode->extents[i].block_count += inode->extents[i + 1].block_count;
            for(int j = i + 1; j < 7; j++) inode->extents[j] = inode->extents[j + 1];
            inode->extents[7].start_block = 0;
            inode->extents[7].block_count = 0;
            i--; /* recheck merged entry */
        }
    }
}

/**
 * Replace one physical block inside an inline extent with a new LBA.
 *
 * Finds the inline extent containing @p old_phys (after coalescing),
 * splits it so that @p old_phys is replaced by @p new_phys, and shifts
 * any subsequent extents to make room.
 *
 * @param inode     Inode record (modified in place).
 * @param old_phys  Physical LBA that must be replaced.
 * @param new_phys  Replacement physical LBA.
 * @return @c OBMAFS3_OK, @c OBMAFS3_ERR_NOTFOUND, or
 *         @c OBMAFS3_ERR_NOSPC if not enough inline slots.
 */
static int cow_replace_block(struct inode_record *inode, uint64_t old_phys, uint64_t new_phys)
{
    coalesce_inline_extents(inode);

    /* Locate the extent containing old_phys */
    int      ext_idx = -1;
    uint64_t offset  = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count == 0) continue;
        if(old_phys >= inode->extents[i].start_block &&
           old_phys < inode->extents[i].start_block + inode->extents[i].block_count)
        {
            ext_idx = i;
            offset  = old_phys - inode->extents[i].start_block;
            break;
        }
    }
    if(ext_idx < 0) return OBMAFS3_ERR_NOTFOUND;

    uint64_t old_start = inode->extents[ext_idx].start_block;
    uint64_t old_count = inode->extents[ext_idx].block_count;

    /* Single-block extent: just replace the LBA */
    if(old_count == 1)
    {
        inode->extents[ext_idx].start_block = new_phys;
        return OBMAFS3_OK;
    }

    /* Find last used inline slot */
    int last_used = -1;
    for(int j = 7; j >= 0; j--)
    {
        if(inode->extents[j].block_count > 0)
        {
            last_used = j;
            break;
        }
    }

    int extra = (offset == 0 || offset == old_count - 1) ? 1 : 2;
    if(last_used + extra > 7) return OBMAFS3_ERR_NOSPC;

    if(offset == 0)
    {
        /* First block — need 1 extra slot */
        for(int j = last_used; j > ext_idx; j--) inode->extents[j + 1] = inode->extents[j];
        inode->extents[ext_idx].start_block     = new_phys;
        inode->extents[ext_idx].block_count     = 1;
        inode->extents[ext_idx + 1].start_block = old_start + 1;
        inode->extents[ext_idx + 1].block_count = old_count - 1;
        return OBMAFS3_OK;
    }

    if(offset == old_count - 1)
    {
        /* Last block — need 1 extra slot */
        for(int j = last_used; j > ext_idx; j--) inode->extents[j + 1] = inode->extents[j];
        inode->extents[ext_idx].block_count     = old_count - 1;
        inode->extents[ext_idx + 1].start_block = new_phys;
        inode->extents[ext_idx + 1].block_count = 1;
        return OBMAFS3_OK;
    }

    /* Middle block — need 2 extra slots */
    uint64_t right_start = old_start + offset + 1;
    uint64_t right_count = old_count - offset - 1;

    for(int j = last_used; j > ext_idx; j--) inode->extents[j + 2] = inode->extents[j];

    inode->extents[ext_idx].block_count     = offset;
    inode->extents[ext_idx + 1].start_block = new_phys;
    inode->extents[ext_idx + 1].block_count = 1;
    inode->extents[ext_idx + 2].start_block = right_start;
    inode->extents[ext_idx + 2].block_count = right_count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Overflow helpers for clone / CoW / truncate                        */
/* ------------------------------------------------------------------ */

/**
 * Collect every physical LBA stored in the overflow B+Tree for
 * @a inode_id.  Each extent is expanded into individual LBAs in the
 * order they appear in the tree (ascending physical start_block),
 * which mirrors the logical ordering used by overflow_find_phys().
 *
 * The caller must free @c *out_lbas when done.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode whose overflow blocks are collected.
 * @param out_lbas  Receives a malloc'd array of physical LBAs.
 * @param out_count Receives the number of elements in @c *out_lbas.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int overflow_collect_lbas(struct obmafs3_ctx *ctx, uint64_t inode_id, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    uint64_t total = overflow_count_blocks(ctx, inode_id);
    if(total == 0) return OBMAFS3_OK;

    uint64_t *lbas = calloc((size_t)total, sizeof(uint64_t));
    if(!lbas) return OBMAFS3_ERR_NOMEM;

    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0)
    {
        free(lbas);
        return OBMAFS3_OK;
    }

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf)
    {
        free(lbas);
        return OBMAFS3_ERR_NOMEM;
    }

    /* Navigate index nodes to reach the first relevant leaf */
    uint64_t lba = hdr->root_node_lba;
    for(;;)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(lbas);
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

    /* Scan leaves, expanding each extent into individual LBAs */
    uint64_t idx = 0;
    while(lba != 0 && idx < total)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;
        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            past    = 0;
        for(uint16_t i = 0; i < nhdr.node_keys && idx < total; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));
            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }
            for(uint64_t j = 0; j < oe.block_count && idx < total; j++) lbas[idx++] = oe.start_block + j;
        }
        if(past) break;
        lba = nhdr.right_link;
    }

    free(buf);
    *out_lbas  = lbas;
    *out_count = idx;
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
 * Rebuild an inode's inline extents and overflow entries from a flat
 * array of physical LBAs (in logical order).  Contiguous LBAs are
 * coalesced into extent runs; the first 8 runs are stored in the
 * inline extent slots, and any remaining runs are inserted into the
 * overflow B+Tree via overflow_insert().
 *
 * The caller must have already cleared old overflow entries for this
 * inode (via overflow_clear_inode()) before calling this function.
 *
 * @param ctx          Filesystem context.
 * @param inode        Inode record to update (modified in place).
 * @param lbas         Array of physical LBAs in logical order.
 * @param total_blocks Number of elements in @a lbas.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int rebuild_extent_map(struct obmafs3_ctx *ctx, struct inode_record *inode, const uint64_t *lbas,
                              uint64_t total_blocks)
{
    memset(inode->extents, 0, sizeof(inode->extents));
    if(total_blocks == 0) return OBMAFS3_OK;

    /* Count extent runs */
    uint64_t run_count = 1;
    for(uint64_t i = 1; i < total_blocks; i++)
    {
        if(lbas[i] != lbas[i - 1] + 1) run_count++;
    }

    struct extent_run *runs = calloc((size_t)run_count, sizeof(*runs));
    if(!runs) return OBMAFS3_ERR_NOMEM;

    uint64_t ri         = 0;
    runs[0].start_block = lbas[0];
    runs[0].block_count = 1;
    for(uint64_t i = 1; i < total_blocks; i++)
    {
        if(lbas[i] == runs[ri].start_block + runs[ri].block_count) { runs[ri].block_count++; }
        else
        {
            ri++;
            runs[ri].start_block = lbas[i];
            runs[ri].block_count = 1;
        }
    }

    /* First 8 runs go to inline extent slots */
    uint64_t inline_runs = (run_count <= 8) ? run_count : 8;
    for(uint64_t i = 0; i < inline_runs; i++) inode->extents[i] = runs[i];

    /* Remaining runs go to the overflow B+Tree */
    int rc = OBMAFS3_OK;
    for(uint64_t i = 8; i < run_count; i++)
    {
        struct overflow_extent oe;
        oe.inode_id    = inode->inode_id;
        oe.start_block = runs[i].start_block;
        oe.block_count = runs[i].block_count;
        rc             = overflow_insert(ctx, &oe);
        if(rc != OBMAFS3_OK) break;
    }

    free(runs);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  File data writing                                                  */
/* ------------------------------------------------------------------ */

/**
 * Write file data to the filesystem.
 *
 * Allocates blocks as needed (extending inline extents and the overflow
 * extent tree), writes data with optional ZSTD compression, and updates
 * the inode's file size.  When writing to a block whose refcount is
 * greater than 1 (shared via clone), a copy-on-write is performed:
 * a new block is allocated, the old refcount is decremented, and the
 * inode's extent map is updated to point to the private copy.
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
    uint64_t block_size    = ctx->sb.block_size;
    size_t   data_capacity = (size_t)block_size - sizeof(struct block_header);
    size_t   bytes_written = 0;
    int      rc;

    /* Calculate the total number of data blocks needed for the file
     * after this write */
    uint64_t new_end = offset + size;
    if(new_end > inode->file_size) inode->file_size = new_end;

    uint64_t total_blocks_needed = (inode->file_size + data_capacity - 1) / data_capacity;

    /* Count existing allocated blocks (inline + overflow) */
    uint64_t existing_blocks = 0;
    int      i;
    for(i = 0; i < 8; i++) existing_blocks += inode->extents[i].block_count;
    existing_blocks += overflow_count_blocks(ctx, inode->inode_id);

    /* Allocate additional blocks if needed */
    if(total_blocks_needed > existing_blocks)
    {
        uint64_t new_blocks = total_blocks_needed - existing_blocks;
        uint64_t new_start;
        rc = obmafs3_alloc_blocks(ctx, new_blocks, &new_start);
        if(rc != OBMAFS3_OK) return rc;

        /* Initialize new blocks with empty block headers */
        uint8_t *zero_block = ctx->io_buf;
        memset(zero_block, 0, (size_t)block_size);
        struct block_header empty_hdr;
        memset(&empty_hdr, 0, sizeof(empty_hdr));
        empty_hdr.magic           = OBMAFS3_BLOCK_MAGIC;
        empty_hdr.flags           = 0;
        empty_hdr.original_size   = 0;
        empty_hdr.compressed_size = 0;
        /* Checksum of zero-length data */
        obmafs3_checksum_block(zero_block + sizeof(empty_hdr), 0, empty_hdr.checksum);
        memcpy(zero_block, &empty_hdr, sizeof(empty_hdr));
        for(uint64_t b = 0; b < new_blocks; b++)
        {
            rc = obmafs3_block_write(ctx, new_start + b, zero_block, (size_t)block_size);
            if(rc != OBMAFS3_OK)
            {
                return rc;
            }
        }

        /* Add the new extent to the inode */
        int added = 0;
        for(i = 0; i < 8; i++)
        {
            if(inode->extents[i].block_count == 0)
            {
                inode->extents[i].start_block = new_start;
                inode->extents[i].block_count = new_blocks;
                added                         = 1;
                break;
            }
            /* Try to extend an adjacent extent */
            if(inode->extents[i].start_block + inode->extents[i].block_count == new_start)
            {
                inode->extents[i].block_count += new_blocks;
                added = 1;
                break;
            }
        }
        if(!added)
        {
            /* Inline extents full — store in overflow tree */
            struct overflow_extent oe;
            oe.inode_id    = inode->inode_id;
            oe.start_block = new_start;
            oe.block_count = new_blocks;
            rc             = overflow_insert(ctx, &oe);
            if(rc != OBMAFS3_OK) return rc;
        }
    }

    /* Now write the data into the appropriate blocks */
    uint8_t *block_buf = ctx->io_buf;
    uint8_t *work_buf  = ctx->io_buf2;

    while(bytes_written < size)
    {
        /* Determine which logical data block this offset falls into */
        uint64_t write_pos       = offset + bytes_written;
        uint64_t logical_block   = write_pos / data_capacity;
        size_t   offset_in_block = (size_t)(write_pos % data_capacity);

        /* Map logical block to physical LBA via extents */
        uint64_t phys_lba           = 0;
        uint64_t block_count_so_far = 0;
        int      found              = 0;
        for(i = 0; i < 8; i++)
        {
            if(inode->extents[i].block_count == 0) continue;
            if(logical_block < block_count_so_far + inode->extents[i].block_count)
            {
                phys_lba = inode->extents[i].start_block + (logical_block - block_count_so_far);
                found    = 1;
                break;
            }
            block_count_so_far += inode->extents[i].block_count;
        }

        /* If not found in inline extents, check overflow tree */
        if(!found) { found = overflow_find_phys(ctx, inode->inode_id, logical_block, block_count_so_far, &phys_lba); }

        if(!found)
        {
            return OBMAFS3_ERR_IO;
        }

        /* Copy-on-Write: if this block is shared (refcount > 1),
         * allocate a private copy.  The old data is still read from
         * phys_lba; the modified data is written to write_lba. */
        uint64_t write_lba = phys_lba;
        if(found && ctx->refcount_hdr.root_node_lba != 0)
        {
            uint32_t ref;
            rc = obmafs3_refcount_get(ctx, phys_lba, &ref);
            if(rc == OBMAFS3_OK && ref > 1)
            {
                uint64_t new_lba;
                rc = obmafs3_alloc_block(ctx, &new_lba);
                if(rc != OBMAFS3_OK)
                {
                    return rc;
                }
                if(i < 8)
                {
                    /* Inline extent — split in place */
                    rc = cow_replace_block(inode, phys_lba, new_lba);
                    if(rc != OBMAFS3_OK)
                    {
                        obmafs3_free_block(ctx, new_lba);
                        return rc;
                    }
                }
                else
                {
                    /* Overflow extent — flatten, mutate, rebuild */
                    uint64_t inl_count = 0;
                    for(int ii = 0; ii < 8; ii++) inl_count += inode->extents[ii].block_count;

                    uint64_t *ovf_lbas  = NULL;
                    uint64_t  ovf_count = 0;
                    rc                  = overflow_collect_lbas(ctx, inode->inode_id, &ovf_lbas, &ovf_count);
                    if(rc != OBMAFS3_OK)
                    {
                        obmafs3_free_block(ctx, new_lba);
                        return rc;
                    }

                    int replaced = 0;
                    for(uint64_t oi = 0; oi < ovf_count; oi++)
                    {
                        if(ovf_lbas[oi] == phys_lba)
                        {
                            ovf_lbas[oi] = new_lba;
                            replaced     = 1;
                            break;
                        }
                    }

                    if(!replaced)
                    {
                        free(ovf_lbas);
                        obmafs3_free_block(ctx, new_lba);
                        return OBMAFS3_ERR_IO;
                    }

                    rc = overflow_clear_inode(ctx, inode->inode_id);
                    if(rc != OBMAFS3_OK)
                    {
                        free(ovf_lbas);
                        return rc;
                    }

                    uint64_t  total    = inl_count + ovf_count;
                    uint64_t *all_lbas = calloc((size_t)total, sizeof(uint64_t));
                    if(!all_lbas)
                    {
                        free(ovf_lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }

                    uint64_t ai = 0;
                    for(int ii = 0; ii < 8; ii++)
                        for(uint64_t jj = 0; jj < inode->extents[ii].block_count; jj++)
                            all_lbas[ai++] = inode->extents[ii].start_block + jj;
                    for(uint64_t oi = 0; oi < ovf_count; oi++) all_lbas[ai++] = ovf_lbas[oi];
                    free(ovf_lbas);

                    rc = rebuild_extent_map(ctx, inode, all_lbas, total);
                    free(all_lbas);
                    if(rc != OBMAFS3_OK)
                    {
                        return rc;
                    }
                }
                obmafs3_refcount_dec(ctx, phys_lba, NULL);
                write_lba = new_lba;
            }
        }

        /* Read the existing block */
        rc = obmafs3_block_read(ctx, phys_lba, block_buf, (size_t)block_size);
        if(rc != OBMAFS3_OK)
        {
            return rc;
        }

        /* Decompress existing block data into work_buf so we can
         * safely modify it regardless of the on-disk format. */
        struct block_header existing_hdr;
        memcpy(&existing_hdr, block_buf, sizeof(existing_hdr));

        memset(work_buf, 0, data_capacity);
        if(existing_hdr.magic == OBMAFS3_BLOCK_MAGIC && existing_hdr.original_size > 0)
        {
            if(existing_hdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                rc = obmafs3_decompress(block_buf + sizeof(struct block_header), (size_t)existing_hdr.compressed_size,
                                        work_buf, (size_t)existing_hdr.original_size);
                if(rc != OBMAFS3_OK)
                {
                    return rc;
                }
            }
            else
            {
                memcpy(work_buf, block_buf + sizeof(struct block_header), (size_t)existing_hdr.original_size);
            }
        }

        /* Determine how much to write into this block */
        size_t space    = data_capacity - offset_in_block;
        size_t to_write = (size - bytes_written < space) ? size - bytes_written : space;

        /* Write new data into the decompressed work buffer */
        memcpy(work_buf + offset_in_block, (const uint8_t *)buf + bytes_written, to_write);

        /* Compute the data extent in this block */
        struct block_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        bhdr.magic              = OBMAFS3_BLOCK_MAGIC;
        uint64_t block_data_end = offset_in_block + to_write;
        if(existing_hdr.magic == OBMAFS3_BLOCK_MAGIC && existing_hdr.original_size > block_data_end)
            block_data_end = existing_hdr.original_size;
        bhdr.original_size = block_data_end;

        /* Try to compress if enabled */
        uint8_t *out_buf   = block_buf;
        int      compressed = 0;
        if(ctx->compression && block_data_end > 0)
        {
            uint8_t *comp_block = ctx->comp_buf;
            size_t   comp_size  = ctx->comp_buf_size - sizeof(struct block_header);
            int crc = obmafs3_compress(work_buf, (size_t)block_data_end, comp_block + sizeof(struct block_header),
                                       &comp_size, ctx->zstd_level);
            if(crc == OBMAFS3_OK && comp_size < block_data_end)
            {
                /* Compression saved space — use compressed block */
                compressed            = 1;
                bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                bhdr.compression_type = kCompressionZstd;
                bhdr.compressed_size  = comp_size;
                obmafs3_checksum_block(comp_block + sizeof(struct block_header), comp_size, bhdr.checksum);
                memcpy(comp_block, &bhdr, sizeof(bhdr));
                /* Zero-fill remainder of the block */
                size_t used = sizeof(struct block_header) + comp_size;
                if(used < (size_t)block_size) memset(comp_block + used, 0, (size_t)block_size - used);
                out_buf = comp_block;
            }
        }

        if(!compressed)
        {
            /* Store uncompressed — copy work_buf into block_buf */
            memcpy(block_buf + sizeof(struct block_header), work_buf, (size_t)block_data_end);
            bhdr.flags           = 0;
            bhdr.compressed_size = block_data_end;
            obmafs3_checksum_block(block_buf + sizeof(struct block_header), (size_t)block_data_end, bhdr.checksum);
            memcpy(block_buf, &bhdr, sizeof(bhdr));
        }

        rc = obmafs3_block_write(ctx, write_lba, out_buf, (size_t)block_size);
        if(rc != OBMAFS3_OK)
        {
            return rc;
        }

        bytes_written += to_write;
    }

    return OBMAFS3_OK;
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
        if(inode->extents[i].block_count == 0) continue;
        for(uint64_t j = 0; j < inode->extents[i].block_count; j++)
        {
            uint64_t lba = inode->extents[i].start_block + j;
            uint32_t ref = 1;
            obmafs3_refcount_get(ctx, lba, &ref);
            if(ref > 1)
                obmafs3_refcount_dec(ctx, lba, NULL);
            else
                obmafs3_free_block(ctx, lba);
        }
        inode->extents[i].start_block = 0;
        inode->extents[i].block_count = 0;
    }

    /* Free overflow extent blocks */
    uint64_t *ovf_lbas  = NULL;
    uint64_t  ovf_count = 0;
    int       rc        = overflow_collect_lbas(ctx, inode->inode_id, &ovf_lbas, &ovf_count);
    if(rc != OBMAFS3_OK) return rc;

    for(uint64_t j = 0; j < ovf_count; j++)
    {
        uint32_t ref = 1;
        obmafs3_refcount_get(ctx, ovf_lbas[j], &ref);
        if(ref > 1)
            obmafs3_refcount_dec(ctx, ovf_lbas[j], NULL);
        else
            obmafs3_free_block(ctx, ovf_lbas[j]);
    }
    free(ovf_lbas);

    if(ovf_count > 0) overflow_clear_inode(ctx, inode->inode_id);

    return OBMAFS3_OK;
}

/**
 * Truncate an inode's data blocks to @a new_block_count blocks.
 *
 * Blocks beyond @a new_block_count are freed (refcount-aware).  The
 * remaining blocks' extent map is rebuilt: first 8 coalesced runs
 * go to inline extents; any surplus goes to the overflow B+Tree.
 *
 * @param ctx             Filesystem context.
 * @param inode           Inode record to update (modified in place).
 * @param new_block_count Number of data blocks to keep.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_truncate_file_blocks(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t new_block_count)
{
    /* Count inline blocks */
    uint64_t inline_count = 0;
    for(int i = 0; i < 8; i++) inline_count += inode->extents[i].block_count;

    /* Collect overflow blocks */
    uint64_t *ovf_lbas  = NULL;
    uint64_t  ovf_count = 0;
    int       rc        = overflow_collect_lbas(ctx, inode->inode_id, &ovf_lbas, &ovf_count);
    if(rc != OBMAFS3_OK) return rc;

    uint64_t total = inline_count + ovf_count;
    if(new_block_count >= total)
    {
        free(ovf_lbas);
        return OBMAFS3_OK; /* nothing to free */
    }

    /* Build flat LBA array */
    uint64_t *all_lbas = calloc((size_t)total, sizeof(uint64_t));
    if(!all_lbas)
    {
        free(ovf_lbas);
        return OBMAFS3_ERR_NOMEM;
    }

    uint64_t ai = 0;
    for(int i = 0; i < 8; i++)
        for(uint64_t j = 0; j < inode->extents[i].block_count; j++) all_lbas[ai++] = inode->extents[i].start_block + j;
    for(uint64_t j = 0; j < ovf_count; j++) all_lbas[ai++] = ovf_lbas[j];
    free(ovf_lbas);

    /* Free / decrement blocks beyond new_block_count */
    for(uint64_t b = new_block_count; b < total; b++)
    {
        uint32_t ref = 1;
        obmafs3_refcount_get(ctx, all_lbas[b], &ref);
        if(ref > 1)
            obmafs3_refcount_dec(ctx, all_lbas[b], NULL);
        else
            obmafs3_free_block(ctx, all_lbas[b]);
    }

    /* Clear overflow entries and rebuild with the kept blocks */
    if(ovf_count > 0)
    {
        rc = overflow_clear_inode(ctx, inode->inode_id);
        if(rc != OBMAFS3_OK)
        {
            free(all_lbas);
            return rc;
        }
    }

    rc = rebuild_extent_map(ctx, inode, all_lbas, new_block_count);
    free(all_lbas);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Clone / reflink file range                                         */
/* ------------------------------------------------------------------ */

/**
 * Clone a range of data blocks from one inode to another by sharing
 * physical blocks and incrementing their refcounts.
 *
 * Both offsets and length must be aligned to the filesystem's data
 * capacity (block_size - block_header).  Source blocks may reside in
 * inline extents or the overflow B+Tree.  The destination's existing
 * blocks in the target range are freed (refcount-aware), and the
 * resulting extent map is rebuilt — spilling to the overflow tree
 * when more than 8 non-contiguous runs are required.
 *
 * @param ctx         Filesystem context.
 * @param src_inode   Source inode (read-only).
 * @param src_offset  Byte offset into the source file (aligned).
 * @param dst_inode   Destination inode (modified in place).
 * @param dst_offset  Byte offset into the destination file (aligned).
 * @param length      Number of bytes to clone (aligned).
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_clone_file_range(struct obmafs3_ctx *ctx, const struct inode_record *src_inode, uint64_t src_offset,
                             struct inode_record *dst_inode, uint64_t dst_offset, uint64_t length)
{
    size_t data_cap = (size_t)(ctx->sb.block_size - sizeof(struct block_header));
    int    rc;

    /* Validate alignment */
    if(src_offset % data_cap || dst_offset % data_cap || length % data_cap || length == 0) return OBMAFS3_ERR_INVAL;

    /* Same inode not supported */
    if(src_inode->inode_id == dst_inode->inode_id) return OBMAFS3_ERR_INVAL;

    uint64_t num_blocks = length / data_cap;
    uint64_t src_start  = src_offset / data_cap;
    uint64_t dst_start  = dst_offset / data_cap;

    /* Verify source range is within file bounds */
    uint64_t src_total = (src_inode->file_size + data_cap - 1) / data_cap;
    if(src_start + num_blocks > src_total) return OBMAFS3_ERR_INVAL;

    /* Count source inline blocks */
    uint64_t src_inline = 0;
    for(int i = 0; i < 8; i++) src_inline += src_inode->extents[i].block_count;

    /* Collect source overflow LBAs if the range extends beyond inline */
    uint64_t *src_ovf_lbas  = NULL;
    uint64_t  src_ovf_count = 0;
    if(src_start + num_blocks > src_inline)
    {
        rc = overflow_collect_lbas(ctx, src_inode->inode_id, &src_ovf_lbas, &src_ovf_count);
        if(rc != OBMAFS3_OK) return rc;
    }

    uint64_t src_total_blocks = src_inline + src_ovf_count;
    if(src_start + num_blocks > src_total_blocks)
    {
        free(src_ovf_lbas);
        return OBMAFS3_ERR_INVAL;
    }

    /* Build a flat source LBA array (inline + overflow) */
    uint64_t *src_all = calloc((size_t)src_total_blocks, sizeof(uint64_t));
    if(!src_all)
    {
        free(src_ovf_lbas);
        return OBMAFS3_ERR_NOMEM;
    }

    uint64_t ai = 0;
    for(int i = 0; i < 8; i++)
        for(uint64_t j = 0; j < src_inode->extents[i].block_count; j++)
            src_all[ai++] = src_inode->extents[i].start_block + j;
    for(uint64_t j = 0; j < src_ovf_count; j++) src_all[ai++] = src_ovf_lbas[j];
    free(src_ovf_lbas);

    /* Pointer into src_all for the requested range */
    const uint64_t *src_lbas = src_all + src_start;

    /* --- Destination handling --- */

    /* Count destination inline blocks */
    uint64_t dst_inline = 0;
    for(int i = 0; i < 8; i++) dst_inline += dst_inode->extents[i].block_count;

    /* Collect destination overflow LBAs */
    uint64_t *dst_ovf_lbas  = NULL;
    uint64_t  dst_ovf_count = 0;
    rc                      = overflow_collect_lbas(ctx, dst_inode->inode_id, &dst_ovf_lbas, &dst_ovf_count);
    if(rc != OBMAFS3_OK)
    {
        free(src_all);
        return rc;
    }

    uint64_t dst_existing  = dst_inline + dst_ovf_count;
    uint64_t dst_end_block = dst_start + num_blocks;
    uint64_t dst_needed    = dst_end_block > dst_existing ? dst_end_block : dst_existing;

    /* Build flat destination LBA array */
    uint64_t *dst_all = calloc((size_t)dst_needed, sizeof(uint64_t));
    if(!dst_all)
    {
        free(src_all);
        free(dst_ovf_lbas);
        return OBMAFS3_ERR_NOMEM;
    }

    ai = 0;
    for(int i = 0; i < 8; i++)
        for(uint64_t j = 0; j < dst_inode->extents[i].block_count; j++)
            if(ai < dst_needed) dst_all[ai++] = dst_inode->extents[i].start_block + j;
    for(uint64_t j = 0; j < dst_ovf_count; j++)
        if(ai < dst_needed) dst_all[ai++] = dst_ovf_lbas[j];
    free(dst_ovf_lbas);

    /* Allocate zero-initialised blocks for any gap between the
     * current destination end and the clone start */
    if(dst_start > dst_existing)
    {
        uint64_t gap = dst_start - dst_existing;
        uint64_t gap_start;
        rc = obmafs3_alloc_blocks(ctx, gap, &gap_start);
        if(rc != OBMAFS3_OK)
        {
            free(src_all);
            free(dst_all);
            return rc;
        }
        uint8_t *zbuf = calloc(1, (size_t)ctx->sb.block_size);
        if(!zbuf)
        {
            free(src_all);
            free(dst_all);
            return OBMAFS3_ERR_NOMEM;
        }
        struct block_header zhdr;
        memset(&zhdr, 0, sizeof(zhdr));
        zhdr.magic = OBMAFS3_BLOCK_MAGIC;
        obmafs3_checksum_block(zbuf + sizeof(zhdr), 0, zhdr.checksum);
        memcpy(zbuf, &zhdr, sizeof(zhdr));
        for(uint64_t g = 0; g < gap; g++)
        {
            obmafs3_block_write(ctx, gap_start + g, zbuf, (size_t)ctx->sb.block_size);
            dst_all[dst_existing + g] = gap_start + g;
        }
        free(zbuf);
    }

    /* Free old destination blocks in the clone range */
    for(uint64_t b = dst_start; b < dst_start + num_blocks; b++)
    {
        if(b < dst_existing && dst_all[b] != 0)
        {
            uint32_t ref = 1;
            obmafs3_refcount_get(ctx, dst_all[b], &ref);
            if(ref > 1)
                obmafs3_refcount_dec(ctx, dst_all[b], NULL);
            else
                obmafs3_free_block(ctx, dst_all[b]);
        }
    }

    /* Replace destination blocks in the clone range */
    for(uint64_t k = 0; k < num_blocks; k++) dst_all[dst_start + k] = src_lbas[k];

    /* Increment refcounts for source blocks */
    for(uint64_t k = 0; k < num_blocks; k++)
    {
        rc = obmafs3_refcount_inc(ctx, src_lbas[k]);
        if(rc != OBMAFS3_OK)
        {
            free(src_all);
            free(dst_all);
            return rc;
        }
    }

    /* Clear destination overflow entries and rebuild extent map */
    rc = overflow_clear_inode(ctx, dst_inode->inode_id);
    if(rc != OBMAFS3_OK)
    {
        free(src_all);
        free(dst_all);
        return rc;
    }
    rc = rebuild_extent_map(ctx, dst_inode, dst_all, dst_needed);

    free(src_all);
    free(dst_all);
    if(rc != OBMAFS3_OK) return rc;

    uint64_t new_end = dst_offset + length;
    if(new_end > dst_inode->file_size) dst_inode->file_size = new_end;

    return OBMAFS3_OK;
}
