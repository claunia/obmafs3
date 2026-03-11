// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : metadata.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 Image Metadata B+Tree operations.
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
 * Two trees are maintained in tandem:
 *
 * 1. Per-image metadata tree (metadata_hdr / metadata_lba)
 *    Composite key: (inode_id, key).
 *    Leaf: metadata_record.  Index: metadata_index_entry.
 *
 * 2. Reverse-index tree (metadata_idx_hdr / metadata_idx_lba)
 *    Composite key: (key, value, inode_id).
 *    Leaf: metadata_idx_record.  Index: metadata_idx_index_entry.
 *
 * Both trees use multi-block nodes (METADATA_NODE_BLOCKS blocks
 * each = 32768 bytes with default 4096-byte block size).
 */
#include "btree_internal.h"
#include "debug.h"

#include <ctype.h>
#include <regex.h>
#include <strings.h>

#define METADATA_BTREE_MAX_DEPTH 16

/** Compute node buffer size for metadata trees. */
static size_t meta_node_size(const struct obmafs3_ctx *ctx)
{
    return (size_t)METADATA_NODE_BLOCKS * (size_t)ctx->sb.block_size;
}

/* ------------------------------------------------------------------ */
/*  Per-image metadata tree helpers                                    */
/* ------------------------------------------------------------------ */

/** Maximum metadata_record entries that fit in a leaf node. */
static uint16_t meta_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) - sizeof(struct btree_node_header)) / sizeof(struct metadata_record));
}

/** Maximum metadata_index_entry entries that fit in an index node. */
static uint16_t meta_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) - sizeof(struct btree_node_header)) / sizeof(struct metadata_index_entry));
}

/** Compare composite key (inode_id, key). */
static int meta_key_cmp(uint64_t id_a, const char *key_a, uint64_t id_b, const char *key_b)
{
    if(id_a < id_b) return -1;
    if(id_a > id_b) return 1;
    return strncmp(key_a, key_b, METADATA_KEY_MAX);
}

/** Binary search in a metadata leaf node. Returns index or -(ins)-1. */
static int meta_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, const char *key)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int                           mid = lo + (hi - lo) / 2;
        const struct metadata_record *rec =
            (const struct metadata_record *)(data + (size_t)mid * sizeof(struct metadata_record));

        int cmp = meta_key_cmp(rec->inode_id, rec->key, inode_id, key);
        if(cmp == 0) return mid;
        if(cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -(lo + 1);
}

/** Binary search in a metadata index node. Returns slot to descend. */
static uint16_t meta_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, const char *key)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int                                mid = lo + (hi - lo) / 2;
        const struct metadata_index_entry *ie =
            (const struct metadata_index_entry *)(data + (size_t)mid * sizeof(struct metadata_index_entry));

        int cmp = meta_key_cmp(ie->inode_id, ie->key, inode_id, key);
        if(cmp <= 0)
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

struct meta_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/** Allocate a multi-block node via the btree clump allocator. */
static int meta_alloc_node(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, uint64_t *lba)
{
    return obmafs3_btree_alloc_node(ctx, hdr, hdr_lba, lba);
}

/** Free a multi-block node via the btree clump allocator. */
static void meta_free_node(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, uint64_t lba)
{
    obmafs3_btree_free_node(ctx, hdr, hdr_lba, lba);
}

/** Read a multi-block node. */
static int meta_node_read(struct obmafs3_ctx *ctx, uint64_t lba, uint8_t *buf)
{
    return obmafs3_block_read(ctx, lba, buf, meta_node_size(ctx));
}

/** Write a multi-block node. */
static int meta_node_write(struct obmafs3_ctx *ctx, uint64_t lba, const uint8_t *buf)
{
    return obmafs3_block_write(ctx, lba, buf, meta_node_size(ctx));
}

/* ---- Per-image metadata tree: lookup ---- */

/**
 * Look up a metadata record by inode ID and key.
 *
 * Traverses the per-image metadata B+Tree using the composite key
 * (@p inode_id, @p key).
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID to look up.
 * @param key       Metadata key string.
 * @param record    Output metadata record.
 * @return @c OBMAFS3_OK if found, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
static int meta_tree_lookup(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key, struct metadata_record *record)
{
    uint64_t lba = ctx->metadata_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_ERR_NOTFOUND;

    size_t   nsz = meta_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    while(1)
    {
        int rc = meta_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
        }

        if(hdr.level > 0)
        {
            uint16_t                    slot = meta_index_find(buf, hdr.node_keys, inode_id, key);
            struct metadata_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            int idx = meta_leaf_find(buf, hdr.node_keys, inode_id, key);
            if(idx >= 0)
            {
                memcpy(record, buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(*record), sizeof(*record));
                free(buf);
                return OBMAFS3_OK;
            }
            free(buf);
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ---- Per-image metadata tree: insert / update ---- */

