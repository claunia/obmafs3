// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : sector_tag.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 per-sector tag B+Tree operations (hash-dedup variant).
//     Two trees:
//       - Sector Tag Data tree: hash-keyed dictionary of unique tag blobs
//       - Sector Tag Ref tree:  (inode_id, sector, tag_type) -> tag_hash
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

#include <limits.h>

/* ================================================================== */
/*  Sector Tag Data Tree — hash-keyed (like CD prefix/suffix)          */
/* ================================================================== */

static uint16_t stdata_leaf_max(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) /
                      sizeof(struct sector_tag_data_record));
}

static uint16_t stdata_index_max(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) /
                      sizeof(struct btree_index_entry));
}

static int stdata_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t hash)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int      mid = lo + (hi - lo) / 2;
        uint64_t mid_hash;
        memcpy(&mid_hash, data + (size_t)mid * sizeof(struct sector_tag_data_record), sizeof(mid_hash));

        if(mid_hash == hash) return mid;
        if(mid_hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -(lo + 1);
}

static uint16_t stdata_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t hash)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int                      mid = lo + (hi - lo) / 2;
        struct btree_index_entry ie;
        memcpy(&ie, data + (size_t)mid * sizeof(ie), sizeof(ie));

        if(ie.key <= hash) { result = (uint16_t)mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return result;
}

