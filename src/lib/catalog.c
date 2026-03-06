// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : catalog.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 Catalog B+Tree operations.
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
 * Catalog tree: proper B+Tree sorted by (parent_id, name).
 *   level 0  → leaf nodes storing sorted catalog_record entries.
 *   level >0 → index nodes storing sorted catalog_index_entry entries.
 */
#include "btree_internal.h"
#include "debug.h"

/* ------------------------------------------------------------------ */
/*  Catalog B+Tree helpers                                             */
/* ------------------------------------------------------------------ */

/** Maximum catalog_record entries in a leaf node. */
static uint16_t catalog_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct catalog_record));
}

/** Maximum catalog_index_entry entries in an index node. */
static uint16_t catalog_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct catalog_index_entry));
}

/**
 * Compare two catalog keys (parent_id, name).
 * Returns < 0, 0, or > 0.
 */
static int catalog_key_cmp(uint64_t pid_a, const char *name_a, uint64_t pid_b, const char *name_b)
{
    if(pid_a < pid_b) return -1;
    if(pid_a > pid_b) return 1;
    return strcmp(name_a, name_b);
}

/**
 * Binary search for (parent_id, name) in a catalog leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int catalog_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t parent_id, const char *name)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int                   mid = lo + (hi - lo) / 2;
        struct catalog_record rec;
        memcpy(&rec, data + (size_t)mid * sizeof(rec), sizeof(rec));

        int cmp = catalog_key_cmp(rec.parent_id, rec.name, parent_id, name);
        if(cmp == 0) return mid;
        if(cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return -(lo + 1);
}

/**
 * Binary search in a catalog index node for the child covering
 * (parent_id, name).  Returns the slot index of the child to follow.
 */
static uint16_t catalog_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t parent_id, const char *name)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int                        mid = lo + (hi - lo) / 2;
        struct catalog_index_entry ie;
        memcpy(&ie, data + (size_t)mid * sizeof(ie), sizeof(ie));

        int cmp = catalog_key_cmp(ie.parent_id, ie.name, parent_id, name);
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

#define CATALOG_BTREE_MAX_DEPTH 8

struct catalog_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/* ------------------------------------------------------------------ */
/*  Catalog lookup (B+Tree traversal)                                  */
/* ------------------------------------------------------------------ */

/**
 * Look up a catalog entry by parent inode ID and name.
 *
 * Traverses the catalog B+Tree from root to leaf to find the entry
 * matching the composite key (@p parent_id, @p name).
 *
 * @param ctx        Filesystem context.
 * @param parent_id  Inode ID of the parent directory.
 * @param name       Entry name to search for.
 * @param entry      Output catalog record.
 * @return @c OBMAFS3_OK if found, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_catalog_lookup(struct obmafs3_ctx *ctx, uint64_t parent_id, const char *name, struct catalog_record *entry)
{
    uint64_t lba = ctx->catalog_hdr.root_node_lba;
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
            /* Index node: follow the appropriate child pointer */
            uint16_t                   slot = catalog_index_find(buf, hdr.node_keys, parent_id, name);
            struct catalog_index_entry ie;
            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            /* Leaf node: binary search for exact match */
            int idx = catalog_leaf_find(buf, hdr.node_keys, parent_id, name);
            if(idx >= 0)
            {
                memcpy(entry, buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(*entry), sizeof(*entry));
                return OBMAFS3_OK;
            }
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Catalog list (B+Tree traversal with right-link scan)               */
/* ------------------------------------------------------------------ */