/**
 * Insert or update a metadata record in the per-image B+Tree.
 *
 * Handles leaf splitting and root promotion when the target leaf is
 * full.  If a record with the same key already exists it is replaced.
 *
 * @param ctx  Filesystem context.
 * @param rec  Pointer to the metadata record to insert.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int meta_tree_put(struct obmafs3_ctx *ctx, const struct metadata_record *rec)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_hdr.root_node_lba;
    int      rc;

    /* Empty tree: create a single leaf as root */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = meta_alloc_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, nsz);
        if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        struct btree_node_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        hdr.record_type = kBtreeDataTypeMetadataEntry;
        hdr.level       = 0;
        hdr.node_keys   = 1;
        hdr.keys_length = (uint16_t)sizeof(struct metadata_record);
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), rec, sizeof(*rec));
        compute_node_checksum(buf);

        rc = meta_node_write(ctx, new_lba, buf);
        free(buf);
        if(rc != OBMAFS3_OK) return rc;

        ctx->metadata_hdr.root_node_lba = new_lba;
        ctx->metadata_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
    }

    /* Traverse from root to leaf, recording path */
    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int                    depth = 0;
    uint64_t               lba   = root_lba;

    while(1)
    {
        rc = meta_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
        }

        if(hdr.level == 0) break;

        if(depth >= METADATA_BTREE_MAX_DEPTH)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");
        }

        uint16_t slot    = meta_index_find(buf, hdr.node_keys, rec->inode_id, rec->key);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* buf holds the leaf node at lba */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for update-in-place */
    int idx = meta_leaf_find(buf, leaf_hdr.node_keys, rec->inode_id, rec->key);
    if(idx >= 0)
    {
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(struct metadata_record), rec,
               sizeof(*rec));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
        free(buf);
        return rc;
    }

    /* Insert */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = meta_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct metadata_record);

    if(leaf_hdr.node_keys < max_leaf)
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if(insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz, data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)insert_pos) * rec_sz);
        memcpy(data + (size_t)insert_pos * rec_sz, rec, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
        free(buf);
        return rc;
    }

    /* Leaf full — split */
    uint16_t                total = max_leaf + 1;
    struct metadata_record *all   = calloc(total, rec_sz);
    if(!all)
    {
        free(buf);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(&all[insert_pos], rec, rec_sz);
    memcpy(&all[insert_pos + 1], leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    memset(leaf_data, 0, nsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;
    uint64_t new_leaf_lba;
    rc = meta_alloc_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, &new_leaf_lba);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    leaf_hdr.node_keys   = left_count;
    leaf_hdr.keys_length = (uint16_t)(left_count * rec_sz);
    leaf_hdr.right_link  = new_leaf_lba;
    memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, lba, buf);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    memset(buf, 0, nsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeMetadataEntry;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.left_link   = lba;
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), &all[left_count], (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, new_leaf_lba, buf);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    struct metadata_index_entry push_ie;
    push_ie.inode_id = all[left_count].inode_id;
    strncpy(push_ie.key, all[left_count].key, METADATA_KEY_MAX);
    push_ie.child_lba = new_leaf_lba;

    struct metadata_index_entry left_ie;
    left_ie.inode_id = all[0].inode_id;
    strncpy(left_ie.key, all[0].key, METADATA_KEY_MAX);
    left_ie.child_lba = lba;

    uint64_t left_lba = lba;
    free(all);
    ctx->metadata_hdr.total_nodes++;

    /* Update old right neighbor's left_link */
    if(old_right != 0)
    {
        rc = meta_node_read(ctx, old_right, buf);
        if(rc == OBMAFS3_OK)
        {
            struct btree_node_header rnh;
            memcpy(&rnh, buf, sizeof(rnh));
            rnh.left_link = new_leaf_lba;
            memcpy(buf, &rnh, sizeof(rnh));
            compute_node_checksum(buf);
            meta_node_write(ctx, old_right, buf);
        }
    }

    /* Propagate split upward */
    while(depth > 0)
    {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = meta_node_read(ctx, parent_lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = meta_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct metadata_index_entry);

        if(phdr.node_keys < max_idx)
        {
            uint8_t *id = buf + sizeof(struct btree_node_header);

            /* Update the key at parent_slot to the left child's
             * actual minimum.  Without this, the parent key can
             * be stale (higher than the true minimum) after the
             * leftmost child accumulated entries with keys below
             * the original index key. */
            struct metadata_index_entry m_upd;
            memcpy(&m_upd, id + (size_t)parent_slot * ie_sz, sizeof(m_upd));
            m_upd.inode_id = left_ie.inode_id;
            strncpy(m_upd.key, left_ie.key, METADATA_KEY_MAX);
            memcpy(id + (size_t)parent_slot * ie_sz, &m_upd, sizeof(m_upd));

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz, id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);
            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);
            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);
            rc = meta_node_write(ctx, parent_lba, buf);
            free(buf);
            if(rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
        }

        /* Parent full — split index node */
        uint16_t                     idx_total = max_idx + 1;
        struct metadata_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }

        uint8_t *id = buf + sizeof(struct btree_node_header);

        /* Update the key at parent_slot to the left child's
         * actual minimum before building the merged array. */
        struct metadata_index_entry m_upd;
        memcpy(&m_upd, id + (size_t)parent_slot * ie_sz, sizeof(m_upd));
        m_upd.inode_id = left_ie.inode_id;
        strncpy(m_upd.key, left_ie.key, METADATA_KEY_MAX);
        memcpy(id + (size_t)parent_slot * ie_sz, &m_upd, sizeof(m_upd));

        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        /* Allocate new index node before writing so we can set sibling links */
        uint64_t idx_old_right = phdr.right_link;
        uint64_t new_idx_lba;
        rc = meta_alloc_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, &new_idx_lba);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            free(buf);
            return rc;
        }

        memset(id, 0, nsz - sizeof(struct btree_node_header));
        memcpy(id, aie, (size_t)il * ie_sz);
        phdr.node_keys   = il;
        phdr.keys_length = (uint16_t)(il * ie_sz);
        phdr.right_link  = new_idx_lba;
        memcpy(buf, &phdr, sizeof(phdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, parent_lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            free(buf);
            return rc;
        }

        memset(buf, 0, nsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeMetadataEntry;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        nih.left_link   = parent_lba;
        nih.right_link  = idx_old_right;
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, new_idx_lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            free(buf);
            return rc;
        }

        push_ie.inode_id = aie[il].inode_id;
        strncpy(push_ie.key, aie[il].key, METADATA_KEY_MAX);
        push_ie.child_lba = new_idx_lba;

        left_ie.inode_id = aie[0].inode_id;
        strncpy(left_ie.key, aie[0].key, METADATA_KEY_MAX);
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;
        free(aie);
        ctx->metadata_hdr.total_nodes++;

        /* Update old right neighbor's left_link */
        if(idx_old_right != 0)
        {
            rc = meta_node_read(ctx, idx_old_right, buf);
            if(rc == OBMAFS3_OK)
            {
                struct btree_node_header rnh;
                memcpy(&rnh, buf, sizeof(rnh));
                rnh.left_link = new_idx_lba;
                memcpy(buf, &rnh, sizeof(rnh));
                compute_node_checksum(buf);
                meta_node_write(ctx, idx_old_right, buf);
            }
        }
    }

    /* Create new root */
    uint64_t new_root_lba;
    rc = meta_alloc_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, &new_root_lba);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }

    rc = meta_node_read(ctx, left_lba, buf);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, nsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeMetadataEntry;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * sizeof(struct metadata_index_entry));
    memcpy(buf, &rh, sizeof(rh));

    left_ie.child_lba = left_lba;
    struct metadata_index_entry roots[2];
    roots[0] = left_ie;
    roots[1] = push_ie;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = meta_node_write(ctx, new_root_lba, buf);
    free(buf);
    if(rc != OBMAFS3_OK) return rc;

    ctx->metadata_hdr.root_node_lba = new_root_lba;
    ctx->metadata_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
}

/* ---- Per-image metadata tree: delete ---- */

/**
 * Delete a metadata record from the per-image B+Tree.
 *
 * Locates and removes the record keyed by (@p inode_id, @p key).
 * Frees empty leaf nodes and updates the tree header.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID of the record to delete.
 * @param key       Metadata key string.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
static int meta_tree_delete(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_hdr.root_node_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int                    depth = 0;
    uint64_t               lba   = root_lba;

    while(1)
    {
        rc = meta_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
        }

        if(hdr.level == 0) break;

        if(depth >= METADATA_BTREE_MAX_DEPTH)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");
        }

        uint16_t slot    = meta_index_find(buf, hdr.node_keys, inode_id, key);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = meta_leaf_find(buf, leaf_hdr.node_keys, inode_id, key);
    if(idx < 0)
    {
        free(buf);
        return OBMAFS3_ERR_NOTFOUND;
    }

    size_t rec_sz = sizeof(struct metadata_record);
    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        if(depth == 0)
        {
            ctx->metadata_hdr.root_node_lba = 0;
            ctx->metadata_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
            meta_free_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, lba);
        }
        else
        {
            /* Update sibling links around freed leaf */
            if(leaf_hdr.left_link != 0)
            {
                rc = meta_node_read(ctx, leaf_hdr.left_link, buf);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header lnh;
                    memcpy(&lnh, buf, sizeof(lnh));
                    lnh.right_link = leaf_hdr.right_link;
                    memcpy(buf, &lnh, sizeof(lnh));
                    compute_node_checksum(buf);
                    meta_node_write(ctx, leaf_hdr.left_link, buf);
                }
            }
            if(leaf_hdr.right_link != 0)
            {
                rc = meta_node_read(ctx, leaf_hdr.right_link, buf);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header rnh;
                    memcpy(&rnh, buf, sizeof(rnh));
                    rnh.left_link = leaf_hdr.left_link;
                    memcpy(buf, &rnh, sizeof(rnh));
                    compute_node_checksum(buf);
                    meta_node_write(ctx, leaf_hdr.right_link, buf);
                }
            }

            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, nsz);
            if(!pbuf)
            {
                free(buf);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }

            rc = meta_node_read(ctx, plba, pbuf);
            if(rc != OBMAFS3_OK)
            {
                free(pbuf);
                free(buf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct metadata_index_entry);

            if(pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz, pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);
            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if(phdr.node_keys == 0 && depth == 1)
            {
                ctx->metadata_hdr.root_node_lba = 0;
                ctx->metadata_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
                meta_free_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, lba);
                meta_free_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                struct metadata_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->metadata_hdr.root_node_lba = remaining.child_lba;
                ctx->metadata_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
                meta_free_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, lba);
                meta_free_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, plba);
            }
            else
            {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = meta_node_write(ctx, plba, pbuf);
                if(rc == OBMAFS3_OK)
                {
                    ctx->metadata_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
                }
                meta_free_node(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, lba);
            }
            free(pbuf);
        }
    }
    else
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz, data + ((size_t)idx + 1) * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)idx) * rec_sz);
        memset(data + (size_t)leaf_hdr.node_keys * rec_sz, 0, rec_sz);
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
    }

    free(buf);
    return rc;
}

