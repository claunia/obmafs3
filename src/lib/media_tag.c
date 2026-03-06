// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : media_tag.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 Media Tag B+Tree operations.
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
 * Composite key: (inode_id, tag_type).  Leaf nodes store
 * media_tag_record entries; index nodes store
 * media_tag_index_entry entries.  Tags <= 512 bytes are stored
 * inline; larger tags use separately allocated data blocks.
 */
#include "btree_internal.h"
#include "debug.h"

/* ------------------------------------------------------------------ */
/*  Media Tag B+Tree helpers                                           */
/* ------------------------------------------------------------------ */

/** Maximum media_tag_record entries in a leaf node. */
static uint16_t media_tag_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct media_tag_record));
}

/** Maximum media_tag_index_entry entries in an index node. */
static uint16_t media_tag_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct media_tag_index_entry));
}

/** Compare composite key (inode_id, tag_type). */
static int media_tag_key_cmp(uint64_t id_a, uint16_t type_a, uint64_t id_b, uint16_t type_b)
{
    if(id_a < id_b) return -1;
    if(id_a > id_b) return 1;
    if(type_a < type_b) return -1;
    if(type_a > type_b) return 1;
    return 0;
}

/**
 * Binary search for (inode_id, tag_type) in a media tag leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int media_tag_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, uint16_t tag_type)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int            mid = lo + (hi - lo) / 2;
        const uint8_t *rec = data + (size_t)mid * sizeof(struct media_tag_record);
        uint64_t       mid_id;
        uint16_t       mid_type;
        memcpy(&mid_id, rec, sizeof(mid_id));
        memcpy(&mid_type, rec + sizeof(mid_id), sizeof(mid_type));

        int cmp = media_tag_key_cmp(mid_id, mid_type, inode_id, tag_type);
        if(cmp == 0) return mid;
        if(cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return -(lo + 1);
}

/**
 * Binary search in a media tag index node for the child covering
 * (inode_id, tag_type).  Returns the slot index of the child to follow.
 */
static uint16_t media_tag_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, uint16_t tag_type)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int            mid = lo + (hi - lo) / 2;
        const uint8_t *ie  = data + (size_t)mid * sizeof(struct media_tag_index_entry);
        uint64_t       mid_id;
        uint16_t       mid_type;
        memcpy(&mid_id, ie, sizeof(mid_id));
        memcpy(&mid_type, ie + sizeof(mid_id), sizeof(mid_type));

        int cmp = media_tag_key_cmp(mid_id, mid_type, inode_id, tag_type);
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

#define MEDIA_TAG_BTREE_MAX_DEPTH 8

struct media_tag_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/* ---- Internal lookup ---- */

/**
 * Look up a media tag record by inode ID and tag type.
 *
 * Traverses the media tag B+Tree using the composite key
 * (@p inode_id, @p tag_type).
 *
 * @param ctx        Filesystem context.
 * @param inode_id   Inode ID of the media image.
 * @param tag_type   Media tag type enumeration value.
 * @param record     Output media tag record.
 * @return @c OBMAFS3_OK if found, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
static int media_tag_tree_lookup(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type,
                                 struct media_tag_record *record)
{
    uint64_t lba = ctx->media_tag_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(hdr.level > 0)
        {
            uint16_t                     slot = media_tag_index_find(buf, hdr.node_keys, inode_id, tag_type);
            struct media_tag_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            int idx = media_tag_leaf_find(buf, hdr.node_keys, inode_id, tag_type);
            if(idx >= 0)
            {
                memcpy(record, buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(*record), sizeof(*record));
                return OBMAFS3_OK;
            }
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ---- External data helpers ---- */

/** Free external data blocks referenced by a media tag record. */
static void media_tag_free_external(struct obmafs3_ctx *ctx, const struct media_tag_record *rec)
{
    if(!(rec->flags & MEDIA_TAG_FLAG_INLINE) && rec->data_lba != 0 && rec->data_blocks != 0)
    {
        obmafs3_free_blocks(ctx, rec->data_lba, rec->data_blocks);
    }
}