/**
 * List all catalog entries under a parent directory.
 *
 * Traverses the catalog B+Tree to the leftmost leaf containing
 * @p parent_id and scans the leaf chain, collecting all entries with
 * a matching parent.
 *
 * @param ctx        Filesystem context.
 * @param parent_id  Inode ID of the parent directory.
 * @param entries    Output array of catalog records (caller frees via
 *                   @c obmafs3_catalog_list_free).
 * @param count      Output number of entries.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_catalog_list(struct obmafs3_ctx *ctx, uint64_t parent_id, struct catalog_record **entries, uint32_t *count)
{
    *entries = NULL;
    *count   = 0;

    uint64_t lba = ctx->catalog_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Descend to the leftmost leaf that could contain parent_id.
     * Use an empty name so we land at the first entry for this parent. */
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
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

        uint16_t                   slot = catalog_index_find(buf, hdr.node_keys, parent_id, "");
        struct catalog_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Now scan the leaf chain collecting entries with matching parent_id */
    struct catalog_record *result = NULL;
    uint32_t               n      = 0;
    uint32_t               cap    = 0;

    while(lba != 0)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);

        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct catalog_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if(rec.parent_id < parent_id) continue;
            if(rec.parent_id > parent_id) goto done; /* sorted — no more matches possible */

            /* rec.parent_id == parent_id — collect it */
            if(n >= cap)
            {
                cap                        = (cap == 0) ? 16 : cap * 2;
                struct catalog_record *tmp = realloc(result, cap * sizeof(*tmp));
                if(!tmp)
                {
                    free(buf);
                    free(result);
                    DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                }
                result = tmp;
            }
            result[n++] = rec;
        }

        /* Follow right_link to next leaf */
        lba = hdr.right_link;
        if(lba != 0)
        {
            int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK)
            {
                free(buf);
                free(result);
                return rc;
            }
        }
    }

done:
    free(buf);
    *entries = result;
    *count   = n;
    return OBMAFS3_OK;
}

/**
 * Free a catalog entry array returned by @c obmafs3_catalog_list.
 *
 * @param entries  Array to free (may be NULL).
 */
void obmafs3_catalog_list_free(struct catalog_record *entries) { free(entries); }

/* ------------------------------------------------------------------ */
/*  Resolve inode_id → full path via catalog scan                      */
/* ------------------------------------------------------------------ */

/**
 * Find a catalog entry with the given inode_id by scanning all leaves.
 * The catalog is sorted by (parent_id, name) so we must do a full scan.
 * Returns OBMAFS3_OK and fills *out on success, OBMAFS3_ERR_NOTFOUND otherwise.
 */
static int catalog_find_by_inode(struct obmafs3_ctx *ctx, uint64_t target_inode, struct catalog_record *out)
{
    uint64_t lba = ctx->catalog_hdr.root_node_lba;
    if(lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Descend to leftmost leaf */
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
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

        /* Follow leftmost child (slot 0) */
        struct catalog_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain */
    while(lba != 0)
    {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct catalog_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
            if(rec.inode_id == target_inode)
            {
                *out = rec;
                free(buf);
                return OBMAFS3_OK;
            }
        }

        lba = hdr.right_link;
        if(lba != 0)
        {
            int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK)
            {
                free(buf);
                return rc;
            }
        }
    }

    free(buf);
    return OBMAFS3_ERR_NOTFOUND;
}

/**
 * Resolve an inode ID to its full filesystem path.
 *
 * Walks the catalog bottom-up from @p inode_id to the root, collecting
 * name components, then assembles them into an absolute path.
 *
 * @param ctx           Filesystem context.
 * @param inode_id      Inode ID to resolve.
 * @param path_buf      Output path buffer.
 * @param path_buf_size Size of @p path_buf in bytes.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_resolve_inode_path(struct obmafs3_ctx *ctx, uint64_t inode_id, char *path_buf, size_t path_buf_size)
{
    if(path_buf_size == 0) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    if(inode_id == OBMAFS3_ROOT_INODE_ID)
    {
        path_buf[0] = '/';
        path_buf[1] = '\0';
        return OBMAFS3_OK;
    }

    /* Collect path components bottom-up (max depth 256) */
    const int MAX_DEPTH     = 256;
    char (*components)[256] = malloc((size_t)MAX_DEPTH * 256);
    if(!components) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    int      depth = 0;
    uint64_t cur   = inode_id;

    while(cur != OBMAFS3_ROOT_INODE_ID && depth < MAX_DEPTH)
    {
        struct catalog_record cat;
        int                   rc = catalog_find_by_inode(ctx, cur, &cat);
        if(rc != OBMAFS3_OK)
        {
            free(components);
            return rc;
        }
        memcpy(components[depth], cat.name, 256);
        depth++;
        cur = cat.parent_id;
    }

    /* Build path top-down */
    size_t pos = 0;
    for(int i = depth - 1; i >= 0; i--)
    {
        size_t nlen = strlen(components[i]);
        if(pos + 1 + nlen + 1 > path_buf_size)
        {
            free(components);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }
        path_buf[pos++] = '/';
        memcpy(path_buf + pos, components[i], nlen);
        pos += nlen;
    }
    path_buf[pos] = '\0';

    free(components);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Catalog insert (B+Tree)                                            */
/* ------------------------------------------------------------------ */