/* ================================================================== */
/*  Metadata reverse-index B+Tree                                      */
/*                                                                     */
/*  Composite key: (key, value, inode_id).                             */
/* ================================================================== */

/** Maximum metadata_idx_record entries per reverse-index leaf node. */
static uint16_t midx_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) - sizeof(struct btree_node_header)) / sizeof(struct metadata_idx_record));
}

/** Maximum metadata_idx_index_entry entries per reverse-index index node. */
static uint16_t midx_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) - sizeof(struct btree_node_header)) /
                      sizeof(struct metadata_idx_index_entry));
}

/** Compare composite key (key, value, inode_id). */
static int midx_key_cmp(const char *key_a, const char *val_a, uint64_t id_a, const char *key_b, const char *val_b,
                        uint64_t id_b)
{
    int c = strncmp(key_a, key_b, METADATA_KEY_MAX);
    if(c != 0) return c;
    c = strncmp(val_a, val_b, METADATA_VALUE_MAX);
    if(c != 0) return c;
    if(id_a < id_b) return -1;
    if(id_a > id_b) return 1;
    return 0;
}

/** Binary search in a reverse-index leaf node.  Returns index or -(ins)-1. */
static int midx_leaf_find(const uint8_t *buf, uint16_t node_keys, const char *key, const char *value, uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int                               mid = lo + (hi - lo) / 2;
        const struct metadata_idx_record *rec =
            (const struct metadata_idx_record *)(data + (size_t)mid * sizeof(struct metadata_idx_record));

        int cmp = midx_key_cmp(rec->key, rec->value, rec->inode_id, key, value, inode_id);
        if(cmp == 0) return mid;
        if(cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -(lo + 1);
}

/** Binary search in a reverse-index index node.  Returns slot to descend. */
static uint16_t midx_index_find(const uint8_t *buf, uint16_t node_keys, const char *key, const char *value,
                                uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int                                    mid = lo + (hi - lo) / 2;
        const struct metadata_idx_index_entry *ie =
            (const struct metadata_idx_index_entry *)(data + (size_t)mid * sizeof(struct metadata_idx_index_entry));

        int cmp = midx_key_cmp(ie->key, ie->value, ie->inode_id, key, value, inode_id);
        if(cmp <= 0)
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

/* ---- Reverse-index tree: insert ---- */

/**
 * Insert a record into the metadata reverse-index B+Tree.
 *
 * Handles leaf splitting and root promotion.  The composite key is
 * (key, value, inode_id).
 *
 * @param ctx  Filesystem context.
 * @param rec  Pointer to the reverse-index record to insert.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int midx_tree_put(struct obmafs3_ctx *ctx, const struct metadata_idx_record *rec)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_idx_hdr.root_node_lba;
    int      rc;

    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = meta_alloc_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, nsz);
        if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        struct btree_node_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        hdr.record_type = kBtreeDataTypeMetadataIndexEntry;
        hdr.level       = 0;
        hdr.node_keys   = 1;
        hdr.keys_length = (uint16_t)sizeof(struct metadata_idx_record);
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), rec, sizeof(*rec));
        compute_node_checksum(buf);

        rc = meta_node_write(ctx, new_lba, buf);
        free(buf);
        if(rc != OBMAFS3_OK) return rc;

        ctx->metadata_idx_hdr.root_node_lba = new_lba;
        ctx->metadata_idx_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
    }

    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int                    depth = 0;
    uint64_t               lba   = root_lba;

    while(1)
    {
        rc = meta_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
        }

        if(hdr.level == 0) break;

        if(depth >= METADATA_BTREE_MAX_DEPTH)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");
        }

        uint16_t slot    = midx_index_find(buf, hdr.node_keys, rec->key, rec->value, rec->inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_idx_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for duplicate (should not happen if caller is correct) */
    int idx = midx_leaf_find(buf, leaf_hdr.node_keys, rec->key, rec->value, rec->inode_id);
    if(idx >= 0)
    {
        /* Already exists — update in place */
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(struct metadata_idx_record), rec,
               sizeof(*rec));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
        free(buf);
        return rc;
    }

    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = midx_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct metadata_idx_record);

    if(leaf_hdr.node_keys < max_leaf)
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if(insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz, data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)insert_pos) * rec_sz);
        memcpy(data + (size_t)insert_pos * rec_sz, rec, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
        free(buf);
        return rc;
    }

    /* Leaf full — split */
    uint16_t                    total = max_leaf + 1;
    struct metadata_idx_record *all   = calloc(total, rec_sz);
    if(!all)
    {
        free(buf);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(&all[insert_pos], rec, rec_sz);
    memcpy(&all[insert_pos + 1], leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    memset(leaf_data, 0, nsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;
    uint64_t new_leaf_lba;
    rc = meta_alloc_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, &new_leaf_lba);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    leaf_hdr.node_keys   = left_count;
    leaf_hdr.keys_length = (uint16_t)(left_count * rec_sz);
    leaf_hdr.right_link  = new_leaf_lba;
    memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, lba, buf);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    memset(buf, 0, nsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeMetadataIndexEntry;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.left_link   = lba;
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), &all[left_count], (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, new_leaf_lba, buf);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    struct metadata_idx_index_entry push_ie;
    strncpy(push_ie.key, all[left_count].key, METADATA_KEY_MAX);
    strncpy(push_ie.value, all[left_count].value, METADATA_VALUE_MAX);
    push_ie.inode_id  = all[left_count].inode_id;
    push_ie.child_lba = new_leaf_lba;

    struct metadata_idx_index_entry left_ie;
    strncpy(left_ie.key, all[0].key, METADATA_KEY_MAX);
    strncpy(left_ie.value, all[0].value, METADATA_VALUE_MAX);
    left_ie.inode_id  = all[0].inode_id;
    left_ie.child_lba = lba;

    uint64_t left_lba = lba;
    free(all);
    ctx->metadata_idx_hdr.total_nodes++;

    /* Update old right neighbor's left_link */
    if(old_right != 0)
    {
        rc = meta_node_read(ctx, old_right, buf);
        if(rc == OBMAFS3_OK)
        {
            struct btree_node_header rnh;
            memcpy(&rnh, buf, sizeof(rnh));
            rnh.left_link = new_leaf_lba;
            memcpy(buf, &rnh, sizeof(rnh));
            compute_node_checksum(buf);
            meta_node_write(ctx, old_right, buf);
        }
    }

    /* Propagate split upward */
    while(depth > 0)
    {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = meta_node_read(ctx, parent_lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = midx_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct metadata_idx_index_entry);

        if(phdr.node_keys < max_idx)
        {
            uint8_t *id = buf + sizeof(struct btree_node_header);

            /* Update the key at parent_slot to the left child's
             * actual minimum.  Without this, the parent key can
             * be stale (higher than the true minimum) after the
             * leftmost child accumulated entries with keys below
             * the original index key. */
            struct metadata_idx_index_entry mi_upd;
            memcpy(&mi_upd, id + (size_t)parent_slot * ie_sz, sizeof(mi_upd));
            strncpy(mi_upd.key, left_ie.key, METADATA_KEY_MAX);
            strncpy(mi_upd.value, left_ie.value, METADATA_VALUE_MAX);
            mi_upd.inode_id = left_ie.inode_id;
            memcpy(id + (size_t)parent_slot * ie_sz, &mi_upd, sizeof(mi_upd));

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz, id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);
            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);
            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);
            rc = meta_node_write(ctx, parent_lba, buf);
            free(buf);
            if(rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
        }

        /* Parent full — split index node */
        uint16_t                         idx_total = max_idx + 1;
        struct metadata_idx_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }

        uint8_t *id = buf + sizeof(struct btree_node_header);

        /* Update the key at parent_slot to the left child's
         * actual minimum before building the merged array. */
        struct metadata_idx_index_entry mi_upd;
        memcpy(&mi_upd, id + (size_t)parent_slot * ie_sz, sizeof(mi_upd));
        strncpy(mi_upd.key, left_ie.key, METADATA_KEY_MAX);
        strncpy(mi_upd.value, left_ie.value, METADATA_VALUE_MAX);
        mi_upd.inode_id = left_ie.inode_id;
        memcpy(id + (size_t)parent_slot * ie_sz, &mi_upd, sizeof(mi_upd));

        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        /* Allocate new index node before writing so we can set sibling links */
        uint64_t idx_old_right = phdr.right_link;
        uint64_t new_idx_lba;
        rc = meta_alloc_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, &new_idx_lba);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            free(buf);
            return rc;
        }

        memset(id, 0, nsz - sizeof(struct btree_node_header));
        memcpy(id, aie, (size_t)il * ie_sz);
        phdr.node_keys   = il;
        phdr.keys_length = (uint16_t)(il * ie_sz);
        phdr.right_link  = new_idx_lba;
        memcpy(buf, &phdr, sizeof(phdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, parent_lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            free(buf);
            return rc;
        }

        memset(buf, 0, nsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeMetadataIndexEntry;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        nih.left_link   = parent_lba;
        nih.right_link  = idx_old_right;
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, new_idx_lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            free(buf);
            return rc;
        }

        strncpy(push_ie.key, aie[il].key, METADATA_KEY_MAX);
        strncpy(push_ie.value, aie[il].value, METADATA_VALUE_MAX);
        push_ie.inode_id  = aie[il].inode_id;
        push_ie.child_lba = new_idx_lba;

        strncpy(left_ie.key, aie[0].key, METADATA_KEY_MAX);
        strncpy(left_ie.value, aie[0].value, METADATA_VALUE_MAX);
        left_ie.inode_id  = aie[0].inode_id;
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;
        free(aie);
        ctx->metadata_idx_hdr.total_nodes++;

        /* Update old right neighbor's left_link */
        if(idx_old_right != 0)
        {
            rc = meta_node_read(ctx, idx_old_right, buf);
            if(rc == OBMAFS3_OK)
            {
                struct btree_node_header rnh;
                memcpy(&rnh, buf, sizeof(rnh));
                rnh.left_link = new_idx_lba;
                memcpy(buf, &rnh, sizeof(rnh));
                compute_node_checksum(buf);
                meta_node_write(ctx, idx_old_right, buf);
            }
        }
    }

    /* Create new root */
    uint64_t new_root_lba;
    rc = meta_alloc_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, &new_root_lba);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }

    rc = meta_node_read(ctx, left_lba, buf);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, nsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeMetadataIndexEntry;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * sizeof(struct metadata_idx_index_entry));
    memcpy(buf, &rh, sizeof(rh));

    left_ie.child_lba = left_lba;
    struct metadata_idx_index_entry roots[2];
    roots[0] = left_ie;
    roots[1] = push_ie;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = meta_node_write(ctx, new_root_lba, buf);
    free(buf);
    if(rc != OBMAFS3_OK) return rc;

    ctx->metadata_idx_hdr.root_node_lba = new_root_lba;
    ctx->metadata_idx_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
}

