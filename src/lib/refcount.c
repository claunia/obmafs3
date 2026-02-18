/*
 * refcount.c - OBMAFS3 Block Refcount B+Tree operations
 *
 * Refcount tree: proper B+Tree keyed by block LBA.
 *   level 0  → leaf nodes storing sorted refcount_record entries.
 *   level >0 → index nodes storing sorted btree_index_entry entries.
 *   Splits propagate upward; the root grows when it splits.
 *
 * Only blocks with ref_count > 1 are stored.  Absence from the tree
 * means the block has a refcount of 1 (or is unallocated).
 */
#include "btree_internal.h"

/* ------------------------------------------------------------------ */
/*  Refcount B+Tree helpers                                            */
/* ------------------------------------------------------------------ */

/** Maximum refcount_record entries in a leaf node. */
static uint16_t refcount_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct refcount_record));
}

/** Maximum btree_index_entry entries in an index node. */
static uint16_t refcount_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct btree_index_entry));
}

/**
 * Binary search for an LBA in a leaf node buffer.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int refcount_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t lba)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int      mid = lo + (hi - lo) / 2;
        uint64_t mid_lba;
        memcpy(&mid_lba, data + (size_t)mid * sizeof(struct refcount_record), sizeof(mid_lba));
        if(mid_lba == lba) return mid;
        if(mid_lba < lba)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return -(lo + 1);
}

/**
 * Binary search in an index node for the child covering a given LBA.
 * Returns the slot index of the child pointer to follow.
 */
static uint16_t refcount_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t lba)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int      mid = lo + (hi - lo) / 2;
        uint64_t mid_key;
        memcpy(&mid_key, data + (size_t)mid * sizeof(struct btree_index_entry), sizeof(mid_key));
        if(mid_key <= lba)
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

#define REFCOUNT_BTREE_MAX_DEPTH 8

struct refcount_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/* ------------------------------------------------------------------ */
/*  Refcount get (B+Tree traversal)                                    */
/* ------------------------------------------------------------------ */

/**
 * Look up the reference count for a given block LBA.
 *
 * If the LBA has no entry in the refcount tree, the block is assumed
 * to have a reference count of 1.
 *
 * @param ctx        Filesystem context.
 * @param lba        Block LBA to query.
 * @param ref_count  Output reference count.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_refcount_get(struct obmafs3_ctx *ctx, uint64_t lba, uint32_t *ref_count)
{
    uint64_t root = ctx->refcount_hdr.root_node_lba;

    /* Empty tree → everything has refcount 1 */
    if(root == 0)
    {
        *ref_count = 1;
        return OBMAFS3_OK;
    }

    /* Fast path: check the cached refcount leaf node */
    if(ctx->rc_leaf_valid && lba >= ctx->rc_leaf_min && lba <= ctx->rc_leaf_max)
    {
        int idx = refcount_leaf_find(ctx->rc_leaf_buf, ctx->rc_leaf_count, lba);
        if(idx >= 0)
        {
            struct refcount_record rec;
            memcpy(&rec,
                   ctx->rc_leaf_buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(rec),
                   sizeof(rec));
            *ref_count = rec.ref_count;
        }
        else
        {
            *ref_count = 1;
        }
        return OBMAFS3_OK;
    }

    uint8_t *buf = ctx->node_buf;

    uint64_t cur = root;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, cur, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        if(hdr.level > 0)
        {
            uint16_t                 slot = refcount_index_find(buf, hdr.node_keys, lba);
            struct btree_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            cur = ie.child_lba;
        }
        else
        {
            /* Cache this leaf for future lookups */
            memcpy(ctx->rc_leaf_buf, buf, (size_t)ctx->sb.block_size);
            ctx->rc_leaf_lba   = cur;
            ctx->rc_leaf_count = hdr.node_keys;
            if(hdr.node_keys > 0)
            {
                struct refcount_record first_rec, last_rec;
                memcpy(&first_rec, buf + sizeof(struct btree_node_header), sizeof(first_rec));
                memcpy(&last_rec,
                       buf + sizeof(struct btree_node_header) +
                           (size_t)(hdr.node_keys - 1) * sizeof(struct refcount_record),
                       sizeof(last_rec));
                ctx->rc_leaf_min = first_rec.lba;
                ctx->rc_leaf_max = last_rec.lba;
            }
            else
            {
                ctx->rc_leaf_min = UINT64_MAX;
                ctx->rc_leaf_max = 0;
            }
            ctx->rc_leaf_valid = 1;

            int idx = refcount_leaf_find(buf, hdr.node_keys, lba);
            if(idx >= 0)
            {
                struct refcount_record rec;
                memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(rec), sizeof(rec));
                *ref_count = rec.ref_count;
            }
            else
            {
                *ref_count = 1;
            }
            return OBMAFS3_OK;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Refcount set (insert / update / delete in B+Tree)                  */