/** Allocate contiguous blocks and write external tag data. */
static int media_tag_write_external(struct obmafs3_ctx *ctx, const void *data, uint32_t data_length, uint64_t *out_lba,
                                    uint64_t *out_nblocks)
{
    uint64_t bsz    = ctx->sb.block_size;
    uint64_t blocks = ((uint64_t)data_length + bsz - 1) / bsz;
    uint64_t start_lba;

    int rc = obmafs3_alloc_blocks(ctx, blocks, &start_lba);
    if(rc != OBMAFS3_OK) return rc;

    uint8_t *block_buf = calloc(1, (size_t)bsz);
    if(!block_buf)
    {
        obmafs3_free_blocks(ctx, start_lba, blocks);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    const uint8_t *src       = (const uint8_t *)data;
    uint32_t       remaining = data_length;

    for(uint64_t i = 0; i < blocks; i++)
    {
        memset(block_buf, 0, (size_t)bsz);
        size_t copy = remaining < bsz ? remaining : (size_t)bsz;
        memcpy(block_buf, src, copy);

        rc = obmafs3_block_write(ctx, start_lba + i, block_buf, (size_t)bsz);
        if(rc != OBMAFS3_OK)
        {
            free(block_buf);
            obmafs3_free_blocks(ctx, start_lba, blocks);
            return rc;
        }
        src += copy;
        remaining -= (uint32_t)copy;
    }

    free(block_buf);
    *out_lba     = start_lba;
    *out_nblocks = blocks;
    return OBMAFS3_OK;
}

/**
 * Build a media_tag_record from input data.
 * If data fits inline, stores it inline.
 * Otherwise allocates external blocks and writes the data.
 */
static int media_tag_build_record(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type, const void *data,
                                  uint32_t data_length, struct media_tag_record *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->inode_id    = inode_id;
    rec->tag_type    = tag_type;
    rec->data_length = data_length;

    if(data_length <= MEDIA_TAG_INLINE_MAX)
    {
        rec->flags       = MEDIA_TAG_FLAG_INLINE;
        rec->data_lba    = 0;
        rec->data_blocks = 0;
        if(data_length > 0) memcpy(rec->inline_data, data, data_length);
    }
    else
    {
        rec->flags = 0;
        uint64_t ext_lba, ext_blocks;
        int      rc = media_tag_write_external(ctx, data, data_length, &ext_lba, &ext_blocks);
        if(rc != OBMAFS3_OK) return rc;
        rec->data_lba    = ext_lba;
        rec->data_blocks = ext_blocks;
    }

    return OBMAFS3_OK;
}

/* ---- Insert or update a media tag record in the B+Tree ---- */

/**
 * Insert or update a media tag record in the B+Tree.
 *
 * Handles leaf splitting and root promotion when the target leaf is
 * full.  If a record with the same key already exists it is replaced.
 *
 * @param ctx  Filesystem context.
 * @param rec  Pointer to the media tag record to insert or update.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int media_tag_tree_put(struct obmafs3_ctx *ctx, const struct media_tag_record *rec)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->media_tag_hdr.root_node_lba;
    int      rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = obmafs3_btree_alloc_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
        memset(buf, 0, bsz);

        struct btree_node_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        hdr.record_type = kBtreeDataTypeMediaTagEntry;
        hdr.level       = 0;
        hdr.node_keys   = 1;
        hdr.keys_length = (uint16_t)sizeof(struct media_tag_record);
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), rec, sizeof(*rec));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        ctx->media_tag_hdr.root_node_lba = new_lba;
        ctx->media_tag_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct media_tag_btree_path path[MEDIA_TAG_BTREE_MAX_DEPTH];
    int                         depth = 0;
    uint64_t                    lba   = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(hdr.level == 0) break;

        if(depth >= MEDIA_TAG_BTREE_MAX_DEPTH) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

        uint16_t slot    = media_tag_index_find(buf, hdr.node_keys, rec->inode_id, rec->tag_type);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct media_tag_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for update-in-place */
    int idx = media_tag_leaf_find(buf, leaf_hdr.node_keys, rec->inode_id, rec->tag_type);
    if(idx >= 0)
    {
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(struct media_tag_record), rec,
               sizeof(*rec));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        return rc;
    }

    /* Not found — insert.  insert_pos is where the new record goes. */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = media_tag_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct media_tag_record);

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

        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        return rc;
    }

    /* ---- Leaf is full: split ---- */
    uint16_t                 total = max_leaf + 1;
    struct media_tag_record *all   = calloc(total, rec_sz);
    if(!all) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(&all[insert_pos], rec, rec_sz);
    memcpy(&all[insert_pos + 1], leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    memset(leaf_data, 0, bsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;

    uint64_t new_leaf_lba;
    rc = obmafs3_btree_alloc_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, &new_leaf_lba);
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

    memset(buf, 0, bsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeMediaTagEntry;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.left_link   = lba;
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

    struct media_tag_index_entry push_ie;
    push_ie.inode_id  = all[left_count].inode_id;
    push_ie.tag_type  = all[left_count].tag_type;
    push_ie.child_lba = new_leaf_lba;

    struct media_tag_index_entry left_ie;
    left_ie.inode_id  = all[0].inode_id;
    left_ie.tag_type  = all[0].tag_type;
    left_ie.child_lba = lba;

    uint64_t left_lba = lba;

    free(all);
    ctx->media_tag_hdr.total_nodes++;

    /* Update old right neighbor's left_link */
    if(old_right != 0)
    {
        rc = obmafs3_block_read(ctx, old_right, buf, bsz);
        if(rc == OBMAFS3_OK)
        {
            struct btree_node_header rnh;
            memcpy(&rnh, buf, sizeof(rnh));
            rnh.left_link = new_leaf_lba;
            memcpy(buf, &rnh, sizeof(rnh));
            compute_node_checksum(buf);
            obmafs3_block_write(ctx, old_right, buf, bsz);
        }
    }

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

        uint16_t max_idx    = media_tag_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct media_tag_index_entry);

        if(phdr.node_keys < max_idx)
        {
            uint8_t *id = buf + sizeof(struct btree_node_header);

            /* Update the key at parent_slot to the left child's
             * actual minimum.  Without this, the parent key can
             * be stale (higher than the true minimum) after the
             * leftmost child accumulated entries with keys below
             * the original index key. */
            struct media_tag_index_entry mt_upd;
            memcpy(&mt_upd, id + (size_t)parent_slot * ie_sz, sizeof(mt_upd));
            mt_upd.inode_id = left_ie.inode_id;
            mt_upd.tag_type = left_ie.tag_type;
            memcpy(id + (size_t)parent_slot * ie_sz, &mt_upd, sizeof(mt_upd));

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz, id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);

            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            if(rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
        }

        /* Parent is full — split the index node */
        uint16_t                      idx_total = max_idx + 1;
        struct media_tag_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        uint8_t *id = buf + sizeof(struct btree_node_header);

        /* Update the key at parent_slot to the left child's
         * actual minimum before building the merged array. */
        struct media_tag_index_entry mt_upd;
        memcpy(&mt_upd, id + (size_t)parent_slot * ie_sz, sizeof(mt_upd));
        mt_upd.inode_id = left_ie.inode_id;
        mt_upd.tag_type = left_ie.tag_type;
        memcpy(id + (size_t)parent_slot * ie_sz, &mt_upd, sizeof(mt_upd));

        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        /* Allocate new index node before writing so we can set sibling links */
        uint64_t idx_old_right = phdr.right_link;
        uint64_t new_idx_lba;
        rc = obmafs3_btree_alloc_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, &new_idx_lba);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        memset(id, 0, bsz - sizeof(struct btree_node_header));
        memcpy(id, aie, (size_t)il * ie_sz);
        phdr.node_keys   = il;
        phdr.keys_length = (uint16_t)(il * ie_sz);
        phdr.right_link  = new_idx_lba;
        memcpy(buf, &phdr, sizeof(phdr));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        memset(buf, 0, bsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeMediaTagEntry;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        nih.left_link   = parent_lba;
        nih.right_link  = idx_old_right;
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, new_idx_lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        push_ie.inode_id  = aie[il].inode_id;
        push_ie.tag_type  = aie[il].tag_type;
        push_ie.child_lba = new_idx_lba;

        left_ie.inode_id  = aie[0].inode_id;
        left_ie.tag_type  = aie[0].tag_type;
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;

        free(aie);
        ctx->media_tag_hdr.total_nodes++;

        /* Update old right neighbor's left_link */
        if(idx_old_right != 0)
        {
            rc = obmafs3_block_read(ctx, idx_old_right, buf, bsz);
            if(rc == OBMAFS3_OK)
            {
                struct btree_node_header rnh;
                memcpy(&rnh, buf, sizeof(rnh));
                rnh.left_link = new_idx_lba;
                memcpy(buf, &rnh, sizeof(rnh));
                compute_node_checksum(buf);
                obmafs3_block_write(ctx, idx_old_right, buf, bsz);
            }
        }
    }

    /* ---- Create new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_btree_alloc_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, &new_root_lba);
    if(rc != OBMAFS3_OK) return rc;

    rc = obmafs3_block_read(ctx, left_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, bsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeMediaTagEntry;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * sizeof(struct media_tag_index_entry));
    memcpy(buf, &rh, sizeof(rh));

    left_ie.child_lba = left_lba;
    struct media_tag_index_entry roots[2];
    roots[0] = left_ie;
    roots[1] = push_ie;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    ctx->media_tag_hdr.root_node_lba = new_root_lba;
    ctx->media_tag_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
}

/* ---- Delete a media tag record from the B+Tree ---- */

