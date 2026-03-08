// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : junk_map.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — Junk Map B+Tree
//
// --[ Description ] ----------------------------------------------------------
//
//     B+Tree storage for Nintendo disc junk/padding seed entries.
//     Keyed by (inode_id, offset). Each leaf record embeds the 17-word
//     LFG seed inline (94 bytes per record, 42 records per 4096-byte leaf).
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

#include "btree.h"
#include "defs.h"
#include "obmafs.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nh = (struct btree_node_header *)buf;
    memset(nh->checksum, 0, sizeof(nh->checksum));
    obmafs3_checksum_block(buf, sizeof(*nh) + nh->keys_length, nh->checksum);
}

static int jm_key_cmp(uint64_t id_a, uint64_t off_a, uint64_t id_b, uint64_t off_b)
{
    if(id_a < id_b) return -1;
    if(id_a > id_b) return 1;
    if(off_a < off_b) return -1;
    if(off_a > off_b) return 1;
    return 0;
}

/* Binary search in leaf node — returns index if found, or -(insertion_point+1) */
static int jm_leaf_find(const uint8_t *buf, uint16_t nkeys, uint64_t inode_id, uint64_t offset)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)nkeys - 1;
    while(lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *rec = data + (size_t)mid * sizeof(struct junk_map_record);
        uint64_t mid_id, mid_off;
        memcpy(&mid_id, rec, sizeof(mid_id));
        memcpy(&mid_off, rec + 8, sizeof(mid_off));
        int cmp = jm_key_cmp(mid_id, mid_off, inode_id, offset);
        if(cmp == 0) return mid;
        if(cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -(lo + 1);
}

/* Binary search in index node — returns slot of child to descend into */
static uint16_t jm_index_find(const uint8_t *buf, uint16_t nkeys, uint64_t inode_id, uint64_t offset)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    uint16_t slot = 0;
    for(uint16_t i = 1; i < nkeys; i++)
    {
        struct junk_map_index_entry ie;
        memcpy(&ie, data + (size_t)i * sizeof(ie), sizeof(ie));
        if(jm_key_cmp(ie.inode_id, ie.offset, inode_id, offset) <= 0)
            slot = i;
        else
            break;
    }
    return slot;
}

/* ---- Lazy tree creation ---- */

static int jm_ensure_tree(struct obmafs3_ctx *ctx)
{
    if(ctx->sb.junk_map_lba != 0) return OBMAFS3_OK;

    /* Allocate a block for the header */
    uint64_t hdr_lba;
    int rc = obmafs3_alloc_block(ctx, &hdr_lba);
    if(rc != OBMAFS3_OK) return rc;

    struct btree_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    hdr.data_type = kBtreeDataTypeJunkMapEntry;
    hdr.node_size = (uint16_t)ctx->sb.block_size;
    hdr.tree_type = kBtreeTypeJunkMap;
    obmafs3_checksum_block(&hdr, sizeof(hdr), hdr.checksum);

    rc = obmafs3_block_write(ctx, hdr_lba, &hdr, sizeof(hdr));
    if(rc != OBMAFS3_OK) return rc;

    ctx->junk_map_hdr    = hdr;
    ctx->sb.junk_map_lba = hdr_lba;

    /* Persist superblock with new LBA */
    rc = obmafs3_sb_write(ctx->fd, &ctx->sb);
    return rc;
}

/* ================================================================== */
/*  Insert                                                             */
/* ================================================================== */