/* ---- Reverse-index tree: delete ---- */

/**
 * Delete a record from the metadata reverse-index B+Tree.
 *
 * @param ctx       Filesystem context.
 * @param key       Metadata key.
 * @param value     Metadata value.
 * @param inode_id  Inode ID component of the composite key.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
static int midx_tree_delete(struct obmafs3_ctx *ctx, const char *key, const char *value, uint64_t inode_id)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_idx_hdr.root_node_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int                    depth = 0;
    uint64_t               lba   = root_lba;

    while(1)
    {
        rc = meta_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
        }

        if(hdr.level == 0) break;

        if(depth >= METADATA_BTREE_MAX_DEPTH)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");
        }

        uint16_t slot    = midx_index_find(buf, hdr.node_keys, key, value, inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_idx_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = midx_leaf_find(buf, leaf_hdr.node_keys, key, value, inode_id);
    if(idx < 0)
    {
        free(buf);
        return OBMAFS3_ERR_NOTFOUND;
    }

    size_t rec_sz = sizeof(struct metadata_idx_record);
    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        if(depth == 0)
        {
            ctx->metadata_idx_hdr.root_node_lba = 0;
            ctx->metadata_idx_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
            meta_free_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, lba);
        }
        else
        {
            /* Update sibling links around freed leaf */
            if(leaf_hdr.left_link != 0)
            {
                rc = meta_node_read(ctx, leaf_hdr.left_link, buf);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header lnh;
                    memcpy(&lnh, buf, sizeof(lnh));
                    lnh.right_link = leaf_hdr.right_link;
                    memcpy(buf, &lnh, sizeof(lnh));
                    compute_node_checksum(buf);
                    meta_node_write(ctx, leaf_hdr.left_link, buf);
                }
            }
            if(leaf_hdr.right_link != 0)
            {
                rc = meta_node_read(ctx, leaf_hdr.right_link, buf);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header rnh;
                    memcpy(&rnh, buf, sizeof(rnh));
                    rnh.left_link = leaf_hdr.left_link;
                    memcpy(buf, &rnh, sizeof(rnh));
                    compute_node_checksum(buf);
                    meta_node_write(ctx, leaf_hdr.right_link, buf);
                }
            }

            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, nsz);
            if(!pbuf)
            {
                free(buf);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }

            rc = meta_node_read(ctx, plba, pbuf);
            if(rc != OBMAFS3_OK)
            {
                free(pbuf);
                free(buf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct metadata_idx_index_entry);

            if(pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz, pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);
            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if(phdr.node_keys == 0 && depth == 1)
            {
                ctx->metadata_idx_hdr.root_node_lba = 0;
                ctx->metadata_idx_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
                meta_free_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, lba);
                meta_free_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                struct metadata_idx_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->metadata_idx_hdr.root_node_lba = remaining.child_lba;
                ctx->metadata_idx_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
                meta_free_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, lba);
                meta_free_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, plba);
            }
            else
            {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = meta_node_write(ctx, plba, pbuf);
                if(rc == OBMAFS3_OK)
                {
                    ctx->metadata_idx_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
                }
                meta_free_node(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, lba);
            }
            free(pbuf);
        }
    }
    else
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz, data + ((size_t)idx + 1) * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)idx) * rec_sz);
        memset(data + (size_t)leaf_hdr.node_keys * rec_sz, 0, rec_sz);
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
    }

    free(buf);
    return rc;
}

/* ================================================================== */
/*  Metadata public API                                                */
/* ================================================================== */