/**
 * Delete a media tag record from the B+Tree.
 *
 * Frees any external data blocks associated with the tag and removes
 * the record.  Empty leaf nodes are freed and the tree header is updated.
 *
 * @param ctx        Filesystem context.
 * @param inode_id   Inode ID of the media image.
 * @param tag_type   Media tag type to delete.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
static int media_tag_tree_delete(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->media_tag_hdr.root_node_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct media_tag_btree_path path[MEDIA_TAG_BTREE_MAX_DEPTH];
    int                         depth = 0;
    uint64_t                    lba   = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(hdr.level == 0) break;

        if(depth >= MEDIA_TAG_BTREE_MAX_DEPTH) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

        uint16_t slot    = media_tag_index_find(buf, hdr.node_keys, inode_id, tag_type);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct media_tag_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = media_tag_leaf_find(buf, leaf_hdr.node_keys, inode_id, tag_type);
    if(idx < 0) return OBMAFS3_ERR_NOTFOUND;

    struct media_tag_record del_rec;
    size_t                  rec_sz = sizeof(struct media_tag_record);
    memcpy(&del_rec, buf + sizeof(struct btree_node_header) + (size_t)idx * rec_sz, rec_sz);

    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        if(depth == 0)
        {
            ctx->media_tag_hdr.root_node_lba = 0;
            ctx->media_tag_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
            obmafs3_btree_free_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, lba);
        }
        else
        {
            /* Update sibling links around freed leaf */
            if(leaf_hdr.left_link != 0)
            {
                rc = obmafs3_block_read(ctx, leaf_hdr.left_link, buf, bsz);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header lnh;
                    memcpy(&lnh, buf, sizeof(lnh));
                    lnh.right_link = leaf_hdr.right_link;
                    memcpy(buf, &lnh, sizeof(lnh));
                    compute_node_checksum(buf);
                    obmafs3_block_write(ctx, leaf_hdr.left_link, buf, bsz);
                }
            }
            if(leaf_hdr.right_link != 0)
            {
                rc = obmafs3_block_read(ctx, leaf_hdr.right_link, buf, bsz);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header rnh;
                    memcpy(&rnh, buf, sizeof(rnh));
                    rnh.left_link = leaf_hdr.left_link;
                    memcpy(buf, &rnh, sizeof(rnh));
                    compute_node_checksum(buf);
                    obmafs3_block_write(ctx, leaf_hdr.right_link, buf, bsz);
                }
            }

            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if(!pbuf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if(rc != OBMAFS3_OK)
            {
                free(pbuf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct media_tag_index_entry);

            if(pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz, pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if(phdr.node_keys == 0 && depth == 1)
            {
                ctx->media_tag_hdr.root_node_lba = 0;
                ctx->media_tag_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
                obmafs3_btree_free_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, lba);
                obmafs3_btree_free_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                struct media_tag_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->media_tag_hdr.root_node_lba = remaining.child_lba;
                ctx->media_tag_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
                obmafs3_btree_free_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, lba);
                obmafs3_btree_free_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, plba);
            }
            else
            {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if(rc == OBMAFS3_OK)
                {
                    ctx->media_tag_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
                }
                obmafs3_btree_free_node(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, lba);
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
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
    }

    media_tag_free_external(ctx, &del_rec);

    return rc;
}