/**
 * Insert a catalog record into the B+Tree.
 *
 * Handles leaf splitting and root promotion when the target leaf is
 * full.  Updates the tree header on disk after insertion.
 *
 * @param ctx    Filesystem context.
 * @param entry  Pointer to the catalog record to insert.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_catalog_insert(struct obmafs3_ctx *ctx, const struct catalog_record *entry)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->catalog_hdr.root_node_lba;
    int      rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if(root_lba == 0)
    {
        uint64_t new_lba;
        rc = obmafs3_btree_alloc_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, &new_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
        memset(buf, 0, bsz);

        struct btree_node_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        hdr.record_type = kBtreeDataTypeFilename;
        hdr.level       = 0;
        hdr.node_keys   = 1;
        hdr.keys_length = (uint16_t)sizeof(struct catalog_record);
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), entry, sizeof(*entry));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, new_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        ctx->catalog_hdr.root_node_lba = new_lba;
        ctx->catalog_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct catalog_btree_path path[CATALOG_BTREE_MAX_DEPTH];
    int                       depth = 0;
    uint64_t                  lba   = root_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(hdr.level == 0) break; /* reached leaf; buf holds it at lba */

        if(depth >= CATALOG_BTREE_MAX_DEPTH) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

        uint16_t slot    = catalog_index_find(buf, hdr.node_keys, entry->parent_id, entry->name);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct catalog_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for duplicate */
    int idx = catalog_leaf_find(buf, leaf_hdr.node_keys, entry->parent_id, entry->name);
    if(idx >= 0)
    {
        /* Update in place */
        memcpy(buf + sizeof(struct btree_node_header) + (size_t)idx * sizeof(struct catalog_record), entry,
               sizeof(*entry));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        return rc;
    }

    /* Not found — insert.  insert_pos is where the new record goes. */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = catalog_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct catalog_record);

    if(leaf_hdr.node_keys < max_leaf)
    {
        /* Room in leaf */
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
    uint16_t               total = max_leaf + 1;
    struct catalog_record *all   = calloc(total, rec_sz);
    if(!all) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

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
    rc = obmafs3_btree_alloc_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, &new_leaf_lba);
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
    nh.record_type = kBtreeDataTypeFilename;
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

    /* Separator key for the new right child */
    struct catalog_index_entry push_ie;
    memset(&push_ie, 0, sizeof(push_ie));
    push_ie.parent_id = all[left_count].parent_id;
    strncpy(push_ie.name, all[left_count].name, sizeof(push_ie.name) - 1);
    push_ie.child_lba = new_leaf_lba;

    struct catalog_index_entry left_ie;
    memset(&left_ie, 0, sizeof(left_ie));
    left_ie.parent_id = all[0].parent_id;
    strncpy(left_ie.name, all[0].name, sizeof(left_ie.name) - 1);
    left_ie.child_lba = lba;

    free(all);
    ctx->catalog_hdr.total_nodes++;

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

        uint16_t max_idx    = catalog_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct catalog_index_entry);

        if(phdr.node_keys < max_idx)
        {
            /* Room in parent — insert */
            uint8_t *id = buf + sizeof(struct btree_node_header);

            /* Update the key at parent_slot to the left child's
             * actual minimum.  Without this, the parent key can
             * be stale (higher than the true minimum) after the
             * leftmost child accumulated entries with keys below
             * the original index key. */
            struct catalog_index_entry cat_upd;
            memcpy(&cat_upd, id + (size_t)parent_slot * ie_sz, sizeof(cat_upd));
            cat_upd.parent_id = left_ie.parent_id;
            strncpy(cat_upd.name, left_ie.name, sizeof(cat_upd.name) - 1);
            memcpy(id + (size_t)parent_slot * ie_sz, &cat_upd, sizeof(cat_upd));

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz, id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);

            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, sizeof(push_ie));

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            if(rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
        }

        /* Parent is full — split the index node */
        uint16_t                    idx_total = max_idx + 1;
        struct catalog_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        uint8_t *id = buf + sizeof(struct btree_node_header);

        /* Update the key at parent_slot to the left child's
         * actual minimum before building the merged array. */
        struct catalog_index_entry cat_upd;
        memcpy(&cat_upd, id + (size_t)parent_slot * ie_sz, sizeof(cat_upd));
        cat_upd.parent_id = left_ie.parent_id;
        strncpy(cat_upd.name, left_ie.name, sizeof(cat_upd.name) - 1);
        memcpy(id + (size_t)parent_slot * ie_sz, &cat_upd, sizeof(cat_upd));

        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        aie[idx_insert] = push_ie;
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        /* Allocate new index node before writing so we can set sibling links */
        uint64_t idx_old_right = phdr.right_link;
        uint64_t new_idx_lba;
        rc = obmafs3_btree_alloc_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, &new_idx_lba);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        /* Rewrite old index with left half */
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
        nih.record_type = kBtreeDataTypeFilename;
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

        /* Prepare for next level up */
        memset(&push_ie, 0, sizeof(push_ie));
        push_ie.parent_id = aie[il].parent_id;
        strncpy(push_ie.name, aie[il].name, sizeof(push_ie.name) - 1);
        push_ie.child_lba = new_idx_lba;

        memset(&left_ie, 0, sizeof(left_ie));
        left_ie.parent_id = aie[0].parent_id;
        strncpy(left_ie.name, aie[0].name, sizeof(left_ie.name) - 1);
        left_ie.child_lba = parent_lba;

        free(aie);
        ctx->catalog_hdr.total_nodes++;

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
    rc = obmafs3_btree_alloc_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, &new_root_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Read old root to get its level */
    rc = obmafs3_block_read(ctx, left_ie.child_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, bsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeFilename;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * sizeof(struct catalog_index_entry));
    memcpy(buf, &rh, sizeof(rh));

    struct catalog_index_entry roots[2];
    roots[0] = left_ie;
    roots[1] = push_ie;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    ctx->catalog_hdr.root_node_lba = new_root_lba;
    ctx->catalog_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
}