/**
 * Retrieve a metadata value for a given inode and key.
 *
 * @param ctx         Filesystem context.
 * @param inode_id    Inode ID of the file.
 * @param key         Metadata key to look up.
 * @param value       Output buffer for the NUL-terminated value.
 * @param value_size  Size of @p value in bytes.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_metadata_get(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key, char *value, size_t value_size)
{
    if(ctx->sb.metadata_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    struct metadata_record rec;
    int                    rc = meta_tree_lookup(ctx, inode_id, key, &rec);
    if(rc != OBMAFS3_OK) return rc;

    strncpy(value, rec.value, value_size);
    if(value_size > 0) value[value_size - 1] = '\0';
    return OBMAFS3_OK;
}

/**
 * Store or update a metadata key/value pair for a given inode.
 *
 * If the key already exists the old reverse-index entry is removed
 * before inserting the new value into both the per-image and
 * reverse-index trees.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID of the file.
 * @param key       Metadata key (max 255 characters).
 * @param value     Metadata value (max 1024 characters).
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_metadata_put(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key, const char *value)
{
    if(ctx->sb.metadata_lba == 0 || ctx->sb.metadata_idx_lba == 0) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");
    if(!key || strlen(key) == 0 || strlen(key) > 255) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");
    if(!value || strlen(value) > 1024) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

    /* If key already exists, remove old index entry first */
    struct metadata_record old_rec;
    int                    rc = meta_tree_lookup(ctx, inode_id, key, &old_rec);
    if(rc == OBMAFS3_OK)
    {
        /* Remove old reverse-index entry */
        midx_tree_delete(ctx, old_rec.key, old_rec.value, inode_id);
    }
    else if(rc != OBMAFS3_ERR_NOTFOUND) { return rc; }

    /* Build and insert the per-image record */
    struct metadata_record new_rec;
    memset(&new_rec, 0, sizeof(new_rec));
    new_rec.inode_id = inode_id;
    strncpy(new_rec.key, key, METADATA_KEY_MAX - 1);
    strncpy(new_rec.value, value, METADATA_VALUE_MAX - 1);

    rc = meta_tree_put(ctx, &new_rec);
    if(rc != OBMAFS3_OK) return rc;

    /* Insert reverse-index entry */
    struct metadata_idx_record idx_rec;
    memset(&idx_rec, 0, sizeof(idx_rec));
    strncpy(idx_rec.key, key, METADATA_KEY_MAX - 1);
    strncpy(idx_rec.value, value, METADATA_VALUE_MAX - 1);
    idx_rec.inode_id = inode_id;

    return midx_tree_put(ctx, &idx_rec);
}

/**
 * Delete a metadata key/value pair for a given inode.
 *
 * Removes the entry from both the per-image tree and the reverse-index
 * tree.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID of the file.
 * @param key       Metadata key to delete.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_metadata_delete(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key)
{
    if(ctx->sb.metadata_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    /* Look up the value so we can delete the index entry */
    struct metadata_record rec;
    int                    rc = meta_tree_lookup(ctx, inode_id, key, &rec);
    if(rc != OBMAFS3_OK) return rc;

    /* Delete from per-image tree */
    rc = meta_tree_delete(ctx, inode_id, key);
    if(rc != OBMAFS3_OK) return rc;

    /* Delete from reverse-index tree */
    if(ctx->sb.metadata_idx_lba != 0) midx_tree_delete(ctx, rec.key, rec.value, inode_id);

    return OBMAFS3_OK;
}

/**
 * Delete all metadata key/value pairs for a given inode.
 *
 * Repeatedly lists and deletes entries until none remain.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID of the file.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_metadata_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    if(ctx->sb.metadata_lba == 0) return OBMAFS3_OK;

    while(1)
    {
        char   **keys;
        uint32_t count;
        int      rc = obmafs3_metadata_list(ctx, inode_id, &keys, &count);
        if(rc != OBMAFS3_OK) return rc;
        if(count == 0)
        {
            obmafs3_metadata_list_free(keys, count);
            return OBMAFS3_OK;
        }

        char first_key[METADATA_KEY_MAX];
        strncpy(first_key, keys[0], METADATA_KEY_MAX);
        first_key[METADATA_KEY_MAX - 1] = '\0';
        obmafs3_metadata_list_free(keys, count);

        rc = obmafs3_metadata_delete(ctx, inode_id, first_key);
        if(rc != OBMAFS3_OK) return rc;
    }
}

/**
 * List all metadata keys stored for a given inode.
 *
 * Traverses the per-image metadata B+Tree leaf chain and collects all
 * keys associated with @p inode_id.  The caller must free the returned
 * array with @c obmafs3_metadata_list_free.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID of the file.
 * @param keys      Output array of key strings.
 * @param count     Output number of keys.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_metadata_list(struct obmafs3_ctx *ctx, uint64_t inode_id, char ***keys, uint32_t *count)
{
    *keys  = NULL;
    *count = 0;

    if(ctx->sb.metadata_lba == 0) return OBMAFS3_OK;

    uint64_t lba = ctx->metadata_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_OK;

    size_t   nsz = meta_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Traverse to the leaf that would contain (inode_id, "") */
    while(1)
    {
        int rc = meta_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
        }

        if(hdr.level == 0) break;

        uint16_t                    slot = meta_index_find(buf, hdr.node_keys, inode_id, "");
        struct metadata_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain */
    uint32_t cap = 16;
    char   **kl  = malloc(cap * sizeof(char *));
    if(!kl)
    {
        free(buf);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    uint32_t n = 0;

    while(1)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct metadata_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if(rec.inode_id == inode_id)
            {
                if(n >= cap)
                {
                    cap *= 2;
                    char **tmp = realloc(kl, cap * sizeof(char *));
                    if(!tmp)
                    {
                        for(uint32_t j = 0; j < n; j++) free(kl[j]);
                        free(kl);
                        free(buf);
                        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                    }
                    kl = tmp;
                }
                kl[n] = strndup(rec.key, METADATA_KEY_MAX);
                if(!kl[n])
                {
                    for(uint32_t j = 0; j < n; j++) free(kl[j]);
                    free(kl);
                    free(buf);
                    DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                }
                n++;
            }
            else if(rec.inode_id > inode_id) { goto list_done; }
        }

        if(hdr.right_link == 0) break;

        int rc = meta_node_read(ctx, hdr.right_link, buf);
        if(rc != OBMAFS3_OK)
        {
            for(uint32_t j = 0; j < n; j++) free(kl[j]);
            free(kl);
            free(buf);
            return rc;
        }
    }

list_done:
    free(buf);
    *keys  = kl;
    *count = n;
    return OBMAFS3_OK;
}

/**
 * Free a key array returned by @c obmafs3_metadata_list.
 *
 * @param keys   Array to free (may be NULL).
 * @param count  Number of elements in @p keys.
 */
void obmafs3_metadata_list_free(char **keys, uint32_t count)
{
    if(!keys) return;
    for(uint32_t i = 0; i < count; i++) free(keys[i]);
    free(keys);
}

/* ------------------------------------------------------------------ */
/*  Path resolution cache                                              */
/* ------------------------------------------------------------------ */

/*
 * A simple open-addressing hash table that caches inode_id → path
 * mappings during a query batch.  This avoids repeated full catalog
 * scans for shared parent directories when resolving sibling inodes.
 */

#define PATH_CACHE_INITIAL_CAP 64 /* must be power of 2 */

struct path_cache_entry
{
    uint64_t inode_id; /* 0 = empty slot */
    char    *path;     /* heap-allocated, owned by the cache */
};

struct path_cache
{
    struct path_cache_entry *entries;
    uint32_t                 cap;   /* always power of 2 */
    uint32_t                 count;
};

static int path_cache_init(struct path_cache *pc)
{
    pc->cap     = PATH_CACHE_INITIAL_CAP;
    pc->count   = 0;
    pc->entries = calloc(pc->cap, sizeof(struct path_cache_entry));
    return pc->entries ? 0 : -1;
}

static void path_cache_free(struct path_cache *pc)
{
    if(!pc->entries) return;
    for(uint32_t i = 0; i < pc->cap; i++)
        free(pc->entries[i].path);
    free(pc->entries);
    pc->entries = NULL;
    pc->count   = 0;
    pc->cap     = 0;
}

static uint32_t pc_slot(uint64_t id, uint32_t cap) { return (uint32_t)(id * 0x9E3779B97F4A7C15ULL >> 32) & (cap - 1); }