/* ================================================================== */
/*  Media Tag public API                                               */
/* ================================================================== */

/**
 * Retrieve media tag data for a given inode and tag type.
 *
 * Looks up the tag record; if the data is inline it is copied directly,
 * otherwise the external data blocks are read.  The caller must free
 * the returned buffer with @c obmafs3_media_tag_data_free.
 *
 * @param ctx          Filesystem context.
 * @param inode_id     Inode ID of the media image.
 * @param tag_type     Media tag type to retrieve.
 * @param data         Output pointer to the allocated data buffer.
 * @param data_length  Output length of the data in bytes.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_media_tag_get(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type, void **data,
                          uint32_t *data_length)
{
    if(ctx->sb.media_tag_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    struct media_tag_record rec;
    int                     rc = media_tag_tree_lookup(ctx, inode_id, tag_type, &rec);
    if(rc != OBMAFS3_OK) return rc;

    uint8_t *buf = malloc(rec.data_length);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    if(rec.flags & MEDIA_TAG_FLAG_INLINE) { memcpy(buf, rec.inline_data, rec.data_length); }
    else
    {
        size_t   bsz       = (size_t)ctx->sb.block_size;
        uint8_t *block_buf = calloc(1, bsz);
        if(!block_buf)
        {
            free(buf);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }

        uint32_t remaining = rec.data_length;
        uint32_t offset    = 0;

        for(uint64_t i = 0; i < rec.data_blocks && remaining > 0; i++)
        {
            rc = obmafs3_block_read(ctx, rec.data_lba + i, block_buf, bsz);
            if(rc != OBMAFS3_OK)
            {
                free(block_buf);
                free(buf);
                return rc;
            }
            size_t copy = remaining < bsz ? remaining : bsz;
            memcpy(buf + offset, block_buf, copy);
            offset += (uint32_t)copy;
            remaining -= (uint32_t)copy;
        }

        free(block_buf);
    }

    *data        = buf;
    *data_length = rec.data_length;
    return OBMAFS3_OK;
}

/**
 * Free a media tag data buffer returned by @c obmafs3_media_tag_get.
 *
 * @param data  Data buffer to free (may be NULL).
 */
