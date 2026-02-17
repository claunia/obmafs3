/*
 * block.c - OBMAFS3 block I/O, compression, and file data reading/writing
 */
#include "obmafs.h"

#include <zstd.h>
#include <stdlib.h>
#include <string.h>

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
int obmafs3_compress(const void *src, size_t src_size,
                     void *dst, size_t *dst_size, int level)
{
    size_t result = ZSTD_compress(dst, *dst_size, src, src_size, level);
    if (ZSTD_isError(result))
        return OBMAFS3_ERR_IO;
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
int obmafs3_decompress(const void *src, size_t src_size,
                       void *dst, size_t dst_size)
{
    size_t result = ZSTD_decompress(dst, dst_size, src, src_size);
    if (ZSTD_isError(result))
        return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Overflow extent B+Tree helpers                                     */
/* ------------------------------------------------------------------ */

/** Compute and store the checksum for a btree node block. */
static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr = (struct btree_node_header *)buf;
    size_t data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

/** Maximum overflow_extent records in a leaf node. */
static uint16_t overflow_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct overflow_extent));
}

/** Maximum btree_index_entry entries in an index node. */
static uint16_t overflow_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct btree_index_entry));
}

/**
 * Binary search for (inode_id, start_block) in an overflow leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int overflow_leaf_find(const uint8_t *buf, uint16_t node_keys,
                              uint64_t inode_id, uint64_t start_block)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        struct overflow_extent oe;
        memcpy(&oe, data + (size_t)mid * sizeof(oe), sizeof(oe));

        if (oe.inode_id < inode_id) {
            lo = mid + 1;
        } else if (oe.inode_id > inode_id) {
            hi = mid - 1;
        } else if (oe.start_block < start_block) {
            lo = mid + 1;
        } else if (oe.start_block > start_block) {
            hi = mid - 1;
        } else {
            return mid;
        }
    }

    return -(lo + 1);
}

/**
 * Binary search in an overflow index node for the child covering inode_id.
 * Returns the slot index of the child pointer to follow.
 */
