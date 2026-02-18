/*
 * inode.c - OBMAFS3 Inode B+Tree operations
 *
 * Inode tree: proper B+Tree (btrfs-style).
 *   level 0  → leaf nodes storing sorted inode_record entries.
 *   level >0 → index nodes storing sorted btree_index_entry entries.
 *   Splits propagate upward; the root grows when it splits.
 */
#include "btree_internal.h"

/* ------------------------------------------------------------------ */
/*  Inode B+Tree helpers                                               */
/* ------------------------------------------------------------------ */

/** Maximum inode_record entries in a leaf node. */
static uint16_t inode_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct inode_record));
}

/** Maximum btree_index_entry entries in an index node. */
static uint16_t inode_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct btree_index_entry));
}

/**
 * Binary search for inode_id in a leaf node buffer.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int inode_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int      mid = lo + (hi - lo) / 2;
        uint64_t mid_id;
        memcpy(&mid_id, data + (size_t)mid * sizeof(struct inode_record), sizeof(mid_id));
        if(mid_id == inode_id) return mid;
        if(mid_id < inode_id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return -(lo + 1);
}

/**
 * Binary search in an index node for the child covering inode_id.
 * Returns the slot index of the child pointer to follow.
 */
static uint16_t inode_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id)
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

#define INODE_BTREE_MAX_DEPTH 8

struct inode_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/* ------------------------------------------------------------------ */
/*  Inode get (B+Tree traversal)                                       */
/* ------------------------------------------------------------------ */

/**
 * Retrieve an inode record by inode ID.
 *
 * Traverses the inode B+Tree from root to leaf to locate the record
 * for the given @p inode_id.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID to look up.
 * @param inode     Output inode record.
 * @return @c OBMAFS3_OK if found, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_inode_get(struct obmafs3_ctx *ctx, uint64_t inode_id, struct inode_record *inode)
{
    uint64_t lba = ctx->inode_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = ctx->node_buf;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        if(hdr.level > 0)
        {
            /* Index node: follow the appropriate child pointer */
            uint16_t                 slot = inode_index_find(buf, hdr.node_keys, inode_id);
            struct btree_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            /* Leaf node: binary search for exact match */
            int idx = inode_leaf_find(buf, hdr.node_keys, inode_id);
            if(idx >= 0)
            {
                struct inode_record rec;
                memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(rec), sizeof(rec));
                memcpy(inode, &rec, sizeof(*inode));
                return OBMAFS3_OK;
            }
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Inode insert / update (B+Tree)                                     */
/* ------------------------------------------------------------------ */

/**
 * Insert or update an inode record in the B+Tree.
 *
 * If a record with the same inode_id already exists it is replaced;
 * otherwise a new record is inserted.  Handles leaf splitting and
 * root promotion when necessary.
 *
 * @param ctx    Filesystem context.
 * @param inode  Pointer to the inode record to store.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_inode_put(struct obmafs3_ctx *ctx, const struct inode_record *inode)
{
    const struct inode_record *rec = inode;

    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    int      rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = obmafs3_alloc_block(ctx, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = ctx->node_buf;
        memset(buf, 0, bsz);

        struct btree_node_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        hdr.record_type = kBtreeDataTypeInode;
        hdr.level       = 0;
        hdr.node_keys   = 1;
        hdr.keys_length = (uint16_t)sizeof(struct inode_record);
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), rec, sizeof(*rec));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        ctx->inode_hdr.root_node_lba = new_lba;
        ctx->inode_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = ctx->node_buf;

    struct inode_btree_path path[INODE_BTREE_MAX_DEPTH];
    int                     depth = 0;
    uint64_t                lba   = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        if(hdr.level == 0) break; /* reached leaf; buf holds it at lba */

        if(depth >= INODE_BTREE_MAX_DEPTH) return OBMAFS3_ERR_INVAL;

        uint16_t slot    = inode_index_find(buf, hdr.node_keys, rec->inode_id);
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

    /* Check for update-in-place */
    int idx = inode_leaf_find(buf, leaf_hdr.node_keys, rec->inode_id);
    if(idx >= 0)
    {
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(struct inode_record), rec, sizeof(*rec));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        return rc;
    }

    /* Not found — insert.  insert_pos is where the new record goes. */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = inode_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct inode_record);

    if(leaf_hdr.node_keys < max_leaf)
    {
        /* Room in leaf */
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
    uint16_t             total = max_leaf + 1;
    struct inode_record *all   = calloc(total, rec_sz);
    if(!all) return OBMAFS3_ERR_NOMEM;

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    /* Build sorted array of all records including the new one */
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    all[insert_pos] = *rec;
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
    nh.record_type = kBtreeDataTypeInode;
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
    ctx->inode_hdr.total_nodes++;

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

        uint16_t max_idx    = inode_index_max_keys(ctx);
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
            return obmafs3_btree_header_write(ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
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
        nih.record_type = kBtreeDataTypeInode;
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
        ctx->inode_hdr.total_nodes++;
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
    rh.record_type = kBtreeDataTypeInode;
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

    ctx->inode_hdr.root_node_lba = new_root_lba;
    ctx->inode_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
}

/**
 * Delete an inode record from the B+Tree.
 *
 * Locates and removes the record for @p inode_id.  Frees empty leaf
 * nodes and updates the tree header.
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode ID to delete.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_inode_delete(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = ctx->node_buf;

    struct inode_btree_path path[INODE_BTREE_MAX_DEPTH];
    int                     depth = 0;
    uint64_t                lba   = root_lba;

    /* Traverse to leaf */
    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        if(hdr.level == 0) break;

        if(depth >= INODE_BTREE_MAX_DEPTH) return OBMAFS3_ERR_INVAL;

        uint16_t slot    = inode_index_find(buf, hdr.node_keys, inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* buf holds the leaf at lba */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = inode_leaf_find(buf, leaf_hdr.node_keys, inode_id);
    if(idx < 0) return OBMAFS3_ERR_NOTFOUND;

    /* Save the record for freeing extent blocks later */
    struct inode_record del_rec;
    size_t              rec_sz = sizeof(struct inode_record);
    memcpy(&del_rec, buf + sizeof(struct btree_node_header) + (size_t)idx * rec_sz, rec_sz);

    /* Remove the record from the leaf */
    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        /* Leaf is now empty */
        if(depth == 0)
        {
            /* Root leaf empty — tree is empty */
            ctx->inode_hdr.root_node_lba = 0;
            ctx->inode_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
            obmafs3_free_block(ctx, lba);
        }
        else
        {
            /* Remove child pointer from parent */
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if(!pbuf) return OBMAFS3_ERR_NOMEM;

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if(rc != OBMAFS3_OK)
            {
                free(pbuf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct btree_index_entry);

            if(pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz, pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if(phdr.node_keys == 0 && depth == 1)
            {
                /* Root index has no children — tree is empty */
                ctx->inode_hdr.root_node_lba = 0;
                ctx->inode_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                /* Root index has one child — collapse tree height */
                struct btree_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->inode_hdr.root_node_lba = remaining.child_lba;
                ctx->inode_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            }
            else
            {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if(rc == OBMAFS3_OK)
                {
                    ctx->inode_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
                }
                obmafs3_free_block(ctx, lba);
            }

            free(pbuf);
        }
    }
    else
    {
        /* Compact remaining entries */
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

    /* Free extent blocks from the deleted inode */
    for(int ei = 0; ei < 8; ei++)
    {
        if(del_rec.extents[ei].block_count > 0)
            obmafs3_free_blocks(ctx, del_rec.extents[ei].start_block, del_rec.extents[ei].block_count);
    }

    return rc;
}