static const char *path_cache_lookup(const struct path_cache *pc, uint64_t inode_id)
{
    uint32_t mask = pc->cap - 1;
    uint32_t idx  = pc_slot(inode_id, pc->cap);
    for(uint32_t i = 0; i < pc->cap; i++)
    {
        uint32_t slot = (idx + i) & mask;
        if(pc->entries[slot].inode_id == 0) return NULL;
        if(pc->entries[slot].inode_id == inode_id) return pc->entries[slot].path;
    }
    return NULL;
}

static int path_cache_grow(struct path_cache *pc)
{
    uint32_t                 new_cap = pc->cap * 2;
    struct path_cache_entry *new_ent = calloc(new_cap, sizeof(struct path_cache_entry));
    if(!new_ent) return -1;

    uint32_t mask = new_cap - 1;
    for(uint32_t i = 0; i < pc->cap; i++)
    {
        if(pc->entries[i].inode_id == 0) continue;
        uint32_t idx = pc_slot(pc->entries[i].inode_id, new_cap);
        while(new_ent[idx].inode_id != 0) idx = (idx + 1) & mask;
        new_ent[idx] = pc->entries[i];
    }
    free(pc->entries);
    pc->entries = new_ent;
    pc->cap     = new_cap;
    return 0;
}

static int path_cache_insert(struct path_cache *pc, uint64_t inode_id, const char *path)
{
    /* Grow at 70% load */
    if(pc->count * 10 >= pc->cap * 7)
    {
        if(path_cache_grow(pc)) return -1;
    }

    uint32_t mask = pc->cap - 1;
    uint32_t idx  = pc_slot(inode_id, pc->cap);
    while(pc->entries[idx].inode_id != 0)
    {
        if(pc->entries[idx].inode_id == inode_id) return 0; /* already cached */
        idx = (idx + 1) & mask;
    }
    pc->entries[idx].inode_id = inode_id;
    pc->entries[idx].path     = strdup(path);
    if(!pc->entries[idx].path) return -1;
    pc->count++;
    return 0;
}

/**
 * Resolve an inode ID to its full path, using a cache to avoid
 * repeated catalog scans for shared parent directories.
 *
 * On first call for a given inode, walks bottom-up via catalog_find_by_inode
 * but stops as soon as a cached ancestor is found.  All intermediate
 * directories (and the target inode itself) are inserted into the cache.
 */
static int resolve_inode_path_cached(struct obmafs3_ctx *ctx, uint64_t inode_id, char *path_buf, size_t path_buf_size,
                                     struct path_cache *pc)
{
    if(path_buf_size == 0) return OBMAFS3_ERR_NOMEM;

    if(inode_id == OBMAFS3_ROOT_INODE_ID)
    {
        path_buf[0] = '/';
        path_buf[1] = '\0';
        return OBMAFS3_OK;
    }

    /* Check cache first */
    const char *cached = path_cache_lookup(pc, inode_id);
    if(cached)
    {
        if(strlen(cached) + 1 > path_buf_size) return OBMAFS3_ERR_NOMEM;
        memcpy(path_buf, cached, strlen(cached) + 1);
        return OBMAFS3_OK;
    }

    /* Walk bottom-up, stopping at a cached ancestor or root */
    const int MAX_DEPTH = 256;

    struct
    {
        uint64_t inode_id;
        char     name[256];
    } *stack = malloc((size_t)MAX_DEPTH * sizeof(*stack));
    if(!stack) return OBMAFS3_ERR_NOMEM;

    int      depth = 0;
    uint64_t cur   = inode_id;

    const char *prefix      = NULL;
    size_t      prefix_len  = 0;

    while(cur != OBMAFS3_ROOT_INODE_ID && depth < MAX_DEPTH)
    {
        /* Check if this ancestor is cached */
        cached = path_cache_lookup(pc, cur);
        if(cached)
        {
            prefix     = cached;
            prefix_len = strlen(cached);
            break;
        }

        struct catalog_record cat;
        int                   rc = obmafs3_catalog_find_by_inode(ctx, cur, &cat);
        if(rc != OBMAFS3_OK)
        {
            free(stack);
            return rc;
        }
        stack[depth].inode_id = cur;
        memcpy(stack[depth].name, cat.name, 256);
        depth++;
        cur = cat.parent_id;
    }

    /* Build path: prefix (cached ancestor or "/") + stack components top-down */
    size_t pos = 0;
    if(prefix)
    {
        if(prefix_len + 1 > path_buf_size)
        {
            free(stack);
            return OBMAFS3_ERR_NOMEM;
        }
        memcpy(path_buf, prefix, prefix_len);
        pos = prefix_len;
    }

    for(int i = depth - 1; i >= 0; i--)
    {
        size_t nlen = strlen(stack[i].name);
        if(pos + 1 + nlen + 1 > path_buf_size)
        {
            free(stack);
            return OBMAFS3_ERR_NOMEM;
        }
        path_buf[pos++] = '/';
        memcpy(path_buf + pos, stack[i].name, nlen);
        pos += nlen;

        /* Cache this intermediate path for the corresponding inode */
        path_buf[pos] = '\0';
        path_cache_insert(pc, stack[i].inode_id, path_buf);
    }

    path_buf[pos] = '\0';
    free(stack);
    return OBMAFS3_OK;
}

/**
 * Query which files have a specific metadata key/value pair.
 *
 * Searches the reverse-index tree for all inodes matching (@p key,
 * @p value) and resolves each to a full filesystem path.  The caller
 * must free the returned array with @c obmafs3_metadata_query_free.
 *
 * @param ctx    Filesystem context.
 * @param key    Metadata key to search for.
 * @param value  Metadata value to match.
 * @param paths  Output array of path strings.
 * @param count  Output number of matching paths.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_metadata_query(struct obmafs3_ctx *ctx, const char *key, const char *value, char ***paths, uint32_t *count)
{
    *paths = NULL;
    *count = 0;

    if(ctx->sb.metadata_idx_lba == 0) return OBMAFS3_OK;

    uint64_t lba = ctx->metadata_idx_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_OK;

    struct path_cache pc;
    if(path_cache_init(&pc)) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    size_t   nsz = meta_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Traverse to the leaf that would contain (key, value, 0) */
    while(1)
    {
        int rc = meta_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
        }

        if(hdr.level == 0) break;

        uint16_t                        slot = midx_index_find(buf, hdr.node_keys, key, value, 0);
        struct metadata_idx_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain for entries matching (key, value) */
    uint32_t cap    = 16;
    char   **result = malloc(cap * sizeof(char *));
    if(!result)
    {
        free(buf);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    uint32_t n = 0;

    while(1)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct metadata_idx_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            int kcmp = strncmp(rec.key, key, METADATA_KEY_MAX);
            if(kcmp < 0) continue;
            if(kcmp > 0) goto query_done;

            int vcmp = strncmp(rec.value, value, METADATA_VALUE_MAX);
            if(vcmp < 0) continue;
            if(vcmp > 0) goto query_done;

            /* key and value match — resolve inode to path */
            char path[4096];
            int  prc = resolve_inode_path_cached(ctx, rec.inode_id, path, sizeof(path), &pc);
            if(prc != OBMAFS3_OK) continue; /* skip unresolvable inodes */

            if(n >= cap)
            {
                cap *= 2;
                char **tmp = realloc(result, cap * sizeof(char *));
                if(!tmp)
                {
                    for(uint32_t j = 0; j < n; j++) free(result[j]);
                    free(result);
                    free(buf);
                    path_cache_free(&pc);
                    DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                }
                result = tmp;
            }
            result[n] = strdup(path);
            if(!result[n])
            {
                for(uint32_t j = 0; j < n; j++) free(result[j]);
                free(result);
                free(buf);
                path_cache_free(&pc);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }
            n++;
        }

        if(hdr.right_link == 0) break;

        int rc = meta_node_read(ctx, hdr.right_link, buf);
        if(rc != OBMAFS3_OK)
        {
            for(uint32_t j = 0; j < n; j++) free(result[j]);
            free(result);
            free(buf);
            path_cache_free(&pc);
            return rc;
        }
    }