/** Lookup a sector_tag_data_record by hash. */
static int stdata_tree_lookup(struct obmafs3_ctx *ctx, uint64_t hash, struct sector_tag_data_record *record)
{
    uint64_t lba = ctx->sector_tag_data_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(nhdr.level > 0)
        {
            uint16_t                 slot = stdata_index_find(buf, nhdr.node_keys, hash);
            struct btree_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            int idx = stdata_leaf_find(buf, nhdr.node_keys, hash);
            if(idx >= 0)
            {
                memcpy(record,
                       buf + sizeof(struct btree_node_header) +
                           (size_t)idx * sizeof(struct sector_tag_data_record),
                       sizeof(*record));
                return OBMAFS3_OK;
            }
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

#define STDATA_BTREE_MAX_DEPTH 8

struct stdata_path { uint64_t lba; uint16_t slot; };

/** Insert or update a sector_tag_data_record (hash-keyed). */
static int stdata_tree_put(struct obmafs3_ctx *ctx, const struct sector_tag_data_record *record)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->sector_tag_data_hdr.root_node_lba;
    uint64_t hdr_lba  = ctx->sb.sector_tag_data_lba;
    int      rc;

    /* Empty tree — create root leaf */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_data_hdr, hdr_lba, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, bsz);
        if(!buf) return OBMAFS3_ERR_NOMEM;

        struct btree_node_header nh;
        memset(&nh, 0, sizeof(nh));
        nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nh.record_type = kBtreeDataTypeSectorTagDataEntry;
        nh.level       = 0;
        nh.node_keys   = 1;
        nh.keys_length = (uint16_t)sizeof(struct sector_tag_data_record);
        memcpy(buf, &nh, sizeof(nh));
        memcpy(buf + sizeof(nh), record, sizeof(*record));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        free(buf);
        if(rc != OBMAFS3_OK) return rc;

        ctx->sector_tag_data_hdr.root_node_lba = new_lba;
        return obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_data_hdr);
    }

    /* Traverse to the target leaf */
    struct stdata_path path[STDATA_BTREE_MAX_DEPTH];
    int                depth = 0;
    uint64_t           lba   = root_lba;

    uint8_t *buf  = calloc(1, bsz);
    uint8_t *buf2 = calloc(1, bsz);
    if(!buf || !buf2) { free(buf); free(buf2); return OBMAFS3_ERR_NOMEM; }

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) goto out;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { rc = OBMAFS3_ERR_BADMAGIC; goto out; }

        if(nhdr.level > 0)
        {
            uint16_t slot = stdata_index_find(buf, nhdr.node_keys, record->hash);
            if(depth < STDATA_BTREE_MAX_DEPTH) { path[depth].lba = lba; path[depth].slot = slot; depth++; }
            struct btree_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            /* Leaf node */
            int idx = stdata_leaf_find(buf, nhdr.node_keys, record->hash);

            if(idx >= 0)
            {
                /* Update existing entry */
                memcpy(buf + sizeof(struct btree_node_header) +
                           (size_t)idx * sizeof(struct sector_tag_data_record),
                       record, sizeof(*record));
                compute_node_checksum(buf);
                rc = obmafs3_block_write(ctx, lba, buf, bsz);
                goto out;
            }

            /* Insert new entry */
            int      ip       = -(idx + 1);
            uint16_t max_keys = stdata_leaf_max(ctx);

            if(nhdr.node_keys < max_keys)
            {
                /* Room in this leaf */
                uint8_t *data = buf + sizeof(struct btree_node_header);
                size_t   rec_sz = sizeof(struct sector_tag_data_record);
                memmove(data + ((size_t)ip + 1) * rec_sz,
                        data + (size_t)ip * rec_sz,
                        (size_t)(nhdr.node_keys - ip) * rec_sz);
                memcpy(data + (size_t)ip * rec_sz, record, rec_sz);
                nhdr.node_keys++;
                nhdr.keys_length = (uint16_t)(nhdr.node_keys * rec_sz);
                memcpy(buf, &nhdr, sizeof(nhdr));
                compute_node_checksum(buf);
                rc = obmafs3_block_write(ctx, lba, buf, bsz);
                goto out;
            }

            /* Split the leaf */
            uint16_t total     = nhdr.node_keys + 1;
            uint16_t left_cnt  = total / 2;
            uint16_t right_cnt = total - left_cnt;
            size_t   rec_sz    = sizeof(struct sector_tag_data_record);

            /* Build temporary sorted array */
            uint8_t *tmp = malloc((size_t)total * rec_sz);
            if(!tmp) { rc = OBMAFS3_ERR_NOMEM; goto out; }
            uint8_t *src = buf + sizeof(struct btree_node_header);
            memcpy(tmp, src, (size_t)ip * rec_sz);
            memcpy(tmp + (size_t)ip * rec_sz, record, rec_sz);
            memcpy(tmp + ((size_t)ip + 1) * rec_sz, src + (size_t)ip * rec_sz,
                   (size_t)(nhdr.node_keys - ip) * rec_sz);

            /* Left leaf stays in current node */
            memcpy(src, tmp, (size_t)left_cnt * rec_sz);
            nhdr.node_keys   = left_cnt;
            nhdr.keys_length = (uint16_t)(left_cnt * rec_sz);

            /* Allocate right leaf */
            uint64_t right_lba;
            rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_data_hdr, hdr_lba, &right_lba);
            if(rc != OBMAFS3_OK) { free(tmp); goto out; }

            memset(buf2, 0, bsz);
            struct btree_node_header rh;
            memset(&rh, 0, sizeof(rh));
            rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
            rh.record_type = kBtreeDataTypeSectorTagDataEntry;
            rh.level       = 0;
            rh.node_keys   = right_cnt;
            rh.keys_length = (uint16_t)(right_cnt * rec_sz);
            rh.left_link   = lba;
            rh.right_link  = nhdr.right_link;
            memcpy(buf2, &rh, sizeof(rh));
            memcpy(buf2 + sizeof(rh), tmp + (size_t)left_cnt * rec_sz, (size_t)right_cnt * rec_sz);
            compute_node_checksum(buf2);

            nhdr.right_link = right_lba;
            memcpy(buf, &nhdr, sizeof(nhdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, lba, buf, bsz);
            if(rc != OBMAFS3_OK) { free(tmp); goto out; }
            rc = obmafs3_block_write(ctx, right_lba, buf2, bsz);
            if(rc != OBMAFS3_OK) { free(tmp); goto out; }

            /* Get separator key (first hash in right leaf) */
            uint64_t sep_hash;
            memcpy(&sep_hash, tmp + (size_t)left_cnt * rec_sz, sizeof(sep_hash));
            free(tmp);

            /* Propagate split upward */
            uint64_t left_lba  = lba;
            uint64_t child_lba = right_lba;
            uint64_t sep_key   = sep_hash;

            while(depth > 0)
            {
                depth--;
                uint64_t parent_lba  = path[depth].lba;
                uint16_t parent_slot = path[depth].slot;

                rc = obmafs3_block_read(ctx, parent_lba, buf, bsz);
                if(rc != OBMAFS3_OK) goto out;

                struct btree_node_header ph;
                memcpy(&ph, buf, sizeof(ph));
                uint16_t imax = stdata_index_max(ctx);

                if(ph.node_keys < imax)
                {
                    /* Room in parent — update existing key and insert separator */
                    uint8_t *idx_data = buf + sizeof(struct btree_node_header);

                    /* Update parent_slot key to left child's actual minimum */
                    uint64_t left_min;
                    memcpy(&left_min, buf + sizeof(struct btree_node_header) +
                                          (size_t)parent_slot * sizeof(struct btree_index_entry),
                           sizeof(left_min));
                    /* Read actual left min from left child */
                    {
                        uint8_t *lbuf = calloc(1, bsz);
                        if(lbuf)
                        {
                            if(obmafs3_block_read(ctx, left_lba, lbuf, bsz) == OBMAFS3_OK)
                            {
                                struct btree_node_header lnh;
                                memcpy(&lnh, lbuf, sizeof(lnh));
                                if(lnh.node_keys > 0)
                                {
                                    if(lnh.level > 0)
                                        memcpy(&left_min, lbuf + sizeof(struct btree_node_header), sizeof(left_min));
                                    else
                                        memcpy(&left_min, lbuf + sizeof(struct btree_node_header), sizeof(left_min));
                                }
                            }
                            free(lbuf);
                        }
                    }
                    struct btree_index_entry upd;
                    memcpy(&upd, idx_data + (size_t)parent_slot * sizeof(upd), sizeof(upd));
                    upd.key = left_min;
                    memcpy(idx_data + (size_t)parent_slot * sizeof(upd), &upd, sizeof(upd));

                    struct btree_index_entry new_ie = { .key = sep_key, .child_lba = child_lba };
                    uint16_t ins = parent_slot + 1;
                    memmove(idx_data + ((size_t)ins + 1) * sizeof(new_ie),
                            idx_data + (size_t)ins * sizeof(new_ie),
                            (size_t)(ph.node_keys - ins) * sizeof(new_ie));
                    memcpy(idx_data + (size_t)ins * sizeof(new_ie), &new_ie, sizeof(new_ie));
                    ph.node_keys++;
                    ph.keys_length = (uint16_t)(ph.node_keys * sizeof(struct btree_index_entry));
                    memcpy(buf, &ph, sizeof(ph));
                    compute_node_checksum(buf);
                    rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
                    goto out;
                }

                /* Parent is full — split parent index node */
                uint16_t ptotal     = ph.node_keys + 1;
                uint16_t pleft_cnt  = ptotal / 2;
                uint16_t pright_cnt = ptotal - pleft_cnt;

                uint8_t *ptmp = malloc((size_t)ptotal * sizeof(struct btree_index_entry));
                if(!ptmp) { rc = OBMAFS3_ERR_NOMEM; goto out; }

                uint8_t *psrc = buf + sizeof(struct btree_node_header);
                uint16_t pins = parent_slot + 1;

                /* Update parent_slot key to left child's actual minimum */
                {
                    uint8_t *lbuf = calloc(1, bsz);
                    if(lbuf)
                    {
                        if(obmafs3_block_read(ctx, left_lba, lbuf, bsz) == OBMAFS3_OK)
                        {
                            uint64_t left_min;
                            memcpy(&left_min, lbuf + sizeof(struct btree_node_header), sizeof(left_min));
                            struct btree_index_entry upd;
                            memcpy(&upd, psrc + (size_t)parent_slot * sizeof(upd), sizeof(upd));
                            upd.key = left_min;
                            memcpy(psrc + (size_t)parent_slot * sizeof(upd), &upd, sizeof(upd));
                        }
                        free(lbuf);
                    }
                }

                memcpy(ptmp, psrc, (size_t)pins * sizeof(struct btree_index_entry));
                struct btree_index_entry new_ie = { .key = sep_key, .child_lba = child_lba };
                memcpy(ptmp + (size_t)pins * sizeof(struct btree_index_entry), &new_ie, sizeof(new_ie));
                memcpy(ptmp + ((size_t)pins + 1) * sizeof(struct btree_index_entry),
                       psrc + (size_t)pins * sizeof(struct btree_index_entry),
                       (size_t)(ph.node_keys - pins) * sizeof(struct btree_index_entry));

                /* Left index node */
                memcpy(psrc, ptmp, (size_t)pleft_cnt * sizeof(struct btree_index_entry));
                ph.node_keys   = pleft_cnt;
                ph.keys_length = (uint16_t)(pleft_cnt * sizeof(struct btree_index_entry));

                /* Right index node */
                uint64_t pright_lba;
                rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_data_hdr, hdr_lba, &pright_lba);
                if(rc != OBMAFS3_OK) { free(ptmp); goto out; }

                memset(buf2, 0, bsz);
                struct btree_node_header prh;
                memset(&prh, 0, sizeof(prh));
                prh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
                prh.record_type = kBtreeDataTypeSectorTagDataEntry;
                prh.level       = ph.level;
                prh.node_keys   = pright_cnt;
                prh.keys_length = (uint16_t)(pright_cnt * sizeof(struct btree_index_entry));
                prh.left_link   = parent_lba;
                prh.right_link  = ph.right_link;
                memcpy(buf2, &prh, sizeof(prh));
                memcpy(buf2 + sizeof(prh), ptmp + (size_t)pleft_cnt * sizeof(struct btree_index_entry),
                       (size_t)pright_cnt * sizeof(struct btree_index_entry));
                compute_node_checksum(buf2);

                ph.right_link = pright_lba;
                memcpy(buf, &ph, sizeof(ph));
                compute_node_checksum(buf);

                rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
                if(rc != OBMAFS3_OK) { free(ptmp); goto out; }
                rc = obmafs3_block_write(ctx, pright_lba, buf2, bsz);
                if(rc != OBMAFS3_OK) { free(ptmp); goto out; }

                struct btree_index_entry first_right;
                memcpy(&first_right, ptmp + (size_t)pleft_cnt * sizeof(struct btree_index_entry),
                       sizeof(first_right));
                sep_key   = first_right.key;
                left_lba  = parent_lba;
                child_lba = pright_lba;
                free(ptmp);
            }

            /* Need a new root */
            uint64_t new_root_lba;
            rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_data_hdr, hdr_lba, &new_root_lba);
            if(rc != OBMAFS3_OK) goto out;

            memset(buf, 0, bsz);
            struct btree_node_header nrh;
            memset(&nrh, 0, sizeof(nrh));
            nrh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
            nrh.record_type = kBtreeDataTypeSectorTagDataEntry;

            /* Determine level from left child */
            uint8_t *tbuf = calloc(1, bsz);
            uint8_t  new_level = 1;
            if(tbuf)
            {
                if(obmafs3_block_read(ctx, left_lba, tbuf, bsz) == OBMAFS3_OK)
                {
                    struct btree_node_header ch;
                    memcpy(&ch, tbuf, sizeof(ch));
                    new_level = ch.level + 1;
                }
                free(tbuf);
            }
            nrh.level       = new_level;
            nrh.node_keys   = 2;
            nrh.keys_length = (uint16_t)(2 * sizeof(struct btree_index_entry));
            memcpy(buf, &nrh, sizeof(nrh));

            /* Read actual minimum key of left child */
            uint64_t left_min_key = 0;
            tbuf = calloc(1, bsz);
            if(tbuf)
            {
                if(obmafs3_block_read(ctx, left_lba, tbuf, bsz) == OBMAFS3_OK)
                    memcpy(&left_min_key, tbuf + sizeof(struct btree_node_header), sizeof(left_min_key));
                free(tbuf);
            }

            struct btree_index_entry ie0 = { .key = left_min_key, .child_lba = left_lba };
            struct btree_index_entry ie1 = { .key = sep_key, .child_lba = child_lba };
            memcpy(buf + sizeof(nrh), &ie0, sizeof(ie0));
            memcpy(buf + sizeof(nrh) + sizeof(ie0), &ie1, sizeof(ie1));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
            if(rc != OBMAFS3_OK) goto out;

            ctx->sector_tag_data_hdr.root_node_lba = new_root_lba;
            rc = obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_data_hdr);
            goto out;
        }
    }

out:
    free(buf);
    free(buf2);
    return rc;
}

/* ================================================================== */
/*  Sector Tag Ref Tree — composite key (inode_id, sector, tag_type)   */
/* ================================================================== */

static uint16_t stref_leaf_max(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) /
                      sizeof(struct sector_tag_ref_record));
}

static uint16_t stref_index_max(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) /
                      sizeof(struct sector_tag_ref_index_entry));
}

static int stref_key_cmp(uint64_t id_a, int64_t sec_a, uint16_t type_a,
                          uint64_t id_b, int64_t sec_b, uint16_t type_b)
{
    if(id_a < id_b) return -1;
    if(id_a > id_b) return 1;
    if(sec_a < sec_b) return -1;
    if(sec_a > sec_b) return 1;
    if(type_a < type_b) return -1;
    if(type_a > type_b) return 1;
    return 0;
}

static int stref_leaf_find(const uint8_t *buf, uint16_t node_keys,
                            uint64_t inode_id, int64_t sector, uint16_t tag_type)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *rec = data + (size_t)mid * sizeof(struct sector_tag_ref_record);
        uint64_t mid_id;
        int64_t  mid_sec;
        uint16_t mid_type;
        memcpy(&mid_id, rec, sizeof(mid_id));
        memcpy(&mid_sec, rec + sizeof(mid_id), sizeof(mid_sec));
        memcpy(&mid_type, rec + sizeof(mid_id) + sizeof(mid_sec), sizeof(mid_type));

        int cmp = stref_key_cmp(mid_id, mid_sec, mid_type, inode_id, sector, tag_type);
        if(cmp == 0) return mid;
        if(cmp < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -(lo + 1);
}

static uint16_t stref_index_find(const uint8_t *buf, uint16_t node_keys,
                                  uint64_t inode_id, int64_t sector, uint16_t tag_type)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *ie = data + (size_t)mid * sizeof(struct sector_tag_ref_index_entry);
        uint64_t mid_id;
        int64_t  mid_sec;
        uint16_t mid_type;
        memcpy(&mid_id, ie, sizeof(mid_id));
        memcpy(&mid_sec, ie + sizeof(mid_id), sizeof(mid_sec));
        memcpy(&mid_type, ie + sizeof(mid_id) + sizeof(mid_sec), sizeof(mid_type));

        int cmp = stref_key_cmp(mid_id, mid_sec, mid_type, inode_id, sector, tag_type);
        if(cmp <= 0) { result = (uint16_t)mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return result;
}

/** Lookup a sector_tag_ref_record by (inode_id, sector, tag_type). */
static int stref_tree_lookup(struct obmafs3_ctx *ctx,
                              uint64_t inode_id, int64_t sector, uint16_t tag_type,
                              struct sector_tag_ref_record *record)
{
    uint64_t lba = ctx->sector_tag_ref_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(nhdr.level > 0)
        {
            uint16_t slot = stref_index_find(buf, nhdr.node_keys, inode_id, sector, tag_type);
            struct sector_tag_ref_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            int idx = stref_leaf_find(buf, nhdr.node_keys, inode_id, sector, tag_type);
            if(idx >= 0)
            {
                memcpy(record,
                       buf + sizeof(struct btree_node_header) +
                           (size_t)idx * sizeof(struct sector_tag_ref_record),
                       sizeof(*record));
                return OBMAFS3_OK;
            }
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

#define STREF_BTREE_MAX_DEPTH 8

struct stref_path { uint64_t lba; uint16_t slot; };

/** Insert or update a sector_tag_ref_record (composite-keyed). */
static int stref_tree_put(struct obmafs3_ctx *ctx, const struct sector_tag_ref_record *record)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->sector_tag_ref_hdr.root_node_lba;
    uint64_t hdr_lba  = ctx->sb.sector_tag_ref_lba;
    int      rc;

    /* Empty tree — create root leaf */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, bsz);
        if(!buf) return OBMAFS3_ERR_NOMEM;

        struct btree_node_header nh;
        memset(&nh, 0, sizeof(nh));
        nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nh.record_type = kBtreeDataTypeSectorTagRefEntry;
        nh.level       = 0;
        nh.node_keys   = 1;
        nh.keys_length = (uint16_t)sizeof(struct sector_tag_ref_record);
        memcpy(buf, &nh, sizeof(nh));
        memcpy(buf + sizeof(nh), record, sizeof(*record));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        free(buf);
        if(rc != OBMAFS3_OK) return rc;

        ctx->sector_tag_ref_hdr.root_node_lba = new_lba;
        return obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_ref_hdr);
    }

    /* Traverse to the target leaf */
    struct stref_path path[STREF_BTREE_MAX_DEPTH];
    int               depth = 0;
    uint64_t          lba   = root_lba;

    uint8_t *buf  = calloc(1, bsz);
    uint8_t *buf2 = calloc(1, bsz);
    if(!buf || !buf2) { free(buf); free(buf2); return OBMAFS3_ERR_NOMEM; }

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) goto out2;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { rc = OBMAFS3_ERR_BADMAGIC; goto out2; }

        if(nhdr.level > 0)
        {
            uint16_t slot = stref_index_find(buf, nhdr.node_keys,
                                              record->inode_id, record->sector, record->tag_type);
            if(depth < STREF_BTREE_MAX_DEPTH) { path[depth].lba = lba; path[depth].slot = slot; depth++; }
            struct sector_tag_ref_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            /* Leaf node */
            int idx = stref_leaf_find(buf, nhdr.node_keys,
                                       record->inode_id, record->sector, record->tag_type);

            if(idx >= 0)
            {
                /* Update existing entry */
                memcpy(buf + sizeof(struct btree_node_header) +
                           (size_t)idx * sizeof(struct sector_tag_ref_record),
                       record, sizeof(*record));
                compute_node_checksum(buf);
                rc = obmafs3_block_write(ctx, lba, buf, bsz);
                goto out2;
            }

            /* Insert new entry */
            int      ip       = -(idx + 1);
            uint16_t max_keys = stref_leaf_max(ctx);

            if(nhdr.node_keys < max_keys)
            {
                /* Room in this leaf */
                uint8_t *data = buf + sizeof(struct btree_node_header);
                size_t   rec_sz = sizeof(struct sector_tag_ref_record);
                memmove(data + ((size_t)ip + 1) * rec_sz,
                        data + (size_t)ip * rec_sz,
                        (size_t)(nhdr.node_keys - ip) * rec_sz);
                memcpy(data + (size_t)ip * rec_sz, record, rec_sz);
                nhdr.node_keys++;
                nhdr.keys_length = (uint16_t)(nhdr.node_keys * rec_sz);
                memcpy(buf, &nhdr, sizeof(nhdr));
                compute_node_checksum(buf);
                rc = obmafs3_block_write(ctx, lba, buf, bsz);
                goto out2;
            }

            /* Split the leaf */
            uint16_t total     = nhdr.node_keys + 1;
            uint16_t left_cnt  = total / 2;
            uint16_t right_cnt = total - left_cnt;
            size_t   rec_sz    = sizeof(struct sector_tag_ref_record);

            uint8_t *tmp = malloc((size_t)total * rec_sz);
            if(!tmp) { rc = OBMAFS3_ERR_NOMEM; goto out2; }
            uint8_t *src = buf + sizeof(struct btree_node_header);
            memcpy(tmp, src, (size_t)ip * rec_sz);
            memcpy(tmp + (size_t)ip * rec_sz, record, rec_sz);
            memcpy(tmp + ((size_t)ip + 1) * rec_sz, src + (size_t)ip * rec_sz,
                   (size_t)(nhdr.node_keys - ip) * rec_sz);

            /* Left leaf */
            memcpy(src, tmp, (size_t)left_cnt * rec_sz);
            nhdr.node_keys   = left_cnt;
            nhdr.keys_length = (uint16_t)(left_cnt * rec_sz);

            /* Right leaf */
            uint64_t right_lba;
            rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, &right_lba);
            if(rc != OBMAFS3_OK) { free(tmp); goto out2; }

            memset(buf2, 0, bsz);
            struct btree_node_header rh;
            memset(&rh, 0, sizeof(rh));
            rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
            rh.record_type = kBtreeDataTypeSectorTagRefEntry;
            rh.level       = 0;
            rh.node_keys   = right_cnt;
            rh.keys_length = (uint16_t)(right_cnt * rec_sz);
            rh.left_link   = lba;
            rh.right_link  = nhdr.right_link;
            memcpy(buf2, &rh, sizeof(rh));
            memcpy(buf2 + sizeof(rh), tmp + (size_t)left_cnt * rec_sz, (size_t)right_cnt * rec_sz);
            compute_node_checksum(buf2);

            nhdr.right_link = right_lba;
            memcpy(buf, &nhdr, sizeof(nhdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, lba, buf, bsz);
            if(rc != OBMAFS3_OK) { free(tmp); goto out2; }
            rc = obmafs3_block_write(ctx, right_lba, buf2, bsz);
            if(rc != OBMAFS3_OK) { free(tmp); goto out2; }

            /* Get separator key (first record in right leaf) */
            struct sector_tag_ref_record sep_rec;
            memcpy(&sep_rec, tmp + (size_t)left_cnt * rec_sz, sizeof(sep_rec));
            free(tmp);

            /* Propagate split upward */
            uint64_t left_lba_p  = lba;
            uint64_t child_lba_p = right_lba;

            while(depth > 0)
            {
                depth--;
                uint64_t parent_lba  = path[depth].lba;
                uint16_t parent_slot = path[depth].slot;

                rc = obmafs3_block_read(ctx, parent_lba, buf, bsz);
                if(rc != OBMAFS3_OK) goto out2;

                struct btree_node_header ph;
                memcpy(&ph, buf, sizeof(ph));
                uint16_t imax = stref_index_max(ctx);

                if(ph.node_keys < imax)
                {
                    uint8_t *idx_data = buf + sizeof(struct btree_node_header);

                    /* Update parent_slot key to left child's actual minimum */
                    {
                        uint8_t *lbuf = calloc(1, bsz);
                        if(lbuf)
                        {
                            if(obmafs3_block_read(ctx, left_lba_p, lbuf, bsz) == OBMAFS3_OK)
                            {
                                struct sector_tag_ref_index_entry upd;
                                memcpy(&upd, idx_data + (size_t)parent_slot * sizeof(upd), sizeof(upd));
                                struct sector_tag_ref_record lmin;
                                memcpy(&lmin, lbuf + sizeof(struct btree_node_header), sizeof(lmin));
                                upd.inode_id = lmin.inode_id;
                                upd.sector   = lmin.sector;
                                upd.tag_type = lmin.tag_type;
                                memcpy(idx_data + (size_t)parent_slot * sizeof(upd), &upd, sizeof(upd));
                            }
                            free(lbuf);
                        }
                    }

                    struct sector_tag_ref_index_entry new_ie = {
                        .inode_id  = sep_rec.inode_id,
                        .sector    = sep_rec.sector,
                        .tag_type  = sep_rec.tag_type,
                        .child_lba = child_lba_p
                    };
                    uint16_t ins = parent_slot + 1;
                    memmove(idx_data + ((size_t)ins + 1) * sizeof(new_ie),
                            idx_data + (size_t)ins * sizeof(new_ie),
                            (size_t)(ph.node_keys - ins) * sizeof(new_ie));
                    memcpy(idx_data + (size_t)ins * sizeof(new_ie), &new_ie, sizeof(new_ie));
                    ph.node_keys++;
                    ph.keys_length = (uint16_t)(ph.node_keys * sizeof(struct sector_tag_ref_index_entry));
                    memcpy(buf, &ph, sizeof(ph));
                    compute_node_checksum(buf);
                    rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
                    goto out2;
                }

                /* Parent is full — split parent */
                uint16_t ptotal     = ph.node_keys + 1;
                uint16_t pleft_cnt  = ptotal / 2;
                uint16_t pright_cnt = ptotal - pleft_cnt;
                size_t   iesz       = sizeof(struct sector_tag_ref_index_entry);

                uint8_t *ptmp = malloc((size_t)ptotal * iesz);
                if(!ptmp) { rc = OBMAFS3_ERR_NOMEM; goto out2; }

                uint8_t *psrc = buf + sizeof(struct btree_node_header);
                uint16_t pins = parent_slot + 1;

                /* Update parent_slot key to left child's minimum */
                {
                    uint8_t *lbuf = calloc(1, bsz);
                    if(lbuf)
                    {
                        if(obmafs3_block_read(ctx, left_lba_p, lbuf, bsz) == OBMAFS3_OK)
                        {
                            struct sector_tag_ref_index_entry upd;
                            memcpy(&upd, psrc + (size_t)parent_slot * iesz, iesz);
                            struct sector_tag_ref_record lmin;
                            memcpy(&lmin, lbuf + sizeof(struct btree_node_header), sizeof(lmin));
                            upd.inode_id = lmin.inode_id;
                            upd.sector   = lmin.sector;
                            upd.tag_type = lmin.tag_type;
                            memcpy(psrc + (size_t)parent_slot * iesz, &upd, iesz);
                        }
                        free(lbuf);
                    }
                }

                memcpy(ptmp, psrc, (size_t)pins * iesz);
                struct sector_tag_ref_index_entry new_ie = {
                    .inode_id = sep_rec.inode_id, .sector = sep_rec.sector,
                    .tag_type = sep_rec.tag_type, .child_lba = child_lba_p
                };
                memcpy(ptmp + (size_t)pins * iesz, &new_ie, iesz);
                memcpy(ptmp + ((size_t)pins + 1) * iesz, psrc + (size_t)pins * iesz,
                       (size_t)(ph.node_keys - pins) * iesz);

                memcpy(psrc, ptmp, (size_t)pleft_cnt * iesz);
                ph.node_keys   = pleft_cnt;
                ph.keys_length = (uint16_t)(pleft_cnt * iesz);

                uint64_t pright_lba;
                rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, &pright_lba);
                if(rc != OBMAFS3_OK) { free(ptmp); goto out2; }

                memset(buf2, 0, bsz);
                struct btree_node_header prh;
                memset(&prh, 0, sizeof(prh));
                prh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
                prh.record_type = kBtreeDataTypeSectorTagRefEntry;
                prh.level       = ph.level;
                prh.node_keys   = pright_cnt;
                prh.keys_length = (uint16_t)(pright_cnt * iesz);
                prh.left_link   = parent_lba;
                prh.right_link  = ph.right_link;
                memcpy(buf2, &prh, sizeof(prh));
                memcpy(buf2 + sizeof(prh), ptmp + (size_t)pleft_cnt * iesz, (size_t)pright_cnt * iesz);
                compute_node_checksum(buf2);

                ph.right_link = pright_lba;
                memcpy(buf, &ph, sizeof(ph));
                compute_node_checksum(buf);

                rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
                if(rc != OBMAFS3_OK) { free(ptmp); goto out2; }
                rc = obmafs3_block_write(ctx, pright_lba, buf2, bsz);
                if(rc != OBMAFS3_OK) { free(ptmp); goto out2; }

                struct sector_tag_ref_index_entry first_right;
                memcpy(&first_right, ptmp + (size_t)pleft_cnt * iesz, sizeof(first_right));
                sep_rec.inode_id = first_right.inode_id;
                sep_rec.sector   = first_right.sector;
                sep_rec.tag_type = first_right.tag_type;
                left_lba_p  = parent_lba;
                child_lba_p = pright_lba;
                free(ptmp);
            }

            /* Need a new root */
            uint64_t new_root_lba;
            rc = obmafs3_btree_alloc_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, &new_root_lba);
            if(rc != OBMAFS3_OK) goto out2;

            memset(buf, 0, bsz);
            struct btree_node_header nrh;
            memset(&nrh, 0, sizeof(nrh));
            nrh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
            nrh.record_type = kBtreeDataTypeSectorTagRefEntry;

            uint8_t *tbuf = calloc(1, bsz);
            uint8_t  new_level = 1;
            if(tbuf)
            {
                if(obmafs3_block_read(ctx, left_lba_p, tbuf, bsz) == OBMAFS3_OK)
                {
                    struct btree_node_header ch;
                    memcpy(&ch, tbuf, sizeof(ch));
                    new_level = ch.level + 1;
                }
                free(tbuf);
            }
            nrh.level       = new_level;
            nrh.node_keys   = 2;
            nrh.keys_length = (uint16_t)(2 * sizeof(struct sector_tag_ref_index_entry));
            memcpy(buf, &nrh, sizeof(nrh));

            /* Read left child's minimum key for ie0 */
            struct sector_tag_ref_record lmin;
            memset(&lmin, 0, sizeof(lmin));
            tbuf = calloc(1, bsz);
            if(tbuf)
            {
                if(obmafs3_block_read(ctx, left_lba_p, tbuf, bsz) == OBMAFS3_OK)
                    memcpy(&lmin, tbuf + sizeof(struct btree_node_header), sizeof(lmin));
                free(tbuf);
            }

            struct sector_tag_ref_index_entry ie0 = {
                .inode_id = lmin.inode_id, .sector = lmin.sector,
                .tag_type = lmin.tag_type, .child_lba = left_lba_p
            };
            struct sector_tag_ref_index_entry ie1 = {
                .inode_id = sep_rec.inode_id, .sector = sep_rec.sector,
                .tag_type = sep_rec.tag_type, .child_lba = child_lba_p
            };
            memcpy(buf + sizeof(nrh), &ie0, sizeof(ie0));
            memcpy(buf + sizeof(nrh) + sizeof(ie0), &ie1, sizeof(ie1));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
            if(rc != OBMAFS3_OK) goto out2;

            ctx->sector_tag_ref_hdr.root_node_lba = new_root_lba;
            rc = obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_ref_hdr);
            goto out2;
        }
    }

out2:
    free(buf);
    free(buf2);
    return rc;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

/**
 * Compute the hash for a sector tag entry.
 * Hash = XXH64(tag_type || data[0..data_length-1]).
 */
static uint64_t sector_tag_hash(uint16_t tag_type, const void *data, uint16_t data_length)
{
    /* Hash = XXH64(tag_type || data).  We build a small contiguous buffer. */
    uint8_t tmp[sizeof(uint16_t) + SECTOR_TAG_DATA_MAX];
    memcpy(tmp, &tag_type, sizeof(tag_type));
    memcpy(tmp + sizeof(tag_type), data, data_length);
    return obmafs3_checksum_xxh64(tmp, sizeof(tag_type) + data_length);
}

/**
 * Read a sector tag.
 *
 * Looks up the (inode_id, sector, tag_type) in the ref tree to get
 * the tag hash, then looks up the hash in the data tree to get the
 * actual tag data.
 */
int obmafs3_sector_tag_get(struct obmafs3_ctx *ctx, uint64_t inode_id, int64_t sector, uint16_t tag_type,
                           void *data, uint16_t *data_length)
{
    if(ctx->sb.sector_tag_ref_lba == 0 || ctx->sb.sector_tag_data_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    /* Step 1: look up the reference */
    struct sector_tag_ref_record ref;
    int rc = stref_tree_lookup(ctx, inode_id, sector, tag_type, &ref);
    if(rc != OBMAFS3_OK) return rc;

    /* Step 2: look up the data by hash */
    struct sector_tag_data_record drec;
    rc = stdata_tree_lookup(ctx, ref.tag_hash, &drec);
    if(rc != OBMAFS3_OK) return rc;

    if(drec.data_length > SECTOR_TAG_DATA_MAX)
        drec.data_length = SECTOR_TAG_DATA_MAX;

    *data_length = drec.data_length;
    memcpy(data, drec.data, drec.data_length);
    return OBMAFS3_OK;
}

/**
 * Write a sector tag.
 *
 * Inserts the tag data into the data tree (dedup by hash), then
 * inserts a reference into the ref tree.
 */
int obmafs3_sector_tag_put(struct obmafs3_ctx *ctx, uint64_t inode_id, int64_t sector, uint16_t tag_type,
                           const void *data, uint16_t data_length)
{
    if(ctx->sb.sector_tag_ref_lba == 0 || ctx->sb.sector_tag_data_lba == 0)
        return OBMAFS3_ERR_INVAL;

    if(data_length > SECTOR_TAG_DATA_MAX)
        return OBMAFS3_ERR_INVAL;

    /* Compute hash */
    uint64_t hash = sector_tag_hash(tag_type, data, data_length);

    /* Step 1: insert into data tree (idempotent — same hash = same data) */
    struct sector_tag_data_record drec;
    memset(&drec, 0, sizeof(drec));
    drec.hash        = hash;
    drec.tag_type    = tag_type;
    drec.data_length = data_length;
    memcpy(drec.data, data, data_length);

    int rc = stdata_tree_put(ctx, &drec);
    if(rc != OBMAFS3_OK) return rc;

    /* Step 2: insert reference */
    struct sector_tag_ref_record ref;
    memset(&ref, 0, sizeof(ref));
    ref.inode_id = inode_id;
    ref.sector   = sector;
    ref.tag_type = tag_type;
    ref.tag_hash = hash;

    return stref_tree_put(ctx, &ref);
}

/**
 * Delete a single sector tag ref record from the Ref B+Tree.
 *
 * Removes the record keyed by (inode_id, sector, tag_type).  Empty leaf
 * nodes are freed and parent index entries are updated.
 */
static int stref_tree_delete(struct obmafs3_ctx *ctx,
                              uint64_t inode_id, int64_t sector, uint16_t tag_type)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->sector_tag_ref_hdr.root_node_lba;
    uint64_t hdr_lba  = ctx->sb.sector_tag_ref_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct stref_path path[STREF_BTREE_MAX_DEPTH];
    int               depth = 0;
    uint64_t          lba   = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(hdr.level == 0) break;

        if(depth >= STREF_BTREE_MAX_DEPTH) DBG_RETURN(OBMAFS3_ERR_INVAL, "tree too deep");

        uint16_t slot    = stref_index_find(buf, hdr.node_keys, inode_id, sector, tag_type);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct sector_tag_ref_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = stref_leaf_find(buf, leaf_hdr.node_keys, inode_id, sector, tag_type);
    if(idx < 0) return OBMAFS3_ERR_NOTFOUND;

    size_t rec_sz = sizeof(struct sector_tag_ref_record);

    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        /* Leaf is now empty */
        if(depth == 0)
        {
            /* Only node in tree — tree becomes empty */
            ctx->sector_tag_ref_hdr.root_node_lba = 0;
            ctx->sector_tag_ref_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_ref_hdr);
            obmafs3_btree_free_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, lba);
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

            /* Remove index entry from parent */
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if(!pbuf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if(rc != OBMAFS3_OK) { free(pbuf); return rc; }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct sector_tag_ref_index_entry);

            if(pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz, pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if(phdr.node_keys == 0 && depth == 1)
            {
                /* Parent was root with single child — tree becomes empty */
                ctx->sector_tag_ref_hdr.root_node_lba = 0;
                ctx->sector_tag_ref_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_ref_hdr);
                obmafs3_btree_free_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, lba);
                obmafs3_btree_free_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                /* Collapse: promote remaining child to root */
                struct sector_tag_ref_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->sector_tag_ref_hdr.root_node_lba = remaining.child_lba;
                ctx->sector_tag_ref_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_ref_hdr);
                obmafs3_btree_free_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, lba);
                obmafs3_btree_free_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, plba);
            }
            else
            {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if(rc == OBMAFS3_OK)
                {
                    ctx->sector_tag_ref_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, hdr_lba, &ctx->sector_tag_ref_hdr);
                }
                obmafs3_btree_free_node(ctx, &ctx->sector_tag_ref_hdr, hdr_lba, lba);
            }

            free(pbuf);
        }
    }
    else
    {
        /* Shift remaining records left and write updated leaf */
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

    return rc;
}

/**
 * List all sector tag entries for a given inode.
 *
 * Traverses the ref tree leaf chain and collects all (sector, tag_type)
 * pairs for @p inode_id.  Returns dynamically allocated arrays that the
 * caller must free.
 *
 * @param ctx        Filesystem context.
 * @param inode_id   Target inode.
 * @param sectors    Output array of sector numbers.
 * @param tag_types  Output array of tag types.
 * @param count      Output number of entries.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int stref_list_for_inode(struct obmafs3_ctx *ctx, uint64_t inode_id,
                                 int64_t **sectors, uint16_t **tag_types, uint32_t *count)
{
    *sectors   = NULL;
    *tag_types = NULL;
    *count     = 0;

    if(ctx->sb.sector_tag_ref_lba == 0) return OBMAFS3_OK;

    uint64_t lba = ctx->sector_tag_ref_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_OK;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Navigate to the leaf that would contain (inode_id, INT64_MIN, 0) */
    int64_t  min_sector = INT64_MIN;
    uint16_t min_type   = 0;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic"); }
        if(hdr.level == 0) break;

        uint16_t                      slot = stref_index_find(buf, hdr.node_keys, inode_id, min_sector, min_type);
        struct sector_tag_ref_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain */
    uint32_t  cap  = 64;
    int64_t  *secs = malloc(cap * sizeof(int64_t));
    uint16_t *typs = malloc(cap * sizeof(uint16_t));
    if(!secs || !typs) { free(secs); free(typs); free(buf); return OBMAFS3_ERR_NOMEM; }

    uint32_t n = 0;

    while(1)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct sector_tag_ref_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if(rec.inode_id == inode_id)
            {
                if(n >= cap)
                {
                    cap *= 2;
                    int64_t  *ts = realloc(secs, cap * sizeof(int64_t));
                    uint16_t *tt = realloc(typs, cap * sizeof(uint16_t));
                    if(!ts || !tt) { free(ts ? ts : secs); free(tt ? tt : typs); free(buf); return OBMAFS3_ERR_NOMEM; }
                    secs = ts;
                    typs = tt;
                }
                secs[n] = rec.sector;
                typs[n] = rec.tag_type;
                n++;
            }
            else if(rec.inode_id > inode_id)
            {
                goto list_done;
            }
        }

        /* Move to right sibling */
        if(hdr.right_link == 0) break;
        int rc = obmafs3_block_read(ctx, hdr.right_link, buf, bsz);
        if(rc != OBMAFS3_OK) break;
    }

list_done:
    free(buf);
    *sectors   = secs;
    *tag_types = typs;
    *count     = n;
    return OBMAFS3_OK;
}

/**
 * Delete all sector tags for a given inode.
 *
 * Lists all ref entries for the inode and deletes them one at a time.
 * Data tree entries are NOT removed (they may be shared by other inodes);
 * orphaned data entries are harmless and can be cleaned up by fsck.
 */
int obmafs3_sector_tag_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    if(ctx->sb.sector_tag_ref_lba == 0) return OBMAFS3_OK;
    if(ctx->sector_tag_ref_hdr.root_node_lba == 0) return OBMAFS3_OK;

    /* Collect all entries first, then delete in reverse order
     * (reverse avoids index shifts invalidating our list) */
    int64_t  *secs;
    uint16_t *typs;
    uint32_t  count;
    int       rc = stref_list_for_inode(ctx, inode_id, &secs, &typs, &count);
    if(rc != OBMAFS3_OK) return rc;

    if(count == 0)
    {
        free(secs);
        free(typs);
        return OBMAFS3_OK;
    }

    /* Delete in reverse order to minimize tree restructuring */
    for(uint32_t i = count; i > 0; i--)
    {
        rc = stref_tree_delete(ctx, inode_id, secs[i - 1], typs[i - 1]);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_NOTFOUND) break;
        rc = OBMAFS3_OK;
    }

    free(secs);
    free(typs);
    return rc;
}