/* ------------------------------------------------------------------ */
/*  Catalog delete (B+Tree)                                            */
/* ------------------------------------------------------------------ */

/**
 * Delete a catalog record from the B+Tree.
 *
 * Locates the leaf containing the entry keyed by (@p parent_id,
 * @p name) and removes it.  Frees empty leaf nodes and updates the
 * tree header.
 *
 * @param ctx        Filesystem context.
 * @param parent_id  Inode ID of the parent directory.
 * @param name       Entry name to delete.
 * @return @c OBMAFS3_OK on success, @c OBMAFS3_ERR_NOTFOUND if absent,
 *         or another error code on failure.
 */
int obmafs3_catalog_delete(struct obmafs3_ctx *ctx, uint64_t parent_id, const char *name)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->catalog_hdr.root_node_lba;
    int      rc;

    if(root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct catalog_btree_path path[CATALOG_BTREE_MAX_DEPTH];
    int                       depth = 0;
    uint64_t                  lba   = root_lba;

    /* Traverse to leaf */
    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(hdr.level == 0) break;

        if(depth >= CATALOG_BTREE_MAX_DEPTH) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

        uint16_t slot    = catalog_index_find(buf, hdr.node_keys, parent_id, name);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct catalog_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* buf holds the leaf at lba */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = catalog_leaf_find(buf, leaf_hdr.node_keys, parent_id, name);
    if(idx < 0) return OBMAFS3_ERR_NOTFOUND;

    size_t rec_sz = sizeof(struct catalog_record);

    /* Remove the record from the leaf */
    leaf_hdr.node_keys--;

    if(leaf_hdr.node_keys == 0)
    {
        /* Leaf is now empty */
        if(depth == 0)
        {
            /* Root leaf empty — tree is empty */
            ctx->catalog_hdr.root_node_lba = 0;
            ctx->catalog_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
            obmafs3_btree_free_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, lba);
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

            /* Remove child pointer from parent */
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
            size_t   ie_sz = sizeof(struct catalog_index_entry);

            if(pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz, pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if(phdr.node_keys == 0 && depth == 1)
            {
                /* Root index has no children — tree is empty */
                ctx->catalog_hdr.root_node_lba = 0;
                ctx->catalog_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
                obmafs3_btree_free_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, lba);
                obmafs3_btree_free_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, plba);
            }
            else if(phdr.node_keys == 1 && depth == 1)
            {
                /* Root index has one child — collapse tree height */
                struct catalog_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->catalog_hdr.root_node_lba = remaining.child_lba;
                ctx->catalog_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
                obmafs3_btree_free_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, lba);
                obmafs3_btree_free_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, plba);
            }
            else
            {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if(rc == OBMAFS3_OK)
                {
                    ctx->catalog_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
                }
                obmafs3_btree_free_node(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, lba);
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

    return rc;
}