query_done:
    free(buf);
    path_cache_free(&pc);
    *paths = result;
    *count = n;
    return OBMAFS3_OK;
}

/**
 * Free a path array returned by @c obmafs3_metadata_query.
 *
 * @param paths  Array to free (may be NULL).
 * @param count  Number of elements in @p paths.
 */
void obmafs3_metadata_query_free(char **paths, uint32_t count)
{
    if(!paths) return;
    for(uint32_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* ------------------------------------------------------------------ */
/*  Multi-filter metadata query                                        */
/* ------------------------------------------------------------------ */

/** Dynamic set of uint64_t inode IDs (sorted, deduplicated). */
struct inode_id_set
{
    uint64_t *ids;
    uint32_t  count;
    uint32_t  cap;
};

static int idset_init(struct inode_id_set *s, uint32_t initial_cap)
{
    s->count = 0;
    s->cap   = initial_cap ? initial_cap : 16;
    s->ids   = malloc(s->cap * sizeof(uint64_t));
    return s->ids ? 0 : -1;
}

static void idset_free(struct inode_id_set *s)
{
    free(s->ids);
    s->ids   = NULL;
    s->count = 0;
    s->cap   = 0;
}

/** Append an inode_id (duplicates allowed at this stage). */
static int idset_add(struct inode_id_set *s, uint64_t id)
{
    if(s->count >= s->cap)
    {
        uint32_t  nc  = s->cap * 2;
        uint64_t *tmp = realloc(s->ids, nc * sizeof(uint64_t));
        if(!tmp) return -1;
        s->ids = tmp;
        s->cap = nc;
    }
    s->ids[s->count++] = id;
    return 0;
}

static int u64_cmp(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/** Sort and deduplicate in-place. */
static void idset_sort_dedup(struct inode_id_set *s)
{
    if(s->count <= 1) return;
    qsort(s->ids, s->count, sizeof(uint64_t), u64_cmp);
    uint32_t w = 1;
    for(uint32_t r = 1; r < s->count; r++)
        if(s->ids[r] != s->ids[w - 1]) s->ids[w++] = s->ids[r];
    s->count = w;
}

/** Intersect sorted/deduped sets a and b into out. */
static int idset_intersect(const struct inode_id_set *a, const struct inode_id_set *b, struct inode_id_set *out)
{
    if(idset_init(out, (a->count < b->count ? a->count : b->count))) return -1;
    uint32_t i = 0, j = 0;
    while(i < a->count && j < b->count)
    {
        if(a->ids[i] == b->ids[j])
        {
            if(idset_add(out, a->ids[i]))
            {
                idset_free(out);
                return -1;
            }
            i++;
            j++;
        }
        else if(a->ids[i] < b->ids[j])
            i++;
        else
            j++;
    }
    return 0;
}

/** Union sorted/deduped sets a and b into out. */
static int idset_union(const struct inode_id_set *a, const struct inode_id_set *b, struct inode_id_set *out)
{
    if(idset_init(out, a->count + b->count)) return -1;
    uint32_t i = 0, j = 0;
    while(i < a->count && j < b->count)
    {
        if(a->ids[i] == b->ids[j])
        {
            if(idset_add(out, a->ids[i]))
            {
                idset_free(out);
                return -1;
            }
            i++;
            j++;
        }
        else if(a->ids[i] < b->ids[j])
        {
            if(idset_add(out, a->ids[i]))
            {
                idset_free(out);
                return -1;
            }
            i++;
        }
        else
        {
            if(idset_add(out, b->ids[j]))
            {
                idset_free(out);
                return -1;
            }
            j++;
        }
    }
    while(i < a->count)
    {
        if(idset_add(out, a->ids[i++]))
        {
            idset_free(out);
            return -1;
        }
    }
    while(j < b->count)
    {
        if(idset_add(out, b->ids[j++]))
        {
            idset_free(out);
            return -1;
        }
    }
    return 0;
}

/**
 * Portable case-insensitive substring search.
 * Returns a pointer to the first occurrence of @p needle in @p haystack,
 * ignoring case, or NULL if not found.
 */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if(!needle[0]) return haystack;
    for(; *haystack; haystack++)
    {
        const char *h = haystack;
        const char *n = needle;
        while(*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
        {
            h++;
            n++;
        }
        if(!*n) return haystack;
    }
    return NULL;
}

/**
 * Test whether a single reverse-index record matches a filter operator.
 *
 * @param rec_value  The value from the metadata_idx_record.
 * @param flt_value  The value from the filter.
 * @param op         The comparison operator.
 * @return Non-zero if the record matches.
 */
static int filter_value_matches(const char *rec_value, const char *flt_value, uint8_t op)
{
    switch(op)
    {
        case kQueryOpEqual:
            return strncmp(rec_value, flt_value, METADATA_VALUE_MAX) == 0;

        case kQueryOpNotEqual:
            return strncmp(rec_value, flt_value, METADATA_VALUE_MAX) != 0;

        case kQueryOpGreater:
            return strncmp(rec_value, flt_value, METADATA_VALUE_MAX) > 0;

        case kQueryOpLess:
            return strncmp(rec_value, flt_value, METADATA_VALUE_MAX) < 0;

        case kQueryOpGreaterEq:
            return strncmp(rec_value, flt_value, METADATA_VALUE_MAX) >= 0;

        case kQueryOpLessEq:
            return strncmp(rec_value, flt_value, METADATA_VALUE_MAX) <= 0;

        case kQueryOpContains:
            return strstr(rec_value, flt_value) != NULL;

        case kQueryOpStartsWith:
        {
            size_t plen = strnlen(flt_value, METADATA_VALUE_MAX);
            return strncmp(rec_value, flt_value, plen) == 0;
        }

        case kQueryOpExists:
            return 1; /* key exists — always matches */

        /* Case-insensitive variants */
        case kQueryOpIEqual:
            return strncasecmp(rec_value, flt_value, METADATA_VALUE_MAX) == 0;

        case kQueryOpINotEqual:
            return strncasecmp(rec_value, flt_value, METADATA_VALUE_MAX) != 0;

        case kQueryOpIContains:
            return ci_strstr(rec_value, flt_value) != NULL;

        case kQueryOpIStartsWith:
        {
            size_t plen = strnlen(flt_value, METADATA_VALUE_MAX);
            return strncasecmp(rec_value, flt_value, plen) == 0;
        }

        /* Numeric comparisons */
        case kQueryOpNumEqual:
        {
            int64_t rv = strtoll(rec_value, NULL, 10);
            int64_t fv = strtoll(flt_value, NULL, 10);
            return rv == fv;
        }
        case kQueryOpNumNotEqual:
        {
            int64_t rv = strtoll(rec_value, NULL, 10);
            int64_t fv = strtoll(flt_value, NULL, 10);
            return rv != fv;
        }
        case kQueryOpNumGreater:
        {
            int64_t rv = strtoll(rec_value, NULL, 10);
            int64_t fv = strtoll(flt_value, NULL, 10);
            return rv > fv;
        }
        case kQueryOpNumLess:
        {
            int64_t rv = strtoll(rec_value, NULL, 10);
            int64_t fv = strtoll(flt_value, NULL, 10);
            return rv < fv;
        }
        case kQueryOpNumGreaterEq:
        {
            int64_t rv = strtoll(rec_value, NULL, 10);
            int64_t fv = strtoll(flt_value, NULL, 10);
            return rv >= fv;
        }
        case kQueryOpNumLessEq:
        {
            int64_t rv = strtoll(rec_value, NULL, 10);
            int64_t fv = strtoll(flt_value, NULL, 10);
            return rv <= fv;
        }

        /* Regular expression matching */
        case kQueryOpRegex:
        case kQueryOpIRegex:
        {
            int    cflags = REG_EXTENDED | REG_NOSUB;
            if(op == kQueryOpIRegex) cflags |= REG_ICASE;
            regex_t re;
            if(regcomp(&re, flt_value, cflags) != 0) return 0;
            int match = regexec(&re, rec_value, 0, NULL, 0) == 0;
            regfree(&re);
            return match;
        }

        default:
            return 0;
    }
}

/**
 * Collect all inode IDs from the reverse-index tree whose key matches
 * @p filter_key and whose value satisfies @p op against @p filter_value.
 *
 * When the filter key is "*" (wildcard), the entire leaf chain is
 * scanned and every record whose value satisfies the operator is
 * collected regardless of key.
 *
 * Navigates the B+Tree to the first leaf containing @p filter_key and
 * scans the leaf chain until the key changes (or the end of the tree
 * for wildcard queries).
 */
static int midx_collect_matching(struct obmafs3_ctx *ctx, const struct obmafs3_query_filter *flt,
                                 struct inode_id_set *out)
{
    if(ctx->sb.metadata_idx_lba == 0) return OBMAFS3_OK;

    uint64_t lba = ctx->metadata_idx_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_OK;

    /* Wildcard key: "*" matches any key */
    int wildcard = (flt->key[0] == '*' && flt->key[1] == '\0');

    size_t   nsz = meta_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    if(wildcard)
    {
        /* Navigate to the leftmost leaf (slot 0 at every level) */
        while(1)
        {
            int rc = meta_node_read(ctx, lba, buf);
            if(rc != OBMAFS3_OK)
            {
                free(buf);
                return rc;
            }

            struct btree_node_header hdr;
            memcpy(&hdr, buf, sizeof(hdr));
            if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
            {
                free(buf);
                DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
            }
            if(hdr.level == 0) break;

            struct metadata_idx_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header), sizeof(ie));
            lba = ie.child_lba;
        }
    }
    else
    {
        /* Navigate to the leaf containing (key, "", 0) — start of key range */
        static const char empty_val[METADATA_VALUE_MAX] = {0};
        while(1)
        {
            int rc = meta_node_read(ctx, lba, buf);
            if(rc != OBMAFS3_OK)
            {
                free(buf);
                return rc;
            }

            struct btree_node_header hdr;
            memcpy(&hdr, buf, sizeof(hdr));
            if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
            {
                free(buf);
                DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
            }
            if(hdr.level == 0) break;

            uint16_t                        slot = midx_index_find(buf, hdr.node_keys, flt->key, empty_val, 0);
            struct metadata_idx_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
    }

    /* Scan leaf chain */
    while(1)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct metadata_idx_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if(!wildcard)
            {
                int kcmp = strncmp(rec.key, flt->key, METADATA_KEY_MAX);
                if(kcmp < 0) continue;          /* haven't reached the key yet */
                if(kcmp > 0) goto collect_done; /* past the key — done */
            }

            /* Key matches (or wildcard) — apply the operator against the value */
            if(filter_value_matches(rec.value, flt->value, flt->op))
            {
                if(idset_add(out, rec.inode_id))
                {
                    free(buf);
                    DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                }
            }
        }

        if(hdr.right_link == 0) break;
        int rc = meta_node_read(ctx, hdr.right_link, buf);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
    }

collect_done:
    free(buf);
    return OBMAFS3_OK;
}

