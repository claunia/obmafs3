// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : metadata_numeric.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Numeric metadata index B+Tree operations.
//     Sorted by (key, int64_t value, inode_id) for efficient numeric range
//     queries on metadata fields like year, track number, etc.
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

#include "btree_internal.h"
#include "debug.h"

#define NUMIDX_BTREE_MAX_DEPTH 16

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

struct numidx_path
{
    uint64_t lba;
    uint16_t slot;
};

static size_t numidx_node_size(const struct obmafs3_ctx *ctx)
{
    return (size_t)METADATA_NODE_BLOCKS * (size_t)ctx->sb.block_size;
}

static int numidx_node_read(struct obmafs3_ctx *ctx, uint64_t lba, uint8_t *buf)
{
    return obmafs3_block_read(ctx, lba, buf, numidx_node_size(ctx));
}

static int numidx_node_write(struct obmafs3_ctx *ctx, uint64_t lba, const uint8_t *buf)
{
    return obmafs3_block_write(ctx, lba, buf, numidx_node_size(ctx));
}

static int numidx_alloc_node(struct obmafs3_ctx *ctx, uint64_t *lba)
{
    return obmafs3_btree_alloc_node(ctx, &ctx->metadata_numeric_idx_hdr, ctx->sb.metadata_numeric_idx_lba, lba);
}

static void numidx_free_node(struct obmafs3_ctx *ctx, uint64_t lba)
{
    obmafs3_btree_free_node(ctx, &ctx->metadata_numeric_idx_hdr, ctx->sb.metadata_numeric_idx_lba, lba);
}

static uint16_t numidx_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((numidx_node_size(ctx) - sizeof(struct btree_node_header)) /
                      sizeof(struct metadata_numeric_idx_record));
}

static uint16_t numidx_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((numidx_node_size(ctx) - sizeof(struct btree_node_header)) /
                      sizeof(struct metadata_numeric_idx_index_entry));
}

/* ------------------------------------------------------------------ */
/*  Composite key comparator (key string, numeric value, inode_id)     */
/* ------------------------------------------------------------------ */

static int numidx_key_cmp(const char *key_a, int64_t val_a, uint64_t id_a,
                          const char *key_b, int64_t val_b, uint64_t id_b)
{
    int c = strncmp(key_a, key_b, METADATA_KEY_MAX);
    if(c != 0) return c;
    if(val_a < val_b) return -1;
    if(val_a > val_b) return 1;
    if(id_a < id_b) return -1;
    if(id_a > id_b) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Binary search                                                      */
/* ------------------------------------------------------------------ */

/** Binary search in a leaf node.  Returns index >= 0 if found,
 *  or -(insertion_point + 1) if not found. */
static int numidx_leaf_find(const uint8_t *buf, uint16_t node_keys,
                            const char *key, int64_t value, uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const struct metadata_numeric_idx_record *rec =
            (const struct metadata_numeric_idx_record *)(data + (size_t)mid * sizeof(struct metadata_numeric_idx_record));

        int cmp = numidx_key_cmp(rec->key, rec->value, rec->inode_id, key, value, inode_id);
        if(cmp == 0) return mid;
        if(cmp < 0) lo = mid + 1;
        else         hi = mid - 1;
    }
    return -(lo + 1);
}