void obmafs3_media_tag_data_free(void *data) { free(data); }

/**
 * Store or update a media tag for a given inode.
 *
 * If a tag with the same type already exists its external data is freed
 * before storing the new data.  Small tags are stored inline; larger
 * tags use external blocks.
 *
 * @param ctx          Filesystem context.
 * @param inode_id     Inode ID of the media image.
 * @param tag_type     Media tag type to store.
 * @param data         Tag data buffer.
 * @param data_length  Length of @p data in bytes.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_media_tag_put(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type, const void *data,
                          uint32_t data_length)
{
    if(ctx->sb.media_tag_lba == 0) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

    /* If the record already exists, free old external data first */
    struct media_tag_record old_rec;
    int                     rc = media_tag_tree_lookup(ctx, inode_id, tag_type, &old_rec);
    if(rc == OBMAFS3_OK)
        media_tag_free_external(ctx, &old_rec);
    else if(rc != OBMAFS3_ERR_NOTFOUND)
        return rc;

    struct media_tag_record new_rec;
    rc = media_tag_build_record(ctx, inode_id, tag_type, data, data_length, &new_rec);
    if(rc != OBMAFS3_OK) return rc;

    return media_tag_tree_put(ctx, &new_rec);
}

/**
 * Delete a single media tag for a given inode.
 *
 * @param ctx        Filesystem context.
 * @param inode_id   Inode ID of the media image.
 * @param tag_type   Media tag type to delete.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_media_tag_delete(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type)
{
    if(ctx->sb.media_tag_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    return media_tag_tree_delete(ctx, inode_id, tag_type);
}

/**
 * Delete all media tags for a given inode.
 *
 * Repeatedly lists and deletes tags until none remain.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID of the media image.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_media_tag_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    if(ctx->sb.media_tag_lba == 0) return OBMAFS3_OK;

    /* Repeatedly list + delete until no tags remain. */
    while(1)
    {
        uint16_t *types;
        uint32_t  count;
        int       rc = obmafs3_media_tag_list(ctx, inode_id, &types, &count);
        if(rc != OBMAFS3_OK) return rc;
        if(count == 0)
        {
            obmafs3_media_tag_list_free(types);
            return OBMAFS3_OK;
        }

        uint16_t first_type = types[0];
        obmafs3_media_tag_list_free(types);

        rc = media_tag_tree_delete(ctx, inode_id, first_type);
        if(rc != OBMAFS3_OK) return rc;
    }
}