static uint16_t overflow_index_find(const uint8_t *buf, uint16_t node_keys,
                                    uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;
    uint16_t result = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t mid_key;
        memcpy(&mid_key,
               data + (size_t)mid * sizeof(struct btree_index_entry),
               sizeof(mid_key));
        if (mid_key <= inode_id) {
            result = (uint16_t)mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return result;
}

#define OVERFLOW_BTREE_MAX_DEPTH 8

struct overflow_btree_path {
    uint64_t lba;
    uint16_t slot;
};

/**
 * Insert an extent into the overflow B+Tree.
 * Entries are sorted by (inode_id, start_block).
 */
static int overflow_insert(struct obmafs3_ctx *ctx,
                           const struct overflow_extent *entry)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    uint64_t hdr_lba = ctx->sb.overflow_lba;
    size_t   bsz     = (size_t)ctx->sb.block_size;
    int rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if (hdr->root_node_lba == 0) {
        uint64_t root_lba;
        rc = obmafs3_alloc_block(ctx, &root_lba);
        if (rc != OBMAFS3_OK)
            return rc;

        uint8_t *buf = calloc(1, bsz);
        if (!buf)
            return OBMAFS3_ERR_NOMEM;

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
        free(buf);
        if (rc != OBMAFS3_OK)
            return rc;

        hdr->root_node_lba = root_lba;
        hdr->total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct overflow_btree_path path[OVERFLOW_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = hdr->root_node_lba;

    while (1) {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if (nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (nhdr.level == 0)
            break; /* reached leaf */

        if (depth >= OVERFLOW_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = overflow_index_find(buf, nhdr.node_keys,
                                            entry->inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct btree_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int      idx        = overflow_leaf_find(buf, leaf_hdr.node_keys,
                                             entry->inode_id,
                                             entry->start_block);
    int      insert_pos = (idx >= 0) ? idx : -(idx + 1);
    uint16_t max_leaf   = overflow_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct overflow_extent);

    if (leaf_hdr.node_keys < max_leaf) {
        /* Room in leaf — sorted insert */
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if (insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz,
                    data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, entry, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length =
            (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* ---- Leaf is full: split ---- */
    uint16_t total = max_leaf + 1;
    struct overflow_extent *all = calloc(total, rec_sz);
    if (!all) {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    /* Build sorted array of all records including the new one */
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    all[insert_pos] = *entry;
    memcpy(&all[insert_pos + 1],
           leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    /* Rewrite old leaf with left half */
    memset(leaf_data, 0, bsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;

    uint64_t new_leaf_lba;
    rc = obmafs3_alloc_block(ctx, &new_leaf_lba);
    if (rc != OBMAFS3_OK) {
        free(all);
        free(buf);
        return rc;
    }

    leaf_hdr.node_keys   = left_count;
    leaf_hdr.keys_length = (uint16_t)(left_count * rec_sz);
    leaf_hdr.right_link  = new_leaf_lba;
    memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
    compute_node_checksum(buf);
    rc = obmafs3_block_write(ctx, lba, buf, bsz);
    if (rc != OBMAFS3_OK) {
        free(all);
        free(buf);
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
    memcpy(buf + sizeof(nh), &all[left_count],
           (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = obmafs3_block_write(ctx, new_leaf_lba, buf, bsz);
    if (rc != OBMAFS3_OK) {
        free(all);
        free(buf);
        return rc;
    }

    uint64_t push_key       = all[left_count].inode_id;
    uint64_t push_child     = new_leaf_lba;
    uint64_t left_first_key = all[0].inode_id;
    uint64_t left_lba       = lba;

    free(all);
    hdr->total_nodes++;

    /* ---- Propagate split upward through index nodes ---- */
    while (depth > 0) {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = obmafs3_block_read(ctx, parent_lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = overflow_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct btree_index_entry);

        if (phdr.node_keys < max_idx) {
            /* Room in parent — insert */
            uint8_t *id = buf + sizeof(struct btree_node_header);

            if (idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz,
                        id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)idx_insert) * ie_sz);

            struct btree_index_entry ne;
            ne.key       = push_key;
            ne.child_lba = push_child;
            memcpy(id + (size_t)idx_insert * ie_sz, &ne, sizeof(ne));

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            free(buf);
            if (rc != OBMAFS3_OK)
                return rc;
            return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        }

        /* Parent is full — split the index node */
        uint16_t idx_total = max_idx + 1;
        struct btree_index_entry *aie = calloc(idx_total, ie_sz);
        if (!aie) {
            free(buf);
            return OBMAFS3_ERR_NOMEM;
        }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        aie[idx_insert].key       = push_key;
        aie[idx_insert].child_lba = push_child;
        memcpy(&aie[idx_insert + 1],
               id + (size_t)idx_insert * ie_sz,
               ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

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
        if (rc != OBMAFS3_OK) {
            free(aie);
            free(buf);
            return rc;
        }

        uint64_t new_idx_lba;
        rc = obmafs3_alloc_block(ctx, &new_idx_lba);
        if (rc != OBMAFS3_OK) {
            free(aie);
            free(buf);
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
        if (rc != OBMAFS3_OK) {
            free(aie);
            free(buf);
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
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }

    /* Read old root to get its level */
    rc = obmafs3_block_read(ctx, left_lba, buf, bsz);
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }
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
    free(buf);
    if (rc != OBMAFS3_OK)
        return rc;

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

    /* Traverse index levels to reach the leaf */
    uint64_t lba = hdr->root_node_lba;

    while (1) {
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

        if (nhdr.level == 0)
            break; /* reached leaf */

        uint16_t slot = overflow_index_find(buf, nhdr.node_keys,
                                            inode_id);
        struct btree_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan the leaf (and follow right_link for entries that span leaves) */
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

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int past = 0;
        for (uint16_t i = 0; i < nhdr.node_keys; i++) {
            struct overflow_extent oe;
            memcpy(&oe, entries + i * sizeof(struct overflow_extent),
                   sizeof(oe));

            if (oe.inode_id < inode_id)
                continue;
            if (oe.inode_id > inode_id) {
                past = 1;
                break;
            }

            if (logical_block >= ovf_block_count &&
                logical_block < ovf_block_count + oe.block_count) {
                *phys_lba = oe.start_block +
                            (logical_block - ovf_block_count);
                free(buf);
                return 1;
            }
            ovf_block_count += oe.block_count;
        }

        if (past)
            break;
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

    /* Traverse index levels to reach the leaf */
    uint64_t lba = hdr->root_node_lba;

    while (1) {
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

        if (nhdr.level == 0)
            break;

        uint16_t slot = overflow_index_find(buf, nhdr.node_keys,
                                            inode_id);
        struct btree_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan the leaf (and follow right_link for entries that span leaves) */
    uint64_t total = 0;

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
        int past = 0;
        for (uint16_t i = 0; i < nhdr.node_keys; i++) {
            struct overflow_extent oe;
            memcpy(&oe, entries + i * sizeof(struct overflow_extent),
                   sizeof(oe));

            if (oe.inode_id < inode_id)
                continue;
            if (oe.inode_id > inode_id) {
                past = 1;
                break;
            }
            total += oe.block_count;
        }

        if (past)
            break;
        lba = nhdr.right_link;
    }

    free(buf);
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
int obmafs3_read_file_data(struct obmafs3_ctx *ctx,
                           const struct inode_record *inode,
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

/**
 * Write file data to the filesystem.
 *
 * Allocates blocks as needed (extending inline extents and the overflow
 * extent tree), writes data with optional ZSTD compression, and updates
 * the inode's file size.
 *
 * @param ctx     Filesystem context.
 * @param inode   Inode record to update (modified in place).
 * @param offset  Byte offset within the file to start writing.
 * @param buf     Data buffer to write.
 * @param size    Number of bytes to write.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_write_file_data(struct obmafs3_ctx *ctx,
                            struct inode_record *inode,
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
    uint8_t *work_buf = malloc(data_capacity);
    if (!block_buf || !work_buf) {
        free(block_buf);
        free(work_buf);
        return OBMAFS3_ERR_NOMEM;
    }

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
            free(work_buf);
            return rc;
        }

        /* Decompress existing block data into work_buf so we can
         * safely modify it regardless of the on-disk format. */
        struct block_header existing_hdr;
        memcpy(&existing_hdr, block_buf, sizeof(existing_hdr));

        memset(work_buf, 0, data_capacity);
        if (existing_hdr.magic == OBMAFS3_BLOCK_MAGIC &&
            existing_hdr.original_size > 0) {
            if (existing_hdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) {
                rc = obmafs3_decompress(
                    block_buf + sizeof(struct block_header),
                    (size_t)existing_hdr.compressed_size,
                    work_buf,
                    (size_t)existing_hdr.original_size);
                if (rc != OBMAFS3_OK) {
                    free(block_buf);
                    free(work_buf);
                    return rc;
                }
            } else {
                memcpy(work_buf, block_buf + sizeof(struct block_header),
                       (size_t)existing_hdr.original_size);
            }
        }

        /* Determine how much to write into this block */
        size_t space = data_capacity - offset_in_block;
        size_t to_write = (size - bytes_written < space)
                              ? size - bytes_written
                              : space;

        /* Write new data into the decompressed work buffer */
        memcpy(work_buf + offset_in_block,
               (const uint8_t *)buf + bytes_written, to_write);

        /* Compute the data extent in this block */
        struct block_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        bhdr.magic = OBMAFS3_BLOCK_MAGIC;
        uint64_t block_data_end = offset_in_block + to_write;
        if (existing_hdr.magic == OBMAFS3_BLOCK_MAGIC &&
            existing_hdr.original_size > block_data_end)
            block_data_end = existing_hdr.original_size;
        bhdr.original_size = block_data_end;

        /* Try to compress if enabled */
        uint8_t *out_buf = block_buf;
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
                    work_buf,
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
                    out_buf = comp_block;
                } else {
                    free(comp_block);
                    comp_block = NULL;
                }
            }
        }

        if (!comp_block) {
            /* Store uncompressed — copy work_buf into block_buf */
            memcpy(block_buf + sizeof(struct block_header),
                   work_buf, (size_t)block_data_end);
            bhdr.flags = 0;
            bhdr.compressed_size = block_data_end;
            obmafs3_checksum_block(
                block_buf + sizeof(struct block_header),
                (size_t)block_data_end, bhdr.checksum);
            memcpy(block_buf, &bhdr, sizeof(bhdr));
        }

        rc = obmafs3_block_write(ctx, phys_lba, out_buf,
                                 (size_t)block_size);
        free(comp_block);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            free(work_buf);
            return rc;
        }

        bytes_written += to_write;
    }

    free(block_buf);
    free(work_buf);
    return OBMAFS3_OK;
}