/**
 * Query which files match a set of metadata filter conditions.
 *
 * Each filter specifies a key, a comparison operator, and a value.
 * Filters are combined with AND (all must match) or OR (any must match).
 * Only the results in the range [@p offset, @p offset + @p limit) are
 * resolved to paths, avoiding expensive catalog lookups for results
 * that the caller will discard.  The total number of matching inodes
 * (before pagination) is written to @p total so the caller can
 * display progress or detect end-of-results.
 *
 * The caller must free the returned array with @c obmafs3_metadata_query_free.
 *
 * @param ctx          Filesystem context.
 * @param filters      Array of filter conditions.
 * @param filter_count Number of filters (1..OBMAFS3_QUERY_MAX_FILTERS).
 * @param combine      kQueryCombineAnd or kQueryCombineOr.
 * @param offset       Number of matching inodes to skip.
 * @param limit        Maximum number of paths to resolve and return.
 *                     Pass 0 to resolve all results (no limit).
 * @param paths        Output array of path strings.
 * @param count        Output number of paths returned this page.
 * @param total        Output total number of matching inodes.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_metadata_query_filtered(struct obmafs3_ctx *ctx, const struct obmafs3_query_filter *filters,
                                    uint8_t filter_count, uint8_t combine, uint32_t offset, uint32_t limit,
                                    char ***paths, uint32_t *count, uint32_t *total)
{
    *paths = NULL;
    *count = 0;
    *total = 0;

    if(filter_count == 0 || filter_count > OBMAFS3_QUERY_MAX_FILTERS) DBG_RETURN(OBMAFS3_ERR_INVAL, "bad filter count");

    /* Collect matching inode IDs for each filter */
    struct inode_id_set sets[OBMAFS3_QUERY_MAX_FILTERS];
    memset(sets, 0, sizeof(sets));

    for(uint8_t f = 0; f < filter_count; f++)
    {
        if(idset_init(&sets[f], 16))
        {
            for(uint8_t j = 0; j < f; j++) idset_free(&sets[j]);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }

        int rc = midx_collect_matching(ctx, &filters[f], &sets[f]);
        if(rc != OBMAFS3_OK)
        {
            for(uint8_t j = 0; j <= f; j++) idset_free(&sets[j]);
            return rc;
        }
        idset_sort_dedup(&sets[f]);
    }

    /* Combine the per-filter ID sets */
    struct inode_id_set result;
    if(filter_count == 1)
    {
        result = sets[0];
        /* Ownership transferred — don't free sets[0] below */
    }
    else
    {
        result = sets[0];
        for(uint8_t f = 1; f < filter_count; f++)
        {
            struct inode_id_set combined;
            int                 rc;
            if(combine == kQueryCombineAnd)
                rc = idset_intersect(&result, &sets[f], &combined);
            else
                rc = idset_union(&result, &sets[f], &combined);

            if(rc != 0)
            {
                idset_free(&result);
                for(uint8_t j = f; j < filter_count; j++) idset_free(&sets[j]);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }

            /* Free the previous result and the consumed set */
            if(f > 1 || filter_count > 1) idset_free(&result);
            idset_free(&sets[f]);
            result = combined;
        }
        /* sets[0] was consumed as the initial result — free it if filter_count > 1 */
        if(filter_count > 1) idset_free(&sets[0]);
    }

    /* Resolve only the inode IDs in [offset, offset+limit) to paths */
    *total = result.count;

    if(offset >= result.count)
    {
        /* Offset past the end — return empty page */
        idset_free(&result);
        *paths = NULL;
        *count = 0;
        return OBMAFS3_OK;
    }

    uint32_t end = (limit > 0 && offset + limit < result.count) ? offset + limit : result.count;
    uint32_t page_max = end - offset;
    uint32_t n   = 0;
    char   **res = malloc(page_max * sizeof(char *));
    if(!res)
    {
        idset_free(&result);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    struct path_cache pc;
    if(path_cache_init(&pc))
    {
        free(res);
        idset_free(&result);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    for(uint32_t i = offset; i < end; i++)
    {
        char path[4096];
        int  prc = resolve_inode_path_cached(ctx, result.ids[i], path, sizeof(path), &pc);
        if(prc != OBMAFS3_OK) continue; /* skip unresolvable inodes */

        res[n] = strdup(path);
        if(!res[n])
        {
            for(uint32_t j = 0; j < n; j++) free(res[j]);
            free(res);
            idset_free(&result);
            path_cache_free(&pc);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }
        n++;
    }

    path_cache_free(&pc);
    idset_free(&result);
    *paths = res;
    *count = n;
    return OBMAFS3_OK;
}