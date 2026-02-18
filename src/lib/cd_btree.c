/*
 * cd_btree.c - OBMAFS3 CD hash B+Tree operations (prefix/suffix/subchannel)
 *
 * All three trees share identical logic: uint64 hash key, fixed-
 * size inline data, no external blocks.  The generic helpers are
 * parameterised by record_size (sizeof the leaf record).  The first
 * 8 bytes of every leaf record is always the uint64 hash.
 * Index nodes use struct btree_index_entry (hash + child_lba).
 */
#include "btree_internal.h"

/* ------------------------------------------------------------------ */
/*  Generic CD hash B+Tree helpers                                     */
/* ------------------------------------------------------------------ */

/** Maximum leaf records for a given record size. */
static uint16_t cd_hash_leaf_max(const struct obmafs3_ctx *ctx, size_t rec_sz)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / rec_sz);
}

/** Maximum index entries per node (same for all three trees). */
static uint16_t cd_hash_index_max(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct btree_index_entry));
}

/**
 * Binary search for hash in a CD hash leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int cd_hash_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t hash, size_t rec_sz)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int      mid = lo + (hi - lo) / 2;
        uint64_t mid_hash;
        memcpy(&mid_hash, data + (size_t)mid * rec_sz, sizeof(mid_hash));

        if(mid_hash == hash) return mid;
        if(mid_hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return -(lo + 1);
}

/**
 * Binary search in an index node for the child covering hash.
 */
static uint16_t cd_hash_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t hash)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int                      mid = lo + (hi - lo) / 2;
        struct btree_index_entry ie;
        memcpy(&ie, data + (size_t)mid * sizeof(ie), sizeof(ie));

        if(ie.key <= hash)
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

#define CD_HASH_BTREE_MAX_DEPTH 8

struct cd_hash_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/* ---- Lookup ---- */

/**
 * Look up a record in a hash-keyed CD B+Tree.
 *
 * Generic lookup used by CD prefix, suffix, and subchannel trees.
 *
 * @param ctx     Filesystem context.
 * @param hdr     B+Tree header for the target tree.
 * @param hash    Hash key to search for.
 * @param record  Output record buffer.
 * @param rec_sz  Size of each record in bytes.
 * @return @c OBMAFS3_OK if found, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
static int cd_hash_tree_lookup(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash, void *record,
                               size_t rec_sz)
{
    uint64_t lba = hdr->root_node_lba;
    if(lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level > 0)
        {
            uint16_t                 slot = cd_hash_index_find(buf, nhdr.node_keys, hash);
            struct btree_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            int idx = cd_hash_leaf_find(buf, nhdr.node_keys, hash, rec_sz);
            if(idx >= 0)
            {
                memcpy(record, buf + sizeof(struct btree_node_header) + (size_t)idx * rec_sz, rec_sz);
                free(buf);
                return OBMAFS3_OK;
            }
            free(buf);
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ---- Insert / update ---- */

/**
 * Insert or update a record in a hash-keyed CD B+Tree.
 *
 * Generic insert/update used by CD prefix, suffix, and subchannel
 * trees.  Handles leaf splitting and root promotion.
 *
 * @param ctx       Filesystem context.
 * @param hdr       B+Tree header (updated on root changes).
 * @param hdr_lba   LBA of the B+Tree header block.
 * @param record    Pointer to the record to insert.
 * @param rec_sz    Size of each record in bytes.
 * @param data_type B+Tree data type tag for new nodes.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int cd_hash_tree_put(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, const void *record,
                            size_t rec_sz, uint8_t data_type)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = hdr->root_node_lba;
    int      rc;

    uint64_t hash;
    memcpy(&hash, record, sizeof(hash));

    /* ---- Empty tree: create a single leaf as root ---- */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = obmafs3_alloc_block(ctx, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, bsz);
        if(!buf) return OBMAFS3_ERR_NOMEM;

        struct btree_node_header nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nhdr.record_type = data_type;
        nhdr.level       = 0;
        nhdr.node_keys   = 1;
        nhdr.keys_length = (uint16_t)rec_sz;
        memcpy(buf, &nhdr, sizeof(nhdr));
        memcpy(buf + sizeof(nhdr), record, rec_sz);
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        free(buf);
        if(rc != OBMAFS3_OK) return rc;

        hdr->root_node_lba = new_lba;
        hdr->total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
    }

    /* ---- Traverse to leaf, recording path ---- */
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct cd_hash_btree_path path[CD_HASH_BTREE_MAX_DEPTH];
    int                       depth = 0;
    uint64_t                  lba   = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level == 0) break;

        if(depth >= CD_HASH_BTREE_MAX_DEPTH)
        {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot    = cd_hash_index_find(buf, nhdr.node_keys, hash);
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
    int idx = cd_hash_leaf_find(buf, leaf_hdr.node_keys, hash, rec_sz);
    if(idx >= 0)
    {
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)idx * rec_sz, record, rec_sz);
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* Not found — insert */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = cd_hash_leaf_max(ctx, rec_sz);

    if(leaf_hdr.node_keys < max_leaf)
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if(insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz, data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, record, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* ---- Leaf full: split ---- */
    uint16_t total = max_leaf + 1;
    uint8_t *all   = calloc(total, rec_sz);
    if(!all)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(all + (size_t)insert_pos * rec_sz, record, rec_sz);
    memcpy(all + ((size_t)insert_pos + 1) * rec_sz, leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    memset(leaf_data, 0, bsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;

    uint64_t new_leaf_lba;
    rc = obmafs3_alloc_block(ctx, &new_leaf_lba);
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
    rc = obmafs3_block_write(ctx, lba, buf, bsz);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    memset(buf, 0, bsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = data_type;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), all + (size_t)left_count * rec_sz, (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = obmafs3_block_write(ctx, new_leaf_lba, buf, bsz);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        free(buf);
        return rc;
    }

    /* Separator to push upward */
    uint64_t right_first_hash;
    memcpy(&right_first_hash, all + (size_t)left_count * rec_sz, sizeof(right_first_hash));

    struct btree_index_entry push_ie;
    push_ie.key       = right_first_hash;
    push_ie.child_lba = new_leaf_lba;

    uint64_t left_first_hash;
    memcpy(&left_first_hash, all, sizeof(left_first_hash));

    struct btree_index_entry left_ie;
    left_ie.key       = left_first_hash;
    left_ie.child_lba = lba;

    uint64_t left_lba = lba;

    free(all);
    hdr->total_nodes++;

    /* ---- Propagate split upward ---- */
    size_t ie_sz = sizeof(struct btree_index_entry);

    while(depth > 0)
    {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = obmafs3_block_read(ctx, parent_lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = cd_hash_index_max(ctx);
        uint16_t idx_insert = parent_slot + 1;

        if(phdr.node_keys < max_idx)
        {
            uint8_t *id = buf + sizeof(struct btree_node_header);

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz, id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);

            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            free(buf);
            if(rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        }

        /* Parent full — split index node */
        uint16_t                  idx_total = max_idx + 1;
        struct btree_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie)
        {
            free(buf);
            return OBMAFS3_ERR_NOMEM;
        }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

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
            free(buf);
            return rc;
        }

        uint64_t new_idx_lba;
        rc = obmafs3_alloc_block(ctx, &new_idx_lba);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            free(buf);
            return rc;
        }

        memset(buf, 0, bsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = data_type;
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
            free(buf);
            return rc;
        }

        push_ie.key       = aie[il].key;
        push_ie.child_lba = new_idx_lba;

        left_ie.key       = aie[0].key;
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;

        free(aie);
        hdr->total_nodes++;
    }

    /* ---- Create new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_alloc_block(ctx, &new_root_lba);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }

    rc = obmafs3_block_read(ctx, left_lba, buf, bsz);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, bsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = data_type;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * ie_sz);
    memcpy(buf, &rh, sizeof(rh));

    left_ie.child_lba = left_lba;
    struct btree_index_entry roots[2];
    roots[0] = left_ie;
    roots[1] = push_ie;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
    free(buf);
    if(rc != OBMAFS3_OK) return rc;

    hdr->root_node_lba = new_root_lba;
    hdr->total_nodes++;
    return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
}

/* ---- Delete ---- */

/**
 * Delete a record from a hash-keyed CD B+Tree.
 *
 * Generic deletion used by CD prefix, suffix, and subchannel trees.
 * Frees empty leaf nodes and updates the tree header.
 *
 * @param ctx      Filesystem context.
 * @param hdr      B+Tree header (updated on root changes).
 * @param hdr_lba  LBA of the B+Tree header block.
 * @param hash     Hash key of the record to delete.
 * @param rec_sz   Size of each record in bytes.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
static int cd_hash_tree_delete(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, uint64_t hash,
                               size_t rec_sz)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = hdr->root_node_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct cd_hash_btree_path path[CD_HASH_BTREE_MAX_DEPTH];
    int                       depth = 0;
    uint64_t                  lba   = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level == 0) break;

        if(depth >= CD_HASH_BTREE_MAX_DEPTH)
        {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot    = cd_hash_index_find(buf, nhdr.node_keys, hash);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct btree_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = cd_hash_leaf_find(buf, leaf_hdr.node_keys, hash, rec_sz);
    if(idx < 0)
    {
        free(buf);
        return OBMAFS3_ERR_NOTFOUND;
    }

    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        if(depth == 0)
        {
            hdr->root_node_lba = 0;
            hdr->total_nodes--;
            rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
            obmafs3_free_block(ctx, lba);
        }
        else
        {
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if(!pbuf)
            {
                free(buf);
                return OBMAFS3_ERR_NOMEM;
            }

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if(rc != OBMAFS3_OK)
            {
                free(pbuf);
                free(buf);
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
                hdr->root_node_lba = 0;
                hdr->total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                struct btree_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                hdr->root_node_lba = remaining.child_lba;
                hdr->total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
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
                    hdr->total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
                }
                obmafs3_free_block(ctx, lba);
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

    free(buf);
    return rc;
}

/* ================================================================== */
/*  CD prefix/suffix/subchannel public API                             */
/* ================================================================== */

/* ---- CD Prefix ---- */

/**
 * Retrieve CD sector prefix data by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the prefix.
 * @param data  Output 16-byte prefix data.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_NOTFOUND.
 */
int obmafs3_cd_prefix_get(struct obmafs3_ctx *ctx, uint64_t hash, uint8_t data[CD_PREFIX_DATA_SIZE])
{
    if(ctx->sb.cd_prefix_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    struct cd_prefix_record rec;
    int rc = cd_hash_tree_lookup(ctx, &ctx->cd_prefix_hdr, hash, &rec, sizeof(struct cd_prefix_record));
    if(rc != OBMAFS3_OK) return rc;

    memcpy(data, rec.data, CD_PREFIX_DATA_SIZE);
    return OBMAFS3_OK;
}

/**
 * Store CD sector prefix data keyed by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the prefix.
 * @param data  16-byte prefix data to store.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_cd_prefix_put(struct obmafs3_ctx *ctx, uint64_t hash, const uint8_t data[CD_PREFIX_DATA_SIZE])
{
    if(ctx->sb.cd_prefix_lba == 0) return OBMAFS3_ERR_INVAL;

    struct cd_prefix_record rec;
    rec.hash = hash;
    memcpy(rec.data, data, CD_PREFIX_DATA_SIZE);

    return cd_hash_tree_put(ctx, &ctx->cd_prefix_hdr, ctx->sb.cd_prefix_lba, &rec, sizeof(struct cd_prefix_record),
                            kBtreeDataTypeCdPrefixEntry);
}

/**
 * Delete CD sector prefix data by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the prefix to delete.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_NOTFOUND.
 */
int obmafs3_cd_prefix_delete(struct obmafs3_ctx *ctx, uint64_t hash)
{
    if(ctx->sb.cd_prefix_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    return cd_hash_tree_delete(ctx, &ctx->cd_prefix_hdr, ctx->sb.cd_prefix_lba, hash, sizeof(struct cd_prefix_record));
}

/* ---- CD Suffix ---- */

/**
 * Retrieve CD sector suffix (ECC/EDC) data by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the suffix.
 * @param data  Output 288-byte suffix data.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_NOTFOUND.
 */
int obmafs3_cd_suffix_get(struct obmafs3_ctx *ctx, uint64_t hash, uint8_t data[CD_SUFFIX_DATA_SIZE])
{
    if(ctx->sb.cd_suffix_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    struct cd_suffix_record rec;
    int rc = cd_hash_tree_lookup(ctx, &ctx->cd_suffix_hdr, hash, &rec, sizeof(struct cd_suffix_record));
    if(rc != OBMAFS3_OK) return rc;

    memcpy(data, rec.data, CD_SUFFIX_DATA_SIZE);
    return OBMAFS3_OK;
}

/**
 * Store CD sector suffix (ECC/EDC) data keyed by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the suffix.
 * @param data  288-byte suffix data to store.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_cd_suffix_put(struct obmafs3_ctx *ctx, uint64_t hash, const uint8_t data[CD_SUFFIX_DATA_SIZE])
{
    if(ctx->sb.cd_suffix_lba == 0) return OBMAFS3_ERR_INVAL;

    struct cd_suffix_record rec;
    rec.hash = hash;
    memcpy(rec.data, data, CD_SUFFIX_DATA_SIZE);

    return cd_hash_tree_put(ctx, &ctx->cd_suffix_hdr, ctx->sb.cd_suffix_lba, &rec, sizeof(struct cd_suffix_record),
                            kBtreeDataTypeCdSuffixEntry);
}

/**
 * Delete CD sector suffix data by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the suffix to delete.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_NOTFOUND.
 */
int obmafs3_cd_suffix_delete(struct obmafs3_ctx *ctx, uint64_t hash)
{
    if(ctx->sb.cd_suffix_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    return cd_hash_tree_delete(ctx, &ctx->cd_suffix_hdr, ctx->sb.cd_suffix_lba, hash, sizeof(struct cd_suffix_record));
}

/* ---- CD Subchannel ---- */

/**
 * Retrieve CD subchannel data by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the subchannel data.
 * @param data  Output 96-byte subchannel data.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_NOTFOUND.
 */
int obmafs3_cd_subchannel_get(struct obmafs3_ctx *ctx, uint64_t hash, uint8_t data[CD_SUBCHANNEL_DATA_SIZE])
{
    if(ctx->sb.cd_subchannel_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    struct cd_subchannel_record rec;
    int rc = cd_hash_tree_lookup(ctx, &ctx->cd_subchannel_hdr, hash, &rec, sizeof(struct cd_subchannel_record));
    if(rc != OBMAFS3_OK) return rc;

    memcpy(data, rec.data, CD_SUBCHANNEL_DATA_SIZE);
    return OBMAFS3_OK;
}

/**
 * Store CD subchannel data keyed by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the subchannel data.
 * @param data  96-byte subchannel data to store.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_cd_subchannel_put(struct obmafs3_ctx *ctx, uint64_t hash, const uint8_t data[CD_SUBCHANNEL_DATA_SIZE])
{
    if(ctx->sb.cd_subchannel_lba == 0) return OBMAFS3_ERR_INVAL;

    struct cd_subchannel_record rec;
    rec.hash = hash;
    memcpy(rec.data, data, CD_SUBCHANNEL_DATA_SIZE);

    return cd_hash_tree_put(ctx, &ctx->cd_subchannel_hdr, ctx->sb.cd_subchannel_lba, &rec,
                            sizeof(struct cd_subchannel_record), kBtreeDataTypeCdSubchannelEntry);
}

/**
 * Delete CD subchannel data by hash.
 *
 * @param ctx   Filesystem context.
 * @param hash  XXH64 hash of the subchannel data to delete.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_NOTFOUND.
 */
int obmafs3_cd_subchannel_delete(struct obmafs3_ctx *ctx, uint64_t hash)
{
    if(ctx->sb.cd_subchannel_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    return cd_hash_tree_delete(ctx, &ctx->cd_subchannel_hdr, ctx->sb.cd_subchannel_lba, hash,
                               sizeof(struct cd_subchannel_record));
}