/** Binary search in an index node.  Returns the slot to descend into. */
static uint16_t numidx_index_find(const uint8_t *buf, uint16_t node_keys,
                                  const char *key, int64_t value, uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;
    uint16_t result = 0;

    while(lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const struct metadata_numeric_idx_index_entry *ie =
            (const struct metadata_numeric_idx_index_entry *)(data + (size_t)mid * sizeof(struct metadata_numeric_idx_index_entry));

        int cmp = numidx_key_cmp(ie->key, ie->value, ie->inode_id, key, value, inode_id);
        if(cmp <= 0) { result = (uint16_t)mid; lo = mid + 1; }
        else         { hi = mid - 1; }
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  Header write helper                                                */
/* ------------------------------------------------------------------ */

static int numidx_hdr_write(struct obmafs3_ctx *ctx)
{
    return obmafs3_btree_header_write(ctx, ctx->sb.metadata_numeric_idx_lba, &ctx->metadata_numeric_idx_hdr);
}

/* ------------------------------------------------------------------ */
/*  Insert with split                                                  */
/* ------------------------------------------------------------------ */

int obmafs3_numidx_put(struct obmafs3_ctx *ctx, const char *key, int64_t value, uint64_t inode_id)
{
    size_t   nsz      = numidx_node_size(ctx);
    uint64_t root_lba = ctx->metadata_numeric_idx_hdr.root_node_lba;
    int      rc;

    /* Empty tree: create first leaf */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = numidx_alloc_node(ctx, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, nsz);
        if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        struct btree_node_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        hdr.record_type = kBtreeDataTypeMetadataNumericIndexEntry;
        hdr.level       = 0;
        hdr.node_keys   = 1;
        hdr.keys_length = (uint16_t)sizeof(struct metadata_numeric_idx_record);

        struct metadata_numeric_idx_record rec;
        memset(&rec, 0, sizeof(rec));
        strncpy(rec.key, key, METADATA_KEY_MAX - 1);
        rec.value    = value;
        rec.inode_id = inode_id;

        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), &rec, sizeof(rec));
        compute_node_checksum(buf);

        rc = numidx_node_write(ctx, new_lba, buf);
        free(buf);
        if(rc != OBMAFS3_OK) return rc;

        ctx->metadata_numeric_idx_hdr.root_node_lba = new_lba;
        ctx->metadata_numeric_idx_hdr.total_nodes   = 1;
        return numidx_hdr_write(ctx);
    }

    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Traverse to leaf */
    struct numidx_path path[NUMIDX_BTREE_MAX_DEPTH];
    int                depth = 0;
    uint64_t           lba   = root_lba;

    while(1)
    {
        rc = numidx_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic"); }
        if(hdr.level == 0) break;
        if(depth >= NUMIDX_BTREE_MAX_DEPTH) { free(buf); DBG_RETURN(OBMAFS3_ERR_INVAL, "tree too deep"); }

        uint16_t slot    = numidx_index_find(buf, hdr.node_keys, key, value, inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_numeric_idx_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for duplicate */
    int idx = numidx_leaf_find(buf, leaf_hdr.node_keys, key, value, inode_id);
    if(idx >= 0) { free(buf); return OBMAFS3_OK; } /* already exists */

    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = numidx_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct metadata_numeric_idx_record);

    /* Non-full leaf: insert in place */
    if(leaf_hdr.node_keys < max_leaf)
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if(insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz,
                    data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)insert_pos) * rec_sz);

        struct metadata_numeric_idx_record rec;
        memset(&rec, 0, sizeof(rec));
        strncpy(rec.key, key, METADATA_KEY_MAX - 1);
        rec.value    = value;
        rec.inode_id = inode_id;
        memcpy(data + (size_t)insert_pos * rec_sz, &rec, rec_sz);

        leaf_hdr.node_keys++;
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = numidx_node_write(ctx, lba, buf);
        free(buf);
        return rc;
    }

    /* Leaf full — split */
    uint16_t total = max_leaf + 1;
    struct metadata_numeric_idx_record *all = calloc(total, rec_sz);
    if(!all) { free(buf); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);

    struct metadata_numeric_idx_record new_rec;
    memset(&new_rec, 0, sizeof(new_rec));
    strncpy(new_rec.key, key, METADATA_KEY_MAX - 1);
    new_rec.value    = value;
    new_rec.inode_id = inode_id;
    memcpy(&all[insert_pos], &new_rec, rec_sz);

    memcpy(&all[insert_pos + 1], leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    /* Rewrite left leaf */
    memset(leaf_data, 0, nsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;
    uint64_t new_leaf_lba;
    rc = numidx_alloc_node(ctx, &new_leaf_lba);
    if(rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    leaf_hdr.node_keys   = left_count;
    leaf_hdr.keys_length = (uint16_t)(left_count * rec_sz);
    leaf_hdr.right_link  = new_leaf_lba;
    memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
    compute_node_checksum(buf);
    rc = numidx_node_write(ctx, lba, buf);
    if(rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    /* Write right leaf */
    memset(buf, 0, nsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeMetadataNumericIndexEntry;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.left_link   = lba;
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), &all[left_count], (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = numidx_node_write(ctx, new_leaf_lba, buf);
    if(rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    /* Build separator for parent */
    struct metadata_numeric_idx_index_entry push_ie;
    memset(&push_ie, 0, sizeof(push_ie));
    strncpy(push_ie.key, all[left_count].key, METADATA_KEY_MAX);
    push_ie.value     = all[left_count].value;
    push_ie.inode_id  = all[left_count].inode_id;
    push_ie.child_lba = new_leaf_lba;

    struct metadata_numeric_idx_index_entry left_ie;
    memset(&left_ie, 0, sizeof(left_ie));
    strncpy(left_ie.key, all[0].key, METADATA_KEY_MAX);
    left_ie.value     = all[0].value;
    left_ie.inode_id  = all[0].inode_id;
    left_ie.child_lba = lba;

    uint64_t left_lba = lba;
    free(all);
    ctx->metadata_numeric_idx_hdr.total_nodes++;

    /* Update old right neighbor's left_link */
    if(old_right != 0)
    {
        rc = numidx_node_read(ctx, old_right, buf);
        if(rc == OBMAFS3_OK)
        {
            struct btree_node_header rnh;
            memcpy(&rnh, buf, sizeof(rnh));
            rnh.left_link = new_leaf_lba;
            memcpy(buf, &rnh, sizeof(rnh));
            compute_node_checksum(buf);
            numidx_node_write(ctx, old_right, buf);
        }
    }

    /* Propagate split upward */
    size_t ie_sz = sizeof(struct metadata_numeric_idx_index_entry);

    while(depth > 0)
    {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = numidx_node_read(ctx, parent_lba, buf);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = numidx_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;

        if(phdr.node_keys < max_idx)
        {
            uint8_t *id = buf + sizeof(struct btree_node_header);

            /* Update parent_slot to left child's actual minimum */
            memcpy(id + (size_t)parent_slot * ie_sz, &left_ie, ie_sz);

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz,
                        id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);
            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);
            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);
            rc = numidx_node_write(ctx, parent_lba, buf);
            free(buf);
            if(rc != OBMAFS3_OK) return rc;
            return numidx_hdr_write(ctx);
        }

        /* Parent full — split index node */
        uint16_t idx_total = max_idx + 1;
        struct metadata_numeric_idx_index_entry *aie = calloc(idx_total, ie_sz);
        if(!aie) { free(buf); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(id + (size_t)parent_slot * ie_sz, &left_ie, ie_sz);

        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz,
               ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        uint64_t idx_old_right = phdr.right_link;
        uint64_t new_idx_lba;
        rc = numidx_alloc_node(ctx, &new_idx_lba);
        if(rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        memset(id, 0, nsz - sizeof(struct btree_node_header));
        memcpy(id, aie, (size_t)il * ie_sz);
        phdr.node_keys   = il;
        phdr.keys_length = (uint16_t)(il * ie_sz);
        phdr.right_link  = new_idx_lba;
        memcpy(buf, &phdr, sizeof(phdr));
        compute_node_checksum(buf);
        rc = numidx_node_write(ctx, parent_lba, buf);
        if(rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        memset(buf, 0, nsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeMetadataNumericIndexEntry;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        nih.left_link   = parent_lba;
        nih.right_link  = idx_old_right;
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = numidx_node_write(ctx, new_idx_lba, buf);
        if(rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        memset(&push_ie, 0, sizeof(push_ie));
        strncpy(push_ie.key, aie[il].key, METADATA_KEY_MAX);
        push_ie.value     = aie[il].value;
        push_ie.inode_id  = aie[il].inode_id;
        push_ie.child_lba = new_idx_lba;

        memset(&left_ie, 0, sizeof(left_ie));
        strncpy(left_ie.key, aie[0].key, METADATA_KEY_MAX);
        left_ie.value     = aie[0].value;
        left_ie.inode_id  = aie[0].inode_id;
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;
        free(aie);
        ctx->metadata_numeric_idx_hdr.total_nodes++;

        if(idx_old_right != 0)
        {
            rc = numidx_node_read(ctx, idx_old_right, buf);
            if(rc == OBMAFS3_OK)
            {
                struct btree_node_header rnh;
                memcpy(&rnh, buf, sizeof(rnh));
                rnh.left_link = new_idx_lba;
                memcpy(buf, &rnh, sizeof(rnh));
                compute_node_checksum(buf);
                numidx_node_write(ctx, idx_old_right, buf);
            }
        }
    }

    /* Create new root */
    uint64_t new_root_lba;
    rc = numidx_alloc_node(ctx, &new_root_lba);
    if(rc != OBMAFS3_OK) { free(buf); return rc; }

    rc = numidx_node_read(ctx, left_lba, buf);
    if(rc != OBMAFS3_OK) { free(buf); return rc; }
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, nsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeMetadataNumericIndexEntry;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * ie_sz);
    memcpy(buf, &rh, sizeof(rh));

    left_ie.child_lba = left_lba;
    struct metadata_numeric_idx_index_entry roots[2];
    roots[0] = left_ie;
    roots[1] = push_ie;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = numidx_node_write(ctx, new_root_lba, buf);
    free(buf);
    if(rc != OBMAFS3_OK) return rc;

    ctx->metadata_numeric_idx_hdr.root_node_lba = new_root_lba;
    ctx->metadata_numeric_idx_hdr.total_nodes++;
    return numidx_hdr_write(ctx);
}

/* ------------------------------------------------------------------ */
/*  Delete                                                             */
/* ------------------------------------------------------------------ */

int obmafs3_numidx_delete(struct obmafs3_ctx *ctx, const char *key, int64_t value, uint64_t inode_id)
{
    size_t   nsz      = numidx_node_size(ctx);
    uint64_t root_lba = ctx->metadata_numeric_idx_hdr.root_node_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    struct numidx_path path[NUMIDX_BTREE_MAX_DEPTH];
    int                depth = 0;
    uint64_t           lba   = root_lba;

    while(1)
    {
        rc = numidx_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic"); }
        if(hdr.level == 0) break;
        if(depth >= NUMIDX_BTREE_MAX_DEPTH) { free(buf); DBG_RETURN(OBMAFS3_ERR_INVAL, "tree too deep"); }

        uint16_t slot    = numidx_index_find(buf, hdr.node_keys, key, value, inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_numeric_idx_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = numidx_leaf_find(buf, leaf_hdr.node_keys, key, value, inode_id);
    if(idx < 0) { free(buf); return OBMAFS3_ERR_NOTFOUND; }

    size_t rec_sz = sizeof(struct metadata_numeric_idx_record);
    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        if(depth == 0)
        {
            ctx->metadata_numeric_idx_hdr.root_node_lba = 0;
            ctx->metadata_numeric_idx_hdr.total_nodes--;
            rc = numidx_hdr_write(ctx);
            numidx_free_node(ctx, lba);
        }
        else
        {
            /* Update sibling links */
            if(leaf_hdr.left_link != 0)
            {
                rc = numidx_node_read(ctx, leaf_hdr.left_link, buf);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header lnh;
                    memcpy(&lnh, buf, sizeof(lnh));
                    lnh.right_link = leaf_hdr.right_link;
                    memcpy(buf, &lnh, sizeof(lnh));
                    compute_node_checksum(buf);
                    numidx_node_write(ctx, leaf_hdr.left_link, buf);
                }
            }
            if(leaf_hdr.right_link != 0)
            {
                rc = numidx_node_read(ctx, leaf_hdr.right_link, buf);
                if(rc == OBMAFS3_OK)
                {
                    struct btree_node_header rnh;
                    memcpy(&rnh, buf, sizeof(rnh));
                    rnh.left_link = leaf_hdr.left_link;
                    memcpy(buf, &rnh, sizeof(rnh));
                    compute_node_checksum(buf);
                    numidx_node_write(ctx, leaf_hdr.right_link, buf);
                }
            }

            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, nsz);
            if(!pbuf) { free(buf); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }

            rc = numidx_node_read(ctx, plba, pbuf);
            if(rc != OBMAFS3_OK) { free(pbuf); free(buf); return rc; }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct metadata_numeric_idx_index_entry);

            if(pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz,
                        pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);
            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if(phdr.node_keys == 0 && depth == 1)
            {
                ctx->metadata_numeric_idx_hdr.root_node_lba = 0;
                ctx->metadata_numeric_idx_hdr.total_nodes -= 2;
                rc = numidx_hdr_write(ctx);
                numidx_free_node(ctx, lba);
                numidx_free_node(ctx, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                struct metadata_numeric_idx_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->metadata_numeric_idx_hdr.root_node_lba = remaining.child_lba;
                ctx->metadata_numeric_idx_hdr.total_nodes -= 2;
                rc = numidx_hdr_write(ctx);
                numidx_free_node(ctx, lba);
                numidx_free_node(ctx, plba);
            }
            else
            {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = numidx_node_write(ctx, plba, pbuf);
                if(rc == OBMAFS3_OK)
                {
                    ctx->metadata_numeric_idx_hdr.total_nodes--;
                    rc = numidx_hdr_write(ctx);
                }
                numidx_free_node(ctx, lba);
            }
            free(pbuf);
        }
    }
    else
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz,
                    data + ((size_t)idx + 1) * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)idx) * rec_sz);
        memset(data + (size_t)leaf_hdr.node_keys * rec_sz, 0, rec_sz);
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = numidx_node_write(ctx, lba, buf);
    }

    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Range scan for query integration                                   */
/* ------------------------------------------------------------------ */

/**
 * Collect inode IDs from the numeric index tree that match a range
 * predicate: low <= value <= high for a given key.
 *
 * For point queries (N=), set low == high.
 * For open-ended ranges, use INT64_MIN or INT64_MAX.
 *
 * Navigates to the first leaf where (key, low, 0) would appear,
 * then scans right until the key changes or value > high.
 */
int obmafs3_numidx_collect_range(struct obmafs3_ctx *ctx, const char *key,
                                 int64_t low, int64_t high,
                                 uint64_t **out_ids, uint32_t *out_count)
{
    *out_ids   = NULL;
    *out_count = 0;

    if(ctx->sb.metadata_numeric_idx_lba == 0) return OBMAFS3_OK;
    uint64_t root_lba = ctx->metadata_numeric_idx_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t   nsz = numidx_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Navigate to the leaf containing (key, low, 0) */
    uint64_t lba = root_lba;
    while(1)
    {
        int rc = numidx_node_read(ctx, lba, buf);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic"); }
        if(hdr.level == 0) break;

        uint16_t slot = numidx_index_find(buf, hdr.node_keys, key, low, 0);
        struct metadata_numeric_idx_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain collecting matching inode IDs */
    uint32_t  cap  = 64;
    uint32_t  n    = 0;
    uint64_t *ids  = malloc(cap * sizeof(uint64_t));
    if(!ids) { free(buf); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }

    while(1)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct metadata_numeric_idx_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            int kcmp = strncmp(rec.key, key, METADATA_KEY_MAX);
            if(kcmp < 0) continue;          /* haven't reached the key yet */
            if(kcmp > 0) goto range_done;   /* past the key — done */

            if(rec.value < low) continue;    /* below range start */
            if(rec.value > high) goto range_done; /* past range end — done (sorted) */

            /* In range — collect inode ID */
            if(n >= cap)
            {
                cap *= 2;
                uint64_t *tmp = realloc(ids, cap * sizeof(uint64_t));
                if(!tmp) { free(ids); free(buf); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }
                ids = tmp;
            }
            ids[n++] = rec.inode_id;
        }

        if(hdr.right_link == 0) break;
        int rc = numidx_node_read(ctx, hdr.right_link, buf);
        if(rc != OBMAFS3_OK) { free(ids); free(buf); return rc; }
    }

range_done:
    free(buf);
    *out_ids   = ids;
    *out_count = n;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Mount-time build from existing metadata_idx tree                   */
/* ------------------------------------------------------------------ */

/**
 * Check if a string value is a valid integer.
 * Returns 1 if the string parses as a valid int64_t, 0 otherwise.
 */
static int is_numeric_value(const char *s, int64_t *out)
{
    if(!s || !*s) return 0;
    char *end;
    long long v = strtoll(s, &end, 10);
    /* Must consume entire string (no trailing junk) */
    if(*end != '\0') return 0;
    *out = (int64_t)v;
    return 1;
}

int obmafs3_numidx_build(struct obmafs3_ctx *ctx)
{
    /* Only build if the metadata_idx tree exists */
    if(ctx->sb.metadata_idx_lba == 0) return OBMAFS3_OK;
    if(ctx->metadata_idx_hdr.root_node_lba == 0) return OBMAFS3_OK;

    /* If the numeric index tree header LBA isn't allocated yet,
     * allocate one block for it now. */
    if(ctx->sb.metadata_numeric_idx_lba == 0)
    {
        uint64_t hdr_lba;
        int rc = obmafs3_alloc_block(ctx, &hdr_lba);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
        hdr.data_type = kBtreeDataTypeMetadataNumericIndexEntry;
        hdr.node_size = (uint16_t)(METADATA_NODE_BLOCKS * ctx->sb.block_size);
        hdr.tree_type = kBtreeTypeMetadataNumericIndex;
        obmafs3_checksum_block(&hdr, sizeof(hdr), hdr.checksum);

        rc = obmafs3_block_write(ctx, hdr_lba, &hdr, sizeof(hdr));
        if(rc != OBMAFS3_OK) return rc;

        ctx->sb.metadata_numeric_idx_lba = hdr_lba;
        ctx->metadata_numeric_idx_hdr    = hdr;
    }

    /* Traverse the metadata_idx tree (leftmost leaf → right_link chain)
     * and insert any numeric values into the numeric index.  */
    size_t   nsz = (size_t)METADATA_NODE_BLOCKS * (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, nsz);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Navigate to leftmost leaf */
    uint64_t lba = ctx->metadata_idx_hdr.root_node_lba;
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, nsz);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic"); }
        if(hdr.level == 0) break;

        struct metadata_idx_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain */
    int      found_any = 0;
    while(1)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct metadata_idx_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            int64_t num;
            if(is_numeric_value(rec.value, &num))
            {
                int rc = obmafs3_numidx_put(ctx, rec.key, num, rec.inode_id);
                if(rc != OBMAFS3_OK) { free(buf); return rc; }
                found_any = 1;
            }
        }

        if(hdr.right_link == 0) break;
        int rc = obmafs3_block_read(ctx, hdr.right_link, buf, nsz);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }
    }

    free(buf);

    /* Set rocompat flag if we added entries and persist superblock */
    if(found_any && !(ctx->sb.rocompat_flags & OBMAFS3_ROCOMPAT_NUMERIC_IDX))
    {
        ctx->sb.rocompat_flags |= OBMAFS3_ROCOMPAT_NUMERIC_IDX;
        int rc = obmafs3_sb_write(ctx->fd, &ctx->sb);
        if(rc != OBMAFS3_OK) return rc;
    }

    return OBMAFS3_OK;
}