int obmafs3_junk_map_put(struct obmafs3_ctx *ctx, uint64_t inode_id, uint64_t offset, uint64_t length,
                         uint16_t partition_index, const uint32_t seed[NGC_LFG_SEED_SIZE])
{
    int rc = jm_ensure_tree(ctx);
    if(rc != OBMAFS3_OK) return rc;

    uint64_t hdr_lba = ctx->sb.junk_map_lba;
    size_t   bsz     = ctx->sb.block_size;
    size_t   rec_sz  = sizeof(struct junk_map_record);
    uint16_t max_keys = (uint16_t)((bsz - sizeof(struct btree_node_header)) / rec_sz);

    /* Build the record */
    struct junk_map_record record;
    record.inode_id        = inode_id;
    record.offset          = offset;
    record.length          = length;
    record.partition_index = partition_index;
    memcpy(record.seed, seed, sizeof(record.seed));

    uint64_t root_lba = ctx->junk_map_hdr.root_node_lba;

    /* ---- Empty tree: create root leaf ---- */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = obmafs3_btree_alloc_node(ctx, &ctx->junk_map_hdr, hdr_lba, &new_lba);
        if(rc != OBMAFS3_OK) return rc;
        ctx->junk_map_hdr.total_nodes++;

        uint8_t *buf = calloc(1, bsz);
        if(!buf) return OBMAFS3_ERR_NOMEM;

        struct btree_node_header nh;
        memset(&nh, 0, sizeof(nh));
        nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nh.record_type = kBtreeDataTypeJunkMapEntry;
        nh.level       = 0;
        nh.node_keys   = 1;
        nh.keys_length = (uint16_t)rec_sz;
        memcpy(buf, &nh, sizeof(nh));
        memcpy(buf + sizeof(nh), &record, rec_sz);
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        free(buf);
        if(rc != OBMAFS3_OK) return rc;

        ctx->junk_map_hdr.root_node_lba = new_lba;
        return obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr);
    }

    /* ---- Navigate to the target leaf ---- */
    uint64_t path_lba[32];
    uint16_t path_slot[32];
    int      depth = 0;

    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t lba = root_lba;
    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { fprintf(stderr, "[junk_map] block_read failed at LBA %" PRIu64 ": rc=%d\n", lba, rc); free(buf); return rc; }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            fprintf(stderr, "[junk_map] bad node magic at LBA %" PRIu64 ": 0x%" PRIx64 " (expected BTREENDE)\n",
                    lba, nhdr.magic);
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level == 0) break; /* reached leaf */

        uint16_t slot = jm_index_find(buf, nhdr.node_keys, inode_id, offset);
        if(depth < 32)
        {
            path_lba[depth]  = lba;
            path_slot[depth] = slot;
            depth++;
        }

        struct junk_map_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- Insert into leaf ---- */
    struct btree_node_header nhdr;
    memcpy(&nhdr, buf, sizeof(nhdr));

    int ip = jm_leaf_find(buf, nhdr.node_keys, inode_id, offset);
    if(ip >= 0)
    {
        /* Key exists — update in place */
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)ip * rec_sz, &record, rec_sz);
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }
    ip = -(ip + 1); /* insertion point */

    if(nhdr.node_keys < max_keys)
    {
        /* Room in leaf — insert in sorted position */
        uint8_t *data = buf + sizeof(struct btree_node_header);
        memmove(data + ((size_t)ip + 1) * rec_sz, data + (size_t)ip * rec_sz,
                (size_t)(nhdr.node_keys - ip) * rec_sz);
        memcpy(data + (size_t)ip * rec_sz, &record, rec_sz);
        nhdr.node_keys++;
        nhdr.keys_length = (uint16_t)(nhdr.node_keys * rec_sz);
        memcpy(buf, &nhdr, sizeof(nhdr));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* ---- Leaf is full — split ---- */
    uint16_t total     = nhdr.node_keys + 1;
    uint16_t left_cnt  = total / 2;
    uint16_t right_cnt = total - left_cnt;

    /* Build sorted temporary array */
    uint8_t *tmp = malloc((size_t)total * rec_sz);
    if(!tmp) { free(buf); return OBMAFS3_ERR_NOMEM; }
    uint8_t *src = buf + sizeof(struct btree_node_header);
    memcpy(tmp, src, (size_t)ip * rec_sz);
    memcpy(tmp + (size_t)ip * rec_sz, &record, rec_sz);
    memcpy(tmp + ((size_t)ip + 1) * rec_sz, src + (size_t)ip * rec_sz,
           (size_t)(nhdr.node_keys - ip) * rec_sz);

    /* Left stays in current node */
    memcpy(src, tmp, (size_t)left_cnt * rec_sz);
    nhdr.node_keys   = left_cnt;
    nhdr.keys_length = (uint16_t)(left_cnt * rec_sz);

    /* Allocate right node */
    uint64_t right_lba;
    rc = obmafs3_btree_alloc_node(ctx, &ctx->junk_map_hdr, hdr_lba, &right_lba);
    if(rc != OBMAFS3_OK) { fprintf(stderr, "[junk_map] alloc_node for split failed: rc=%d\n", rc); free(tmp); free(buf); return rc; }
    ctx->junk_map_hdr.total_nodes++;

    uint8_t *rbuf = calloc(1, bsz);
    if(!rbuf) { free(tmp); free(buf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return OBMAFS3_ERR_NOMEM; }
    struct btree_node_header rnh;
    memset(&rnh, 0, sizeof(rnh));
    rnh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rnh.record_type = kBtreeDataTypeJunkMapEntry;
    rnh.level       = 0;
    rnh.node_keys   = right_cnt;
    rnh.keys_length = (uint16_t)(right_cnt * rec_sz);
    rnh.left_link   = lba;
    rnh.right_link  = nhdr.right_link;
    memcpy(rbuf, &rnh, sizeof(rnh));
    memcpy(rbuf + sizeof(rnh), tmp + (size_t)left_cnt * rec_sz, (size_t)right_cnt * rec_sz);
    compute_node_checksum(rbuf);

    nhdr.right_link = right_lba;
    memcpy(buf, &nhdr, sizeof(nhdr));
    compute_node_checksum(buf);

    rc = obmafs3_block_write(ctx, lba, buf, bsz);
    if(rc != OBMAFS3_OK) { free(tmp); free(buf); free(rbuf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return rc; }
    rc = obmafs3_block_write(ctx, right_lba, rbuf, bsz);
    free(rbuf);
    if(rc != OBMAFS3_OK) { free(tmp); free(buf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return rc; }

    /* Separator key = first key of right node */
    struct junk_map_record *right_first = (struct junk_map_record *)(tmp + (size_t)left_cnt * rec_sz);
    uint64_t sep_id  = right_first->inode_id;
    uint64_t sep_off = right_first->offset;
    free(tmp);

    /* ---- Propagate split upward ---- */
    size_t idx_sz      = sizeof(struct junk_map_index_entry);
    uint16_t max_idx   = (uint16_t)((bsz - sizeof(struct btree_node_header)) / idx_sz);

    while(depth > 0)
    {
        depth--;
        uint64_t parent_lba  = path_lba[depth];
        uint16_t parent_slot = path_slot[depth];

        rc = obmafs3_block_read(ctx, parent_lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        /* Update parent_slot key to left child's actual minimum */
        {
            uint8_t *lbuf = calloc(1, bsz);
            if(lbuf)
            {
                struct junk_map_index_entry cur_ie;
                memcpy(&cur_ie, buf + sizeof(struct btree_node_header) + (size_t)parent_slot * idx_sz, sizeof(cur_ie));
                if(obmafs3_block_read(ctx, cur_ie.child_lba, lbuf, bsz) == OBMAFS3_OK)
                {
                    struct btree_node_header lnh;
                    memcpy(&lnh, lbuf, sizeof(lnh));
                    if(lnh.node_keys > 0)
                    {
                        if(lnh.level == 0)
                        {
                            /* Child is a leaf — read junk_map_record */
                            struct junk_map_record first_rec;
                            memcpy(&first_rec, lbuf + sizeof(struct btree_node_header), sizeof(first_rec));
                            cur_ie.inode_id = first_rec.inode_id;
                            cur_ie.offset   = first_rec.offset;
                        }
                        else
                        {
                            /* Child is an index node — read junk_map_index_entry */
                            struct junk_map_index_entry first_ie;
                            memcpy(&first_ie, lbuf + sizeof(struct btree_node_header), sizeof(first_ie));
                            cur_ie.inode_id = first_ie.inode_id;
                            cur_ie.offset   = first_ie.offset;
                        }
                        memcpy(buf + sizeof(struct btree_node_header) + (size_t)parent_slot * idx_sz,
                               &cur_ie, sizeof(cur_ie));
                    }
                }
                free(lbuf);
            }
        }

        if(phdr.node_keys < max_idx)
        {
            /* Room in parent — insert separator at parent_slot+1 */
            uint8_t *idx_data = buf + sizeof(struct btree_node_header);
            uint16_t ins = parent_slot + 1;
            memmove(idx_data + ((size_t)ins + 1) * idx_sz, idx_data + (size_t)ins * idx_sz,
                    (size_t)(phdr.node_keys - ins) * idx_sz);
            struct junk_map_index_entry new_ie = { sep_id, sep_off, right_lba };
            memcpy(idx_data + (size_t)ins * idx_sz, &new_ie, idx_sz);
            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * idx_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);
            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            free(buf);
            if(rc != OBMAFS3_OK) return rc;
            /* Flush header — alloc_node modified free list / total_nodes */
            return obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr);
        }

        /* Parent full — split the parent index node */
        {
            /* Build sorted temporary index array with the new entry inserted */
            uint16_t ins = parent_slot + 1;
            uint16_t idx_total = phdr.node_keys + 1;
            uint16_t idx_left  = idx_total / 2;
            uint16_t idx_right = idx_total - idx_left;

            uint8_t *idx_tmp = malloc((size_t)idx_total * idx_sz);
            if(!idx_tmp) { free(buf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return OBMAFS3_ERR_NOMEM; }

            uint8_t *idx_data = buf + sizeof(struct btree_node_header);
            memcpy(idx_tmp, idx_data, (size_t)ins * idx_sz);
            struct junk_map_index_entry new_ie = { sep_id, sep_off, right_lba };
            memcpy(idx_tmp + (size_t)ins * idx_sz, &new_ie, idx_sz);
            memcpy(idx_tmp + ((size_t)ins + 1) * idx_sz, idx_data + (size_t)ins * idx_sz,
                   (size_t)(phdr.node_keys - ins) * idx_sz);

            /* Left stays in current parent node */
            memcpy(idx_data, idx_tmp, (size_t)idx_left * idx_sz);
            phdr.node_keys   = idx_left;
            phdr.keys_length = (uint16_t)(idx_left * idx_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            /* Allocate right sibling for the parent */
            uint64_t idx_right_lba;
            rc = obmafs3_btree_alloc_node(ctx, &ctx->junk_map_hdr, hdr_lba, &idx_right_lba);
            if(rc != OBMAFS3_OK) { free(idx_tmp); free(buf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return rc; }
            ctx->junk_map_hdr.total_nodes++;

            uint8_t *idx_rbuf = calloc(1, bsz);
            if(!idx_rbuf) { free(idx_tmp); free(buf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return OBMAFS3_ERR_NOMEM; }

            struct btree_node_header idx_rnh;
            memset(&idx_rnh, 0, sizeof(idx_rnh));
            idx_rnh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
            idx_rnh.record_type = kBtreeDataTypeJunkMapEntry;
            idx_rnh.level       = phdr.level;
            idx_rnh.node_keys   = idx_right;
            idx_rnh.keys_length = (uint16_t)(idx_right * idx_sz);
            idx_rnh.left_link   = parent_lba;
            idx_rnh.right_link  = phdr.right_link;
            memcpy(idx_rbuf, &idx_rnh, sizeof(idx_rnh));
            memcpy(idx_rbuf + sizeof(idx_rnh), idx_tmp + (size_t)idx_left * idx_sz, (size_t)idx_right * idx_sz);
            compute_node_checksum(idx_rbuf);

            /* Update left parent's right_link */
            phdr.right_link = idx_right_lba;
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            if(rc != OBMAFS3_OK) { free(idx_tmp); free(buf); free(idx_rbuf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return rc; }
            rc = obmafs3_block_write(ctx, idx_right_lba, idx_rbuf, bsz);
            free(idx_rbuf);
            if(rc != OBMAFS3_OK) { free(idx_tmp); free(buf); obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr); return rc; }

            /* New separator = first key of right index sibling */
            struct junk_map_index_entry *right_idx_first =
                (struct junk_map_index_entry *)(idx_tmp + (size_t)idx_left * idx_sz);
            sep_id  = right_idx_first->inode_id;
            sep_off = right_idx_first->offset;
            right_lba = idx_right_lba;
            lba = parent_lba;
            free(idx_tmp);
            /* Continue the loop to insert this separator into the grandparent */
        }
    }

    /* ---- Split propagated to root — create new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_btree_alloc_node(ctx, &ctx->junk_map_hdr, hdr_lba, &new_root_lba);
    if(rc != OBMAFS3_OK) { free(buf); return rc; }
    ctx->junk_map_hdr.total_nodes++;

    /* Left child's min key — read from the left child node.
     * 'lba' points to the left child (the old root or split parent).
     * We need its level to set the new root level = old_level + 1. */
    uint8_t old_level = 0;
    rc = obmafs3_block_read(ctx, lba, buf, bsz);
    if(rc != OBMAFS3_OK) { free(buf); return rc; }
    {
        struct btree_node_header old_nh;
        memcpy(&old_nh, buf, sizeof(old_nh));
        old_level = old_nh.level;
    }

    /* Get the left child's first key for the new root's left index entry */
    struct junk_map_index_entry left_ie;
    if(old_level == 0)
    {
        /* Left child is a leaf — get first record's key */
        struct junk_map_record first_rec;
        memcpy(&first_rec, buf + sizeof(struct btree_node_header), sizeof(first_rec));
        left_ie.inode_id = first_rec.inode_id;
        left_ie.offset   = first_rec.offset;
    }
    else
    {
        /* Left child is an index node — get first index entry's key */
        struct junk_map_index_entry first_ie;
        memcpy(&first_ie, buf + sizeof(struct btree_node_header), sizeof(first_ie));
        left_ie.inode_id = first_ie.inode_id;
        left_ie.offset   = first_ie.offset;
    }
    left_ie.child_lba = lba;

    uint8_t *root_buf = calloc(1, bsz);
    if(!root_buf) { free(buf); return OBMAFS3_ERR_NOMEM; }

    struct btree_node_header root_nh;
    memset(&root_nh, 0, sizeof(root_nh));
    root_nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    root_nh.record_type = kBtreeDataTypeJunkMapEntry;
    root_nh.level       = old_level + 1;
    root_nh.node_keys   = 2;
    root_nh.keys_length = (uint16_t)(2 * idx_sz);
    memcpy(root_buf, &root_nh, sizeof(root_nh));

    struct junk_map_index_entry right_ie = { sep_id, sep_off, right_lba };
    memcpy(root_buf + sizeof(root_nh), &left_ie, idx_sz);
    memcpy(root_buf + sizeof(root_nh) + idx_sz, &right_ie, idx_sz);
    compute_node_checksum(root_buf);

    rc = obmafs3_block_write(ctx, new_root_lba, root_buf, bsz);
    free(root_buf);
    free(buf);
    if(rc != OBMAFS3_OK) return rc;

    ctx->junk_map_hdr.root_node_lba = new_root_lba;
    return obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr);
}

/* ================================================================== */
/*  Lookup                                                             */
/* ================================================================== */

int obmafs3_junk_map_lookup(struct obmafs3_ctx *ctx, uint64_t inode_id, uint64_t offset,
                            struct junk_map_record *record)
{
    if(ctx->sb.junk_map_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint64_t root_lba = ctx->junk_map_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    size_t   bsz = ctx->sb.block_size;
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    uint64_t lba = root_lba;

    /* Navigate to the leaf */
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return OBMAFS3_ERR_BADMAGIC;

        if(nhdr.level == 0) break;

        uint16_t slot = jm_index_find(buf, nhdr.node_keys, inode_id, offset);
        struct junk_map_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Search the leaf: find the last record with (inode_id, rec.offset <= offset) */
    struct btree_node_header nhdr;
    memcpy(&nhdr, buf, sizeof(nhdr));
    const uint8_t *data = buf + sizeof(struct btree_node_header);

    for(int i = (int)nhdr.node_keys - 1; i >= 0; i--)
    {
        struct junk_map_record rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

        if(rec.inode_id != inode_id) continue;
        if(rec.offset > offset) continue;

        /* rec.offset <= offset — check if it covers */
        if(offset < rec.offset + rec.length)
        {
            *record = rec;
            return OBMAFS3_OK;
        }
        /* This record starts before offset but doesn't reach it — no match in this leaf */
        break;
    }

    return OBMAFS3_ERR_NOTFOUND;
}

/* ================================================================== */
/*  Delete all entries for an inode                                    */
/* ================================================================== */

int obmafs3_junk_map_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    if(ctx->sb.junk_map_lba == 0) return OBMAFS3_OK;
    if(ctx->junk_map_hdr.root_node_lba == 0) return OBMAFS3_OK;

    size_t   bsz = ctx->sb.block_size;
    uint64_t hdr_lba = ctx->sb.junk_map_lba;

    /* Collect all offsets for this inode */
    uint64_t *offsets  = NULL;
    uint32_t  count    = 0;
    uint32_t  capacity = 0;

    /* Navigate to the first leaf that could contain inode_id */
    uint64_t lba = ctx->junk_map_hdr.root_node_lba;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); free(offsets); return rc; }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.level == 0) break;

        uint16_t slot = jm_index_find(buf, nhdr.node_keys, inode_id, 0);
        struct junk_map_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain collecting entries */
    while(1)
    {
        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct junk_map_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if(rec.inode_id == inode_id)
            {
                if(count >= capacity)
                {
                    capacity = capacity ? capacity * 2 : 64;
                    uint64_t *no = realloc(offsets, capacity * sizeof(uint64_t));
                    if(!no) { free(buf); free(offsets); return OBMAFS3_ERR_NOMEM; }
                    offsets = no;
                }
                offsets[count++] = rec.offset;
            }
            else if(rec.inode_id > inode_id)
                goto scan_done;
        }

        if(nhdr.right_link == 0) break;
        int rc = obmafs3_block_read(ctx, nhdr.right_link, buf, bsz);
        if(rc != OBMAFS3_OK) break;
    }

scan_done:
    free(buf);

    if(count == 0)
    {
        free(offsets);
        return OBMAFS3_OK;
    }

    /* Delete in reverse order to minimize restructuring.
     * For simplicity we just remove entries from leaves without rebalancing.
     * The freed space will be reused on future inserts. */
    for(uint32_t i = count; i > 0; i--)
    {
        /* Navigate to the leaf containing this entry and remove it */
        lba = ctx->junk_map_hdr.root_node_lba;
        buf = calloc(1, bsz);
        if(!buf) { free(offsets); return OBMAFS3_ERR_NOMEM; }

        while(1)
        {
            int rc = obmafs3_block_read(ctx, lba, buf, bsz);
            if(rc != OBMAFS3_OK) { free(buf); free(offsets); return rc; }

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));

            if(nhdr.level > 0)
            {
                uint16_t slot = jm_index_find(buf, nhdr.node_keys, inode_id, offsets[i - 1]);
                struct junk_map_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
                lba = ie.child_lba;
                continue;
            }

            /* Leaf — find and remove the record */
            int idx = jm_leaf_find(buf, nhdr.node_keys, inode_id, offsets[i - 1]);
            if(idx >= 0 && nhdr.node_keys > 0)
            {
                uint8_t *data   = buf + sizeof(struct btree_node_header);
                size_t   rec_sz = sizeof(struct junk_map_record);
                memmove(data + (size_t)idx * rec_sz, data + ((size_t)idx + 1) * rec_sz,
                        (size_t)(nhdr.node_keys - idx - 1) * rec_sz);
                nhdr.node_keys--;
                nhdr.keys_length = (uint16_t)(nhdr.node_keys * rec_sz);
                memcpy(buf, &nhdr, sizeof(nhdr));
                compute_node_checksum(buf);
                obmafs3_block_write(ctx, lba, buf, bsz);
            }
            break;
        }
        free(buf);
    }

    free(offsets);

    /* Update header */
    return obmafs3_btree_header_write(ctx, hdr_lba, &ctx->junk_map_hdr);
}