/**
 * List all media tag types stored for a given inode.
 *
 * Traverses the media tag B+Tree leaf chain and collects all tag type
 * values associated with @p inode_id.  The caller must free the
 * returned array with @c obmafs3_media_tag_list_free.
 *
 * @param ctx        Filesystem context.
 * @param inode_id   Inode ID of the media image.
 * @param tag_types  Output array of tag type values.
 * @param count      Output number of tag types.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_media_tag_list(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t **tag_types, uint32_t *count)
{
    *tag_types = NULL;
    *count     = 0;

    if(ctx->sb.media_tag_lba == 0) return OBMAFS3_OK;

    uint64_t lba = ctx->media_tag_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_OK;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Traverse to the leaf that would contain (inode_id, 0) */
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
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

        uint16_t                     slot = media_tag_index_find(buf, hdr.node_keys, inode_id, 0);
        struct media_tag_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain for entries with matching inode_id */
    uint32_t  cap   = 16;
    uint16_t *types = malloc(cap * sizeof(uint16_t));
    if(!types)
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
            struct media_tag_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if(rec.inode_id == inode_id)
            {
                if(n >= cap)
                {
                    cap *= 2;
                    uint16_t *tmp = realloc(types, cap * sizeof(uint16_t));
                    if(!tmp)
                    {
                        free(types);
                        free(buf);
                        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                    }
                    types = tmp;
                }
                types[n++] = rec.tag_type;
            }
            else if(rec.inode_id > inode_id) { goto scan_done; }
        }

        if(hdr.right_link == 0) break;

        int rc = obmafs3_block_read(ctx, hdr.right_link, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(types);
            free(buf);
            return rc;
        }
    }

scan_done:
    free(buf);
    *tag_types = types;
    *count     = n;
    return OBMAFS3_OK;
}

/**
 * Free a tag type array returned by @c obmafs3_media_tag_list.
 *
 * @param tag_types  Array to free (may be NULL).
 */
void obmafs3_media_tag_list_free(uint16_t *tag_types) { free(tag_types); }