/* ------------------------------------------------------------------ */

/**
 * Set the reference count for a given block LBA.
 *
 * If @p ref_count is 0 or 1 the record is removed from the tree
 * (refcount 1 is the implicit default).  Otherwise the record is
 * inserted or updated.
 *
 * @param ctx        Filesystem context.
 * @param lba        Block LBA.
 * @param ref_count  New reference count to store.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_refcount_set(struct obmafs3_ctx *ctx, uint64_t lba, uint32_t ref_count)
{
    /* Any mutation invalidates the cached refcount leaf */
    ctx->rc_leaf_valid = 0;

    /* ref_count <= 1 means "remove from tree" (1 is implicit default) */
    if(ref_count <= 1)
    {
        /* If tree is empty, nothing to delete */
        if(ctx->refcount_hdr.root_node_lba == 0) return OBMAFS3_OK;

        /* Find the leaf and delete the record if present */
        size_t   bsz = (size_t)ctx->sb.block_size;
        uint64_t cur = ctx->refcount_hdr.root_node_lba;

        uint8_t *buf = ctx->node_buf;

        while(1)
        {
            int rc = obmafs3_block_read(ctx, cur, buf, bsz);
            if(rc != OBMAFS3_OK) return rc;

            struct btree_node_header hdr;
            memcpy(&hdr, buf, sizeof(hdr));

            if(hdr.level > 0)
            {
                uint16_t                 slot = refcount_index_find(buf, hdr.node_keys, lba);
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
                cur = ie.child_lba;
            }
            else
            {
                int idx = refcount_leaf_find(buf, hdr.node_keys, lba);
                if(idx < 0)
                {
                    /* Not in tree — already at implicit 1 */
                    return OBMAFS3_OK;
                }
                /* Delete the record by shifting remaining entries */
                size_t   rec_sz = sizeof(struct refcount_record);
                uint8_t *data   = buf + sizeof(struct btree_node_header);
                if(idx < hdr.node_keys - 1)
                    memmove(data + (size_t)idx * rec_sz, data + ((size_t)idx + 1) * rec_sz,
                            ((size_t)hdr.node_keys - (size_t)idx - 1) * rec_sz);
                hdr.node_keys--;
                hdr.keys_length = (uint16_t)(hdr.node_keys * rec_sz);
                /* Zero the tail */
                memset(data + (size_t)hdr.node_keys * rec_sz, 0, rec_sz);
                memcpy(buf, &hdr, sizeof(hdr));
                compute_node_checksum(buf);
                rc = obmafs3_block_write(ctx, cur, buf, bsz);
                return rc;
            }
        }
    }

    /* ---- ref_count > 1: insert or update ---- */
    struct refcount_record new_rec;
    new_rec.lba       = lba;
    new_rec.ref_count = ref_count;

    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->refcount_hdr.root_node_lba;
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
        hdr.record_type = kBtreeDataTypeRefcountEntry;
        hdr.level       = 0;
        hdr.node_keys   = 1;
        hdr.keys_length = (uint16_t)sizeof(struct refcount_record);
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), &new_rec, sizeof(new_rec));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        ctx->refcount_hdr.root_node_lba = new_lba;
        ctx->refcount_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.refcount_lba, &ctx->refcount_hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = ctx->node_buf;

    struct refcount_btree_path path[REFCOUNT_BTREE_MAX_DEPTH];
    int                        depth   = 0;
    uint64_t                   cur_lba = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, cur_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        if(hdr.level == 0) break; /* reached leaf; buf holds it at cur_lba */

        if(depth >= REFCOUNT_BTREE_MAX_DEPTH) return OBMAFS3_ERR_INVAL;

        uint16_t slot    = refcount_index_find(buf, hdr.node_keys, lba);
        path[depth].lba  = cur_lba;
        path[depth].slot = slot;
        depth++;

        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        cur_lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at cur_lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for update-in-place */
    int idx = refcount_leaf_find(buf, leaf_hdr.node_keys, lba);
    if(idx >= 0)
    {
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(struct refcount_record), &new_rec,
               sizeof(new_rec));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, cur_lba, buf, bsz);
        return rc;
    }

    /* Not found — insert.  insert_pos is where the new record goes. */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = refcount_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct refcount_record);

    if(leaf_hdr.node_keys < max_leaf)
    {
        /* Room in leaf */
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if(insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz, data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, &new_rec, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, cur_lba, buf, bsz);
        return rc;
    }

    /* ---- Leaf is full: split ---- */
    uint16_t                total = max_leaf + 1;
    struct refcount_record *all   = calloc(total, rec_sz);
    if(!all) return OBMAFS3_ERR_NOMEM;

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    /* Build sorted array of all records including the new one */
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    all[insert_pos] = new_rec;
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
    rc = obmafs3_block_write(ctx, cur_lba, buf, bsz);
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
    nh.record_type = kBtreeDataTypeRefcountEntry;
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

    uint64_t push_key   = all[left_count].lba;
    uint64_t push_child = new_leaf_lba;
    uint64_t left_lba_v = cur_lba;

    free(all);
    ctx->refcount_hdr.total_nodes++;

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

        uint16_t max_idx    = refcount_index_max_keys(ctx);
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
            return obmafs3_btree_header_write(ctx, ctx->sb.refcount_lba, &ctx->refcount_hdr);
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

        uint64_t new_idx_lba;
        rc = obmafs3_alloc_block(ctx, &new_idx_lba);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

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

        memset(buf, 0, bsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeRefcountEntry;
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

        push_key   = aie[il].key;
        push_child = new_idx_lba;
        left_lba_v = parent_lba;

        free(aie);
        ctx->refcount_hdr.total_nodes++;
    }

    /* ---- Need a new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_alloc_block(ctx, &new_root_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Read old root to get its level */
    rc = obmafs3_block_read(ctx, left_lba_v, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    struct btree_node_header old_root_hdr;
    memcpy(&old_root_hdr, buf, sizeof(old_root_hdr));

    memset(buf, 0, bsz);
    struct btree_node_header rhdr;
    memset(&rhdr, 0, sizeof(rhdr));
    rhdr.level       = old_root_hdr.level + 1;

    rhdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rhdr.record_type = kBtreeDataTypeRefcountEntry;
    rhdr.node_keys   = 2;
    rhdr.keys_length = (uint16_t)(2 * sizeof(struct btree_index_entry));
    memcpy(buf, &rhdr, sizeof(rhdr));

    /* First child entry uses the smallest possible key for the left subtree */
    struct btree_index_entry e0;
    e0.key       = 0;
    e0.child_lba = left_lba_v;
    memcpy(buf + sizeof(rhdr), &e0, sizeof(e0));

    struct btree_index_entry e1;
    e1.key       = push_key;
    e1.child_lba = push_child;
    memcpy(buf + sizeof(rhdr) + sizeof(e0), &e1, sizeof(e1));
    compute_node_checksum(buf);

    rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    ctx->refcount_hdr.root_node_lba = new_root_lba;
    ctx->refcount_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.refcount_lba, &ctx->refcount_hdr);
}

/* ------------------------------------------------------------------ */
/*  Convenience helpers                                                */
/* ------------------------------------------------------------------ */

/**
 * Increment the reference count for a block.
 *
 * If the block has no entry in the refcount tree (implicit refcount 1),
 * a new record with refcount 2 is created.
 *
 * @param ctx  Filesystem context.
 * @param lba  Block LBA whose refcount to increment.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_refcount_inc(struct obmafs3_ctx *ctx, uint64_t lba)
{
    uint32_t cur;
    int      rc = obmafs3_refcount_get(ctx, lba, &cur);
    if(rc != OBMAFS3_OK) return rc;
    return obmafs3_refcount_set(ctx, lba, cur + 1);
}

/**
 * Decrement the reference count for a block.
 *
 * If the refcount drops to 1 the record is removed from the tree
 * (1 is the implicit default).  If the refcount is already 1 (or
 * absent) it remains at 1.
 *
 * @param ctx        Filesystem context.
 * @param lba        Block LBA whose refcount to decrement.
 * @param new_count  If non-NULL, receives the new refcount after
 *                   decrement.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_refcount_dec(struct obmafs3_ctx *ctx, uint64_t lba, uint32_t *new_count)
{
    uint32_t cur;
    int      rc = obmafs3_refcount_get(ctx, lba, &cur);
    if(rc != OBMAFS3_OK) return rc;

    if(cur <= 1)
    {
        if(new_count) *new_count = 1;
        return OBMAFS3_OK;
    }

    uint32_t nc = cur - 1;
    rc          = obmafs3_refcount_set(ctx, lba, nc);
    if(rc == OBMAFS3_OK && new_count) *new_count = nc;
    return rc;
}
