/*
 * btree.c - OBMAFS3 B+Tree operations
 *
 * Catalog tree: proper B+Tree sorted by (parent_id, name).
 *   level 0  → leaf nodes storing sorted catalog_record entries.
 *   level >0 → index nodes storing sorted catalog_index_entry entries.
 *
 * Inode tree: proper B+Tree (btrfs-style).
 *   level 0  → leaf nodes storing sorted inode_record entries.
 *   level >0 → index nodes storing sorted btree_index_entry entries.
 *   Splits propagate upward; the root grows when it splits.
 */
#include "obmafs.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Compute and store the checksum for a btree node block.
 * The node size is derived from keys_length in the header. */
static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr = (struct btree_node_header *)buf;
    size_t data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

int obmafs3_btree_header_read(struct obmafs3_ctx *ctx, uint64_t lba,
                              struct btree_header *hdr)
{
    int cs_ok = 0;
    int rc = obmafs3_btree_header_read_lenient(ctx, lba, hdr, &cs_ok);
    if (rc != OBMAFS3_OK)
        return rc;
    return cs_ok ? OBMAFS3_OK : OBMAFS3_ERR_CHECKSUM;
}

int obmafs3_btree_header_read_lenient(struct obmafs3_ctx *ctx, uint64_t lba,
                                      struct btree_header *hdr,
                                      int *checksum_ok)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }

    memcpy(hdr, buf, sizeof(*hdr));
    free(buf);

    if (hdr->magic != OBMAFS3_BTREE_HDR_MAGIC)
        return OBMAFS3_ERR_BADMAGIC;

    /* Verify checksum: save stored checksum, zero field, recompute */
    uint8_t stored[32];
    memcpy(stored, hdr->checksum, 32);
    memset(hdr->checksum, 0, 32);
    uint8_t computed[32];
    obmafs3_checksum_block(hdr, sizeof(*hdr), computed);
    memcpy(hdr->checksum, stored, 32);
    *checksum_ok = (memcmp(stored, computed, 32) == 0);

    return OBMAFS3_OK;
}

int obmafs3_btree_header_write(struct obmafs3_ctx *ctx, uint64_t lba,
                               const struct btree_header *hdr)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    memcpy(buf, hdr, sizeof(*hdr));

    /* Compute checksum: zero field, hash the struct, store result */
    struct btree_header *hdr_buf = (struct btree_header *)buf;
    memset(hdr_buf->checksum, 0, sizeof(hdr_buf->checksum));
    obmafs3_checksum_block(buf, sizeof(*hdr), hdr_buf->checksum);

    int rc = obmafs3_block_write(ctx, lba, buf, (size_t)ctx->sb.block_size);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Catalog B+Tree helpers                                             */
/* ------------------------------------------------------------------ */

/** Maximum catalog_record entries in a leaf node. */
static uint16_t catalog_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct catalog_record));
}

/** Maximum catalog_index_entry entries in an index node. */
static uint16_t catalog_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct catalog_index_entry));
}

/**
 * Compare two catalog keys (parent_id, name).
 * Returns < 0, 0, or > 0.
 */
static int catalog_key_cmp(uint64_t pid_a, const char *name_a,
                           uint64_t pid_b, const char *name_b)
{
    if (pid_a < pid_b) return -1;
    if (pid_a > pid_b) return  1;
    return strcmp(name_a, name_b);
}

/**
 * Binary search for (parent_id, name) in a catalog leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int catalog_leaf_find(const uint8_t *buf, uint16_t node_keys,
                             uint64_t parent_id, const char *name)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        struct catalog_record rec;
        memcpy(&rec, data + (size_t)mid * sizeof(rec), sizeof(rec));

        int cmp = catalog_key_cmp(rec.parent_id, rec.name,
                                  parent_id, name);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
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
static uint16_t catalog_index_find(const uint8_t *buf, uint16_t node_keys,
                                   uint64_t parent_id, const char *name)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;
    uint16_t result = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        struct catalog_index_entry ie;
        memcpy(&ie, data + (size_t)mid * sizeof(ie), sizeof(ie));

        int cmp = catalog_key_cmp(ie.parent_id, ie.name,
                                  parent_id, name);
        if (cmp <= 0) {
            result = (uint16_t)mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return result;
}

#define CATALOG_BTREE_MAX_DEPTH 8

struct catalog_btree_path {
    uint64_t lba;
    uint16_t slot;
};

/* ------------------------------------------------------------------ */
/*  Catalog lookup (B+Tree traversal)                                  */
/* ------------------------------------------------------------------ */

int obmafs3_catalog_lookup(struct obmafs3_ctx *ctx, uint64_t parent_id,
                           const char *name,
                           struct catalog_record *entry)
{
    uint64_t lba = ctx->catalog_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    while (1) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level > 0) {
            /* Index node: follow the appropriate child pointer */
            uint16_t slot = catalog_index_find(buf, hdr.node_keys,
                                               parent_id, name);
            struct catalog_index_entry ie;
            memcpy(&ie,
                   buf + sizeof(struct btree_node_header)
                       + (size_t)slot * sizeof(ie),
                   sizeof(ie));
            lba = ie.child_lba;
        } else {
            /* Leaf node: binary search for exact match */
            int idx = catalog_leaf_find(buf, hdr.node_keys,
                                        parent_id, name);
            if (idx >= 0) {
                memcpy(entry,
                       buf + sizeof(struct btree_node_header)
                           + (size_t)idx * sizeof(*entry),
                       sizeof(*entry));
                free(buf);
                return OBMAFS3_OK;
            }
            free(buf);
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Catalog list (B+Tree traversal with right-link scan)               */
/* ------------------------------------------------------------------ */

int obmafs3_catalog_list(struct obmafs3_ctx *ctx, uint64_t parent_id,
                         struct catalog_record **entries,
                         uint32_t *count)
{
    *entries = NULL;
    *count   = 0;

    uint64_t lba = ctx->catalog_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    /* Descend to the leftmost leaf that could contain parent_id.
     * Use an empty name so we land at the first entry for this parent. */
    while (1) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break;

        uint16_t slot = catalog_index_find(buf, hdr.node_keys,
                                           parent_id, "");
        struct catalog_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* Now scan the leaf chain collecting entries with matching parent_id */
    struct catalog_record *result = NULL;
    uint32_t n   = 0;
    uint32_t cap = 0;

    while (lba != 0) {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);

        for (uint16_t i = 0; i < hdr.node_keys; i++) {
            struct catalog_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if (rec.parent_id < parent_id)
                continue;
            if (rec.parent_id > parent_id)
                goto done;   /* sorted — no more matches possible */

            /* rec.parent_id == parent_id — collect it */
            if (n >= cap) {
                cap = (cap == 0) ? 16 : cap * 2;
                struct catalog_record *tmp =
                    realloc(result, cap * sizeof(*tmp));
                if (!tmp) {
                    free(buf);
                    free(result);
                    return OBMAFS3_ERR_NOMEM;
                }
                result = tmp;
            }
            result[n++] = rec;
        }

        /* Follow right_link to next leaf */
        lba = hdr.right_link;
        if (lba != 0) {
            int rc = obmafs3_block_read(ctx, lba, buf,
                                        (size_t)ctx->sb.block_size);
            if (rc != OBMAFS3_OK) {
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

void obmafs3_catalog_list_free(struct catalog_record *entries)
{
    free(entries);
}

/* ------------------------------------------------------------------ */
/*  Resolve inode_id → full path via catalog scan                      */
/* ------------------------------------------------------------------ */

/**
 * Find a catalog entry with the given inode_id by scanning all leaves.
 * The catalog is sorted by (parent_id, name) so we must do a full scan.
 * Returns OBMAFS3_OK and fills *out on success, OBMAFS3_ERR_NOTFOUND otherwise.
 */
static int catalog_find_by_inode(struct obmafs3_ctx *ctx,
                                uint64_t target_inode,
                                struct catalog_record *out)
{
    uint64_t lba = ctx->catalog_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    /* Descend to leftmost leaf */
    while (1) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }
        if (hdr.level == 0) break;

        /* Follow leftmost child (slot 0) */
        struct catalog_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain */
    while (lba != 0) {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for (uint16_t i = 0; i < hdr.node_keys; i++) {
            struct catalog_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
            if (rec.inode_id == target_inode) {
                *out = rec;
                free(buf);
                return OBMAFS3_OK;
            }
        }

        lba = hdr.right_link;
        if (lba != 0) {
            int rc = obmafs3_block_read(ctx, lba, buf,
                                        (size_t)ctx->sb.block_size);
            if (rc != OBMAFS3_OK) { free(buf); return rc; }
        }
    }

    free(buf);
    return OBMAFS3_ERR_NOTFOUND;
}

int obmafs3_resolve_inode_path(struct obmafs3_ctx *ctx,
                               uint64_t inode_id,
                               char *path_buf, size_t path_buf_size)
{
    if (path_buf_size == 0)
        return OBMAFS3_ERR_NOMEM;

    if (inode_id == OBMAFS3_ROOT_INODE_ID) {
        path_buf[0] = '/';
        path_buf[1] = '\0';
        return OBMAFS3_OK;
    }

    /* Collect path components bottom-up (max depth 256) */
    const int MAX_DEPTH = 256;
    char (*components)[256] = malloc((size_t)MAX_DEPTH * 256);
    if (!components)
        return OBMAFS3_ERR_NOMEM;

    int depth = 0;
    uint64_t cur = inode_id;

    while (cur != OBMAFS3_ROOT_INODE_ID && depth < MAX_DEPTH) {
        struct catalog_record cat;
        int rc = catalog_find_by_inode(ctx, cur, &cat);
        if (rc != OBMAFS3_OK) {
            free(components);
            return rc;
        }
        memcpy(components[depth], cat.name, 256);
        depth++;
        cur = cat.parent_id;
    }

    /* Build path top-down */
    size_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        size_t nlen = strlen(components[i]);
        if (pos + 1 + nlen + 1 > path_buf_size) {
            free(components);
            return OBMAFS3_ERR_NOMEM;
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
/*  Inode B+Tree helpers                                               */
/* ------------------------------------------------------------------ */

/** Maximum inode_record entries in a leaf node. */
static uint16_t inode_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct inode_record));
}

/** Maximum btree_index_entry entries in an index node. */
static uint16_t inode_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct btree_index_entry));
}

/**
 * Binary search for inode_id in a leaf node buffer.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int inode_leaf_find(const uint8_t *buf, uint16_t node_keys,
                           uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t mid_id;
        memcpy(&mid_id, data + (size_t)mid * sizeof(struct inode_record),
               sizeof(mid_id));
        if (mid_id == inode_id)
            return mid;
        if (mid_id < inode_id)
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
static uint16_t inode_index_find(const uint8_t *buf, uint16_t node_keys,
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

#define INODE_BTREE_MAX_DEPTH 8

struct inode_btree_path {
    uint64_t lba;
    uint16_t slot;
};

/* ------------------------------------------------------------------ */
/*  Inode get (B+Tree traversal)                                       */
/* ------------------------------------------------------------------ */

int obmafs3_inode_get(struct obmafs3_ctx *ctx, uint64_t inode_id,
                      struct inode_record *inode)
{
    uint64_t lba = ctx->inode_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    while (1) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level > 0) {
            /* Index node: follow the appropriate child pointer */
            uint16_t slot = inode_index_find(buf, hdr.node_keys,
                                             inode_id);
            struct btree_index_entry ie;
            memcpy(&ie,
                   buf + sizeof(struct btree_node_header)
                       + (size_t)slot * sizeof(ie),
                   sizeof(ie));
            lba = ie.child_lba;
        } else {
            /* Leaf node: binary search for exact match */
            int idx = inode_leaf_find(buf, hdr.node_keys, inode_id);
            if (idx >= 0) {
                struct inode_record rec;
                memcpy(&rec,
                       buf + sizeof(struct btree_node_header)
                           + (size_t)idx * sizeof(rec),
                       sizeof(rec));
                memcpy(inode, &rec, sizeof(*inode));
                free(buf);
                return OBMAFS3_OK;
            }
            free(buf);
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Block allocation                                                   */
/* ------------------------------------------------------------------ */

int obmafs3_alloc_block(struct obmafs3_ctx *ctx, uint64_t *lba)
{
    return obmafs3_alloc_blocks(ctx, 1, lba);
}

int obmafs3_alloc_blocks(struct obmafs3_ctx *ctx, uint64_t count,
                         uint64_t *start_lba)
{
    /* Find contiguous free blocks via the bitmap */
    int rc = obmafs3_bitmap_find_free(ctx, count, start_lba);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Mark them as allocated */
    obmafs3_bitmap_set(ctx, *start_lba, count);

    /* Persist the bitmap */
    rc = obmafs3_bitmap_write(ctx);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Keep next_free_lba as a hint for future allocations */
    if (*start_lba + count > ctx->sb.next_free_lba)
        ctx->sb.next_free_lba = *start_lba + count;

    return obmafs3_sb_write(ctx->fd, &ctx->sb);
}

uint64_t obmafs3_alloc_inode_id(struct obmafs3_ctx *ctx)
{
    uint64_t id = ctx->sb.next_inode_id;
    ctx->sb.next_inode_id++;
    obmafs3_sb_write(ctx->fd, &ctx->sb);
    return id;
}

/* ------------------------------------------------------------------ */
/*  Catalog insert (B+Tree)                                            */
/* ------------------------------------------------------------------ */

int obmafs3_catalog_insert(struct obmafs3_ctx *ctx,
                           const struct catalog_record *entry)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->catalog_hdr.root_node_lba;
    int rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if (root_lba == 0) {
        uint64_t new_lba;
        rc = obmafs3_alloc_block(ctx, &new_lba);
        if (rc != OBMAFS3_OK)
            return rc;

        uint8_t *buf = calloc(1, bsz);
        if (!buf)
            return OBMAFS3_ERR_NOMEM;

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
        free(buf);
        if (rc != OBMAFS3_OK)
            return rc;

        ctx->catalog_hdr.root_node_lba = new_lba;
        ctx->catalog_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba,
                                          &ctx->catalog_hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct catalog_btree_path path[CATALOG_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break; /* reached leaf; buf holds it at lba */

        if (depth >= CATALOG_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = catalog_index_find(buf, hdr.node_keys,
                                           entry->parent_id,
                                           entry->name);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct catalog_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for duplicate */
    int idx = catalog_leaf_find(buf, leaf_hdr.node_keys,
                                entry->parent_id, entry->name);
    if (idx >= 0) {
        /* Update in place */
        memcpy(buf + sizeof(struct btree_node_header)
                   + (size_t)idx * sizeof(struct catalog_record),
               entry, sizeof(*entry));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* Not found — insert.  insert_pos is where the new record goes. */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = catalog_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct catalog_record);

    if (leaf_hdr.node_keys < max_leaf) {
        /* Room in leaf */
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
    struct catalog_record *all = calloc(total, rec_sz);
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
    nh.record_type = kBtreeDataTypeFilename;
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

        uint16_t max_idx    = catalog_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct catalog_index_entry);

        if (phdr.node_keys < max_idx) {
            /* Room in parent — insert */
            uint8_t *id = buf + sizeof(struct btree_node_header);

            if (idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz,
                        id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)idx_insert) * ie_sz);

            memcpy(id + (size_t)idx_insert * ie_sz,
                   &push_ie, sizeof(push_ie));

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            free(buf);
            if (rc != OBMAFS3_OK)
                return rc;
            return obmafs3_btree_header_write(
                ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
        }

        /* Parent is full — split the index node */
        uint16_t idx_total = max_idx + 1;
        struct catalog_index_entry *aie = calloc(idx_total, ie_sz);
        if (!aie) {
            free(buf);
            return OBMAFS3_ERR_NOMEM;
        }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        aie[idx_insert] = push_ie;
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
        nih.record_type = kBtreeDataTypeFilename;
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
    }

    /* ---- Create new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_alloc_block(ctx, &new_root_lba);
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }

    /* Read old root to get its level */
    rc = obmafs3_block_read(ctx, left_ie.child_lba, buf, bsz);
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
    free(buf);
    if (rc != OBMAFS3_OK)
        return rc;

    ctx->catalog_hdr.root_node_lba = new_root_lba;
    ctx->catalog_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba,
                                      &ctx->catalog_hdr);
}

/* ------------------------------------------------------------------ */
/*  Catalog delete (B+Tree)                                            */
/* ------------------------------------------------------------------ */

int obmafs3_catalog_delete(struct obmafs3_ctx *ctx, uint64_t parent_id,
                           const char *name)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->catalog_hdr.root_node_lba;
    int rc;

    if (root_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct catalog_btree_path path[CATALOG_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    /* Traverse to leaf */
    while (1) {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break;

        if (depth >= CATALOG_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = catalog_index_find(buf, hdr.node_keys,
                                           parent_id, name);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct catalog_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* buf holds the leaf at lba */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = catalog_leaf_find(buf, leaf_hdr.node_keys,
                                parent_id, name);
    if (idx < 0) {
        free(buf);
        return OBMAFS3_ERR_NOTFOUND;
    }

    size_t rec_sz = sizeof(struct catalog_record);

    /* Remove the record from the leaf */
    leaf_hdr.node_keys--;

    if (leaf_hdr.node_keys == 0) {
        /* Leaf is now empty */
        if (depth == 0) {
            /* Root leaf empty — tree is empty */
            ctx->catalog_hdr.root_node_lba = 0;
            ctx->catalog_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba,
                                            &ctx->catalog_hdr);
            obmafs3_free_block(ctx, lba);
        } else {
            /* Remove child pointer from parent */
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if (!pbuf) {
                free(buf);
                return OBMAFS3_ERR_NOMEM;
            }

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if (rc != OBMAFS3_OK) {
                free(pbuf);
                free(buf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct catalog_index_entry);

            if (pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz,
                        pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if (phdr.node_keys == 0 && depth == 1) {
                /* Root index has no children — tree is empty */
                ctx->catalog_hdr.root_node_lba = 0;
                ctx->catalog_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba,
                                                &ctx->catalog_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else if (phdr.node_keys == 1 && depth == 1) {
                /* Root index has one child — collapse tree height */
                struct catalog_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->catalog_hdr.root_node_lba = remaining.child_lba;
                ctx->catalog_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba,
                                                &ctx->catalog_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if (rc == OBMAFS3_OK) {
                    ctx->catalog_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(
                        ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr);
                }
                obmafs3_free_block(ctx, lba);
            }

            free(pbuf);
        }
    } else {
        /* Compact remaining entries */
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if ((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz,
                    data + ((size_t)idx + 1) * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)idx) * rec_sz);

        memset(data + (size_t)leaf_hdr.node_keys * rec_sz, 0, rec_sz);
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
    }

    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Inode insert / update (B+Tree)                                     */
/* ------------------------------------------------------------------ */

int obmafs3_inode_put(struct obmafs3_ctx *ctx,
                      const struct inode_record *inode)
{
    const struct inode_record *rec = inode;

    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba  = ctx->inode_hdr.root_node_lba;
    int rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if (root_lba == 0) {
        uint64_t new_lba;
        rc = obmafs3_alloc_block(ctx, &new_lba);
        if (rc != OBMAFS3_OK)
            return rc;

        uint8_t *buf = calloc(1, bsz);
        if (!buf)
            return OBMAFS3_ERR_NOMEM;

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
        free(buf);
        if (rc != OBMAFS3_OK)
            return rc;

        ctx->inode_hdr.root_node_lba = new_lba;
        ctx->inode_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.inode_lba,
                                          &ctx->inode_hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct inode_btree_path path[INODE_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break; /* reached leaf; buf holds it at lba */

        if (depth >= INODE_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = inode_index_find(buf, hdr.node_keys,
                                         rec->inode_id);
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

    /* Check for update-in-place */
    int idx = inode_leaf_find(buf, leaf_hdr.node_keys, rec->inode_id);
    if (idx >= 0) {
        memcpy(buf + sizeof(struct btree_node_header)
                   + (size_t)idx * sizeof(struct inode_record),
               rec, sizeof(*rec));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* Not found — insert.  insert_pos is where the new record goes. */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = inode_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct inode_record);

    if (leaf_hdr.node_keys < max_leaf) {
        /* Room in leaf */
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if (insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz,
                    data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, rec, rec_sz);
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
    struct inode_record *all = calloc(total, rec_sz);
    if (!all) {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    /* Build sorted array of all records including the new one */
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    all[insert_pos] = *rec;
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
    nh.record_type = kBtreeDataTypeInode;
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
    ctx->inode_hdr.total_nodes++;

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

        uint16_t max_idx    = inode_index_max_keys(ctx);
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
            return obmafs3_btree_header_write(
                ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
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
        nih.record_type = kBtreeDataTypeInode;
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
        ctx->inode_hdr.total_nodes++;
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
    free(buf);
    if (rc != OBMAFS3_OK)
        return rc;

    ctx->inode_hdr.root_node_lba = new_root_lba;
    ctx->inode_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.inode_lba,
                                      &ctx->inode_hdr);
}

int obmafs3_inode_delete(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba  = ctx->inode_hdr.root_node_lba;
    int rc;

    if (root_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct inode_btree_path path[INODE_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    /* Traverse to leaf */
    while (1) {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break;

        if (depth >= INODE_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = inode_index_find(buf, hdr.node_keys, inode_id);
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

    /* buf holds the leaf at lba */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = inode_leaf_find(buf, leaf_hdr.node_keys, inode_id);
    if (idx < 0) {
        free(buf);
        return OBMAFS3_ERR_NOTFOUND;
    }

    /* Save the record for freeing extent blocks later */
    struct inode_record del_rec;
    size_t rec_sz = sizeof(struct inode_record);
    memcpy(&del_rec,
           buf + sizeof(struct btree_node_header) + (size_t)idx * rec_sz,
           rec_sz);

    /* Remove the record from the leaf */
    leaf_hdr.node_keys--;

    if (leaf_hdr.node_keys == 0) {
        /* Leaf is now empty */
        if (depth == 0) {
            /* Root leaf empty — tree is empty */
            ctx->inode_hdr.root_node_lba = 0;
            ctx->inode_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba,
                                            &ctx->inode_hdr);
            obmafs3_free_block(ctx, lba);
        } else {
            /* Remove child pointer from parent */
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if (!pbuf) {
                free(buf);
                return OBMAFS3_ERR_NOMEM;
            }

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if (rc != OBMAFS3_OK) {
                free(pbuf);
                free(buf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct btree_index_entry);

            if (pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz,
                        pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if (phdr.node_keys == 0 && depth == 1) {
                /* Root index has no children — tree is empty */
                ctx->inode_hdr.root_node_lba = 0;
                ctx->inode_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba,
                                                &ctx->inode_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else if (phdr.node_keys == 1 && depth == 1) {
                /* Root index has one child — collapse tree height */
                struct btree_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->inode_hdr.root_node_lba = remaining.child_lba;
                ctx->inode_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba,
                                                &ctx->inode_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if (rc == OBMAFS3_OK) {
                    ctx->inode_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(
                        ctx, ctx->sb.inode_lba, &ctx->inode_hdr);
                }
                obmafs3_free_block(ctx, lba);
            }

            free(pbuf);
        }
    } else {
        /* Compact remaining entries */
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if ((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz,
                    data + ((size_t)idx + 1) * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)idx) * rec_sz);

        memset(data + (size_t)leaf_hdr.node_keys * rec_sz, 0, rec_sz);
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
    }

    free(buf);

    /* Free extent blocks from the deleted inode */
    for (int ei = 0; ei < 8; ei++) {
        if (del_rec.extents[ei].block_count > 0)
            obmafs3_free_blocks(ctx, del_rec.extents[ei].start_block,
                                del_rec.extents[ei].block_count);
    }

    return rc;
}

/* ================================================================== */
/*  Media Tag B+Tree                                                   */
/*                                                                     */
/*  Composite key: (inode_id, tag_type).  Leaf nodes store             */
/*  media_tag_record entries; index nodes store                        */
/*  media_tag_index_entry entries.  Tags <= 512 bytes are stored       */
/*  inline; larger tags use separately allocated data blocks.          */
/* ================================================================== */

/** Maximum media_tag_record entries in a leaf node. */
static uint16_t media_tag_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct media_tag_record));
}

/** Maximum media_tag_index_entry entries in an index node. */
static uint16_t media_tag_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct media_tag_index_entry));
}

/** Compare composite key (inode_id, tag_type). */
static int media_tag_key_cmp(uint64_t id_a, uint16_t type_a,
                             uint64_t id_b, uint16_t type_b)
{
    if (id_a < id_b) return -1;
    if (id_a > id_b) return  1;
    if (type_a < type_b) return -1;
    if (type_a > type_b) return  1;
    return 0;
}

/**
 * Binary search for (inode_id, tag_type) in a media tag leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int media_tag_leaf_find(const uint8_t *buf, uint16_t node_keys,
                               uint64_t inode_id, uint16_t tag_type)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *rec =
            data + (size_t)mid * sizeof(struct media_tag_record);
        uint64_t mid_id;
        uint16_t mid_type;
        memcpy(&mid_id, rec, sizeof(mid_id));
        memcpy(&mid_type, rec + sizeof(mid_id), sizeof(mid_type));

        int cmp = media_tag_key_cmp(mid_id, mid_type, inode_id, tag_type);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
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
static uint16_t media_tag_index_find(const uint8_t *buf,
                                     uint16_t node_keys,
                                     uint64_t inode_id, uint16_t tag_type)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;
    uint16_t result = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *ie =
            data + (size_t)mid * sizeof(struct media_tag_index_entry);
        uint64_t mid_id;
        uint16_t mid_type;
        memcpy(&mid_id, ie, sizeof(mid_id));
        memcpy(&mid_type, ie + sizeof(mid_id), sizeof(mid_type));

        int cmp = media_tag_key_cmp(mid_id, mid_type, inode_id, tag_type);
        if (cmp <= 0) {
            result = (uint16_t)mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return result;
}

#define MEDIA_TAG_BTREE_MAX_DEPTH 8

struct media_tag_btree_path {
    uint64_t lba;
    uint16_t slot;
};

/* ---- Internal lookup ---- */

static int media_tag_tree_lookup(struct obmafs3_ctx *ctx,
                                 uint64_t inode_id, uint16_t tag_type,
                                 struct media_tag_record *record)
{
    uint64_t lba = ctx->media_tag_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    while (1) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level > 0) {
            uint16_t slot = media_tag_index_find(buf, hdr.node_keys,
                                                 inode_id, tag_type);
            struct media_tag_index_entry ie;
            memcpy(&ie,
                   buf + sizeof(struct btree_node_header)
                       + (size_t)slot * sizeof(ie),
                   sizeof(ie));
            lba = ie.child_lba;
        } else {
            int idx = media_tag_leaf_find(buf, hdr.node_keys,
                                          inode_id, tag_type);
            if (idx >= 0) {
                memcpy(record,
                       buf + sizeof(struct btree_node_header)
                           + (size_t)idx * sizeof(*record),
                       sizeof(*record));
                free(buf);
                return OBMAFS3_OK;
            }
            free(buf);
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ---- External data helpers ---- */

/** Free external data blocks referenced by a media tag record. */
static void media_tag_free_external(struct obmafs3_ctx *ctx,
                                    const struct media_tag_record *rec)
{
    if (!(rec->flags & MEDIA_TAG_FLAG_INLINE) &&
        rec->data_lba != 0 && rec->data_blocks != 0) {
        obmafs3_free_blocks(ctx, rec->data_lba, rec->data_blocks);
    }
}

/** Allocate contiguous blocks and write external tag data. */
static int media_tag_write_external(struct obmafs3_ctx *ctx,
                                    const void *data, uint32_t data_length,
                                    uint64_t *out_lba, uint64_t *out_nblocks)
{
    uint64_t bsz    = ctx->sb.block_size;
    uint64_t blocks = ((uint64_t)data_length + bsz - 1) / bsz;
    uint64_t start_lba;

    int rc = obmafs3_alloc_blocks(ctx, blocks, &start_lba);
    if (rc != OBMAFS3_OK)
        return rc;

    uint8_t *block_buf = calloc(1, (size_t)bsz);
    if (!block_buf) {
        obmafs3_free_blocks(ctx, start_lba, blocks);
        return OBMAFS3_ERR_NOMEM;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint32_t remaining = data_length;

    for (uint64_t i = 0; i < blocks; i++) {
        memset(block_buf, 0, (size_t)bsz);
        size_t copy = remaining < bsz ? remaining : (size_t)bsz;
        memcpy(block_buf, src, copy);

        rc = obmafs3_block_write(ctx, start_lba + i,
                                 block_buf, (size_t)bsz);
        if (rc != OBMAFS3_OK) {
            free(block_buf);
            obmafs3_free_blocks(ctx, start_lba, blocks);
            return rc;
        }
        src       += copy;
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
static int media_tag_build_record(struct obmafs3_ctx *ctx,
                                  uint64_t inode_id, uint16_t tag_type,
                                  const void *data, uint32_t data_length,
                                  struct media_tag_record *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->inode_id    = inode_id;
    rec->tag_type    = tag_type;
    rec->data_length = data_length;

    if (data_length <= MEDIA_TAG_INLINE_MAX) {
        rec->flags       = MEDIA_TAG_FLAG_INLINE;
        rec->data_lba    = 0;
        rec->data_blocks = 0;
        if (data_length > 0)
            memcpy(rec->inline_data, data, data_length);
    } else {
        rec->flags = 0;
        uint64_t ext_lba, ext_blocks;
        int rc = media_tag_write_external(ctx, data, data_length,
                                          &ext_lba,
                                          &ext_blocks);
        if (rc != OBMAFS3_OK)
            return rc;
        rec->data_lba    = ext_lba;
        rec->data_blocks = ext_blocks;
    }

    return OBMAFS3_OK;
}

/* ---- Insert or update a media tag record in the B+Tree ---- */

static int media_tag_tree_put(struct obmafs3_ctx *ctx,
                              const struct media_tag_record *rec)
{
    size_t   bsz     = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->media_tag_hdr.root_node_lba;
    int rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if (root_lba == 0) {
        uint64_t new_lba;
        rc = obmafs3_alloc_block(ctx, &new_lba);
        if (rc != OBMAFS3_OK)
            return rc;

        uint8_t *buf = calloc(1, bsz);
        if (!buf)
            return OBMAFS3_ERR_NOMEM;

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
        free(buf);
        if (rc != OBMAFS3_OK)
            return rc;

        ctx->media_tag_hdr.root_node_lba = new_lba;
        ctx->media_tag_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba,
                                          &ctx->media_tag_hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct media_tag_btree_path path[MEDIA_TAG_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break;

        if (depth >= MEDIA_TAG_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = media_tag_index_find(buf, hdr.node_keys,
                                             rec->inode_id,
                                             rec->tag_type);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct media_tag_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for update-in-place */
    int idx = media_tag_leaf_find(buf, leaf_hdr.node_keys,
                                  rec->inode_id, rec->tag_type);
    if (idx >= 0) {
        memcpy(buf + sizeof(struct btree_node_header)
                   + (size_t)idx * sizeof(struct media_tag_record),
               rec, sizeof(*rec));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* Not found — insert.  insert_pos is where the new record goes. */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = media_tag_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct media_tag_record);

    if (leaf_hdr.node_keys < max_leaf) {
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if (insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz,
                    data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, rec, rec_sz);
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
    struct media_tag_record *all = calloc(total, rec_sz);
    if (!all) {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(&all[insert_pos], rec, rec_sz);
    memcpy(&all[insert_pos + 1],
           leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

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

    memset(buf, 0, bsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeMediaTagEntry;
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

        uint16_t max_idx    = media_tag_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct media_tag_index_entry);

        if (phdr.node_keys < max_idx) {
            uint8_t *id = buf + sizeof(struct btree_node_header);

            if (idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz,
                        id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)idx_insert) * ie_sz);

            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            free(buf);
            if (rc != OBMAFS3_OK)
                return rc;
            return obmafs3_btree_header_write(
                ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr);
        }

        /* Parent is full — split the index node */
        uint16_t idx_total = max_idx + 1;
        struct media_tag_index_entry *aie = calloc(idx_total, ie_sz);
        if (!aie) {
            free(buf);
            return OBMAFS3_ERR_NOMEM;
        }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1],
               id + (size_t)idx_insert * ie_sz,
               ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

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
        nih.record_type = kBtreeDataTypeMediaTagEntry;
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

        push_ie.inode_id  = aie[il].inode_id;
        push_ie.tag_type  = aie[il].tag_type;
        push_ie.child_lba = new_idx_lba;

        left_ie.inode_id  = aie[0].inode_id;
        left_ie.tag_type  = aie[0].tag_type;
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;

        free(aie);
        ctx->media_tag_hdr.total_nodes++;
    }

    /* ---- Create new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_alloc_block(ctx, &new_root_lba);
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }

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
    free(buf);
    if (rc != OBMAFS3_OK)
        return rc;

    ctx->media_tag_hdr.root_node_lba = new_root_lba;
    ctx->media_tag_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba,
                                      &ctx->media_tag_hdr);
}

/* ---- Delete a media tag record from the B+Tree ---- */

static int media_tag_tree_delete(struct obmafs3_ctx *ctx,
                                 uint64_t inode_id, uint16_t tag_type)
{
    size_t   bsz     = (size_t)ctx->sb.block_size;
    uint64_t root_lba = ctx->media_tag_hdr.root_node_lba;
    int rc;

    if (root_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct media_tag_btree_path path[MEDIA_TAG_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break;

        if (depth >= MEDIA_TAG_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = media_tag_index_find(buf, hdr.node_keys,
                                             inode_id, tag_type);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct media_tag_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = media_tag_leaf_find(buf, leaf_hdr.node_keys,
                                  inode_id, tag_type);
    if (idx < 0) {
        free(buf);
        return OBMAFS3_ERR_NOTFOUND;
    }

    struct media_tag_record del_rec;
    size_t rec_sz = sizeof(struct media_tag_record);
    memcpy(&del_rec,
           buf + sizeof(struct btree_node_header) + (size_t)idx * rec_sz,
           rec_sz);

    leaf_hdr.node_keys--;

    if (leaf_hdr.node_keys == 0) {
        if (depth == 0) {
            ctx->media_tag_hdr.root_node_lba = 0;
            ctx->media_tag_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba,
                                            &ctx->media_tag_hdr);
            obmafs3_free_block(ctx, lba);
        } else {
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if (!pbuf) {
                free(buf);
                return OBMAFS3_ERR_NOMEM;
            }

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if (rc != OBMAFS3_OK) {
                free(pbuf);
                free(buf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct media_tag_index_entry);

            if (pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz,
                        pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if (phdr.node_keys == 0 && depth == 1) {
                ctx->media_tag_hdr.root_node_lba = 0;
                ctx->media_tag_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba,
                                                &ctx->media_tag_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else if (phdr.node_keys == 1 && depth == 1) {
                struct media_tag_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->media_tag_hdr.root_node_lba = remaining.child_lba;
                ctx->media_tag_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.media_tag_lba,
                                                &ctx->media_tag_hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if (rc == OBMAFS3_OK) {
                    ctx->media_tag_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(
                        ctx, ctx->sb.media_tag_lba,
                        &ctx->media_tag_hdr);
                }
                obmafs3_free_block(ctx, lba);
            }

            free(pbuf);
        }
    } else {
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if ((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz,
                    data + ((size_t)idx + 1) * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)idx) * rec_sz);

        memset(data + (size_t)leaf_hdr.node_keys * rec_sz, 0, rec_sz);
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
    }

    free(buf);
    media_tag_free_external(ctx, &del_rec);

    return rc;
}

/* ================================================================== */
/*  Media Tag public API                                               */
/* ================================================================== */

int obmafs3_media_tag_get(struct obmafs3_ctx *ctx, uint64_t inode_id,
                          uint16_t tag_type,
                          void **data, uint32_t *data_length)
{
    if (ctx->sb.media_tag_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    struct media_tag_record rec;
    int rc = media_tag_tree_lookup(ctx, inode_id, tag_type, &rec);
    if (rc != OBMAFS3_OK)
        return rc;

    uint8_t *buf = malloc(rec.data_length);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    if (rec.flags & MEDIA_TAG_FLAG_INLINE) {
        memcpy(buf, rec.inline_data, rec.data_length);
    } else {
        size_t bsz = (size_t)ctx->sb.block_size;
        uint8_t *block_buf = calloc(1, bsz);
        if (!block_buf) {
            free(buf);
            return OBMAFS3_ERR_NOMEM;
        }

        uint32_t remaining = rec.data_length;
        uint32_t offset    = 0;

        for (uint64_t i = 0; i < rec.data_blocks && remaining > 0; i++) {
            rc = obmafs3_block_read(ctx, rec.data_lba + i,
                                    block_buf, bsz);
            if (rc != OBMAFS3_OK) {
                free(block_buf);
                free(buf);
                return rc;
            }
            size_t copy = remaining < bsz ? remaining : bsz;
            memcpy(buf + offset, block_buf, copy);
            offset    += (uint32_t)copy;
            remaining -= (uint32_t)copy;
        }

        free(block_buf);
    }

    *data        = buf;
    *data_length = rec.data_length;
    return OBMAFS3_OK;
}

void obmafs3_media_tag_data_free(void *data)
{
    free(data);
}

int obmafs3_media_tag_put(struct obmafs3_ctx *ctx, uint64_t inode_id,
                          uint16_t tag_type,
                          const void *data, uint32_t data_length)
{
    if (ctx->sb.media_tag_lba == 0)
        return OBMAFS3_ERR_INVAL;

    /* If the record already exists, free old external data first */
    struct media_tag_record old_rec;
    int rc = media_tag_tree_lookup(ctx, inode_id, tag_type, &old_rec);
    if (rc == OBMAFS3_OK)
        media_tag_free_external(ctx, &old_rec);
    else if (rc != OBMAFS3_ERR_NOTFOUND)
        return rc;

    struct media_tag_record new_rec;
    rc = media_tag_build_record(ctx, inode_id, tag_type,
                                data, data_length, &new_rec);
    if (rc != OBMAFS3_OK)
        return rc;

    return media_tag_tree_put(ctx, &new_rec);
}

int obmafs3_media_tag_delete(struct obmafs3_ctx *ctx, uint64_t inode_id,
                             uint16_t tag_type)
{
    if (ctx->sb.media_tag_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    return media_tag_tree_delete(ctx, inode_id, tag_type);
}

int obmafs3_media_tag_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    if (ctx->sb.media_tag_lba == 0)
        return OBMAFS3_OK;

    /* Repeatedly list + delete until no tags remain. */
    while (1) {
        uint16_t *types;
        uint32_t count;
        int rc = obmafs3_media_tag_list(ctx, inode_id, &types, &count);
        if (rc != OBMAFS3_OK)
            return rc;
        if (count == 0) {
            obmafs3_media_tag_list_free(types);
            return OBMAFS3_OK;
        }

        uint16_t first_type = types[0];
        obmafs3_media_tag_list_free(types);

        rc = media_tag_tree_delete(ctx, inode_id, first_type);
        if (rc != OBMAFS3_OK)
            return rc;
    }
}

int obmafs3_media_tag_list(struct obmafs3_ctx *ctx, uint64_t inode_id,
                           uint16_t **tag_types, uint32_t *count)
{
    *tag_types = NULL;
    *count     = 0;

    if (ctx->sb.media_tag_lba == 0)
        return OBMAFS3_OK;

    uint64_t lba = ctx->media_tag_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_OK;

    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    /* Traverse to the leaf that would contain (inode_id, 0) */
    while (1) {
        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0)
            break;

        uint16_t slot = media_tag_index_find(buf, hdr.node_keys,
                                             inode_id, 0);
        struct media_tag_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain for entries with matching inode_id */
    uint32_t  cap   = 16;
    uint16_t *types = malloc(cap * sizeof(uint16_t));
    if (!types) {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    uint32_t n = 0;

    while (1) {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for (uint16_t i = 0; i < hdr.node_keys; i++) {
            struct media_tag_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            if (rec.inode_id == inode_id) {
                if (n >= cap) {
                    cap *= 2;
                    uint16_t *tmp = realloc(types,
                                            cap * sizeof(uint16_t));
                    if (!tmp) {
                        free(types);
                        free(buf);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    types = tmp;
                }
                types[n++] = rec.tag_type;
            } else if (rec.inode_id > inode_id) {
                goto scan_done;
            }
        }

        if (hdr.right_link == 0)
            break;

        int rc = obmafs3_block_read(ctx, hdr.right_link, buf, bsz);
        if (rc != OBMAFS3_OK) {
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

void obmafs3_media_tag_list_free(uint16_t *tag_types)
{
    free(tag_types);
}

/* ================================================================== */
/*  Generic CD hash B+Tree (prefix / suffix / subchannel)              */
/*                                                                     */
/*  All three trees share identical logic: uint64 hash key, fixed-     */
/*  size inline data, no external blocks.  The generic helpers are     */
/*  parameterised by record_size (sizeof the leaf record).  The first  */
/*  8 bytes of every leaf record is always the uint64 hash.            */
/*  Index nodes use struct btree_index_entry (hash + child_lba).       */
/* ================================================================== */

/** Maximum leaf records for a given record size. */
static uint16_t cd_hash_leaf_max(const struct obmafs3_ctx *ctx,
                                 size_t rec_sz)
{
    return (uint16_t)((ctx->sb.block_size
                       - sizeof(struct btree_node_header)) / rec_sz);
}

/** Maximum index entries per node (same for all three trees). */
static uint16_t cd_hash_index_max(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size
                       - sizeof(struct btree_node_header))
                      / sizeof(struct btree_index_entry));
}

/**
 * Binary search for hash in a CD hash leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int cd_hash_leaf_find(const uint8_t *buf, uint16_t node_keys,
                             uint64_t hash, size_t rec_sz)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t mid_hash;
        memcpy(&mid_hash, data + (size_t)mid * rec_sz, sizeof(mid_hash));

        if (mid_hash == hash)
            return mid;
        if (mid_hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return -(lo + 1);
}

/**
 * Binary search in an index node for the child covering hash.
 */
static uint16_t cd_hash_index_find(const uint8_t *buf,
                                   uint16_t node_keys, uint64_t hash)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;
    uint16_t result = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        struct btree_index_entry ie;
        memcpy(&ie,
               data + (size_t)mid * sizeof(ie),
               sizeof(ie));

        if (ie.key <= hash) {
            result = (uint16_t)mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return result;
}

#define CD_HASH_BTREE_MAX_DEPTH 8

struct cd_hash_btree_path {
    uint64_t lba;
    uint16_t slot;
};

/* ---- Lookup ---- */

static int cd_hash_tree_lookup(struct obmafs3_ctx *ctx,
                               const struct btree_header *hdr,
                               uint64_t hash, void *record,
                               size_t rec_sz)
{
    uint64_t lba = hdr->root_node_lba;
    if (lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    while (1) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
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

        if (nhdr.level > 0) {
            uint16_t slot = cd_hash_index_find(buf, nhdr.node_keys,
                                               hash);
            struct btree_index_entry ie;
            memcpy(&ie,
                   buf + sizeof(struct btree_node_header)
                       + (size_t)slot * sizeof(ie),
                   sizeof(ie));
            lba = ie.child_lba;
        } else {
            int idx = cd_hash_leaf_find(buf, nhdr.node_keys,
                                        hash, rec_sz);
            if (idx >= 0) {
                memcpy(record,
                       buf + sizeof(struct btree_node_header)
                           + (size_t)idx * rec_sz,
                       rec_sz);
                free(buf);
                return OBMAFS3_OK;
            }
            free(buf);
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ---- Insert / update ---- */

static int cd_hash_tree_put(struct obmafs3_ctx *ctx,
                            struct btree_header *hdr,
                            uint64_t hdr_lba,
                            const void *record, size_t rec_sz,
                            uint8_t data_type)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba  = hdr->root_node_lba;
    int rc;

    uint64_t hash;
    memcpy(&hash, record, sizeof(hash));

    /* ---- Empty tree: create a single leaf as root ---- */
    if (root_lba == 0) {
        uint64_t new_lba;
        rc = obmafs3_alloc_block(ctx, &new_lba);
        if (rc != OBMAFS3_OK)
            return rc;

        uint8_t *buf = calloc(1, bsz);
        if (!buf)
            return OBMAFS3_ERR_NOMEM;

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
        if (rc != OBMAFS3_OK)
            return rc;

        hdr->root_node_lba = new_lba;
        hdr->total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
    }

    /* ---- Traverse to leaf, recording path ---- */
    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct cd_hash_btree_path path[CD_HASH_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

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
            break;

        if (depth >= CD_HASH_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = cd_hash_index_find(buf, nhdr.node_keys, hash);
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

    /* Check for update-in-place */
    int idx = cd_hash_leaf_find(buf, leaf_hdr.node_keys, hash, rec_sz);
    if (idx >= 0) {
        memcpy(buf + sizeof(struct btree_node_header)
                   + (size_t)idx * rec_sz,
               record, rec_sz);
        compute_node_checksum(buf);
        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* Not found — insert */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = cd_hash_leaf_max(ctx, rec_sz);

    if (leaf_hdr.node_keys < max_leaf) {
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if (insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz,
                    data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, record, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length =
            (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        free(buf);
        return rc;
    }

    /* ---- Leaf full: split ---- */
    uint16_t total = max_leaf + 1;
    uint8_t *all = calloc(total, rec_sz);
    if (!all) {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);

    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(all + (size_t)insert_pos * rec_sz, record, rec_sz);
    memcpy(all + ((size_t)insert_pos + 1) * rec_sz,
           leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

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
    memcpy(buf + sizeof(nh), all + (size_t)left_count * rec_sz,
           (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = obmafs3_block_write(ctx, new_leaf_lba, buf, bsz);
    if (rc != OBMAFS3_OK) {
        free(all);
        free(buf);
        return rc;
    }

    /* Separator to push upward */
    uint64_t right_first_hash;
    memcpy(&right_first_hash,
           all + (size_t)left_count * rec_sz, sizeof(right_first_hash));

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

        uint16_t max_idx    = cd_hash_index_max(ctx);
        uint16_t idx_insert = parent_slot + 1;

        if (phdr.node_keys < max_idx) {
            uint8_t *id = buf + sizeof(struct btree_node_header);

            if (idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz,
                        id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)idx_insert) * ie_sz);

            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);

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

        /* Parent full — split index node */
        uint16_t idx_total = max_idx + 1;
        struct btree_index_entry *aie = calloc(idx_total, ie_sz);
        if (!aie) {
            free(buf);
            return OBMAFS3_ERR_NOMEM;
        }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1],
               id + (size_t)idx_insert * ie_sz,
               ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

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
        nih.record_type = data_type;
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
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }

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
    if (rc != OBMAFS3_OK)
        return rc;

    hdr->root_node_lba = new_root_lba;
    hdr->total_nodes++;
    return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
}

/* ---- Delete ---- */

static int cd_hash_tree_delete(struct obmafs3_ctx *ctx,
                               struct btree_header *hdr,
                               uint64_t hdr_lba,
                               uint64_t hash, size_t rec_sz)
{
    size_t   bsz      = (size_t)ctx->sb.block_size;
    uint64_t root_lba  = hdr->root_node_lba;
    int rc;

    if (root_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, bsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct cd_hash_btree_path path[CD_HASH_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

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
            break;

        if (depth >= CD_HASH_BTREE_MAX_DEPTH) {
            free(buf);
            return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = cd_hash_index_find(buf, nhdr.node_keys, hash);
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

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = cd_hash_leaf_find(buf, leaf_hdr.node_keys, hash, rec_sz);
    if (idx < 0) {
        free(buf);
        return OBMAFS3_ERR_NOTFOUND;
    }

    leaf_hdr.node_keys--;

    if (leaf_hdr.node_keys == 0) {
        if (depth == 0) {
            hdr->root_node_lba = 0;
            hdr->total_nodes--;
            rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
            obmafs3_free_block(ctx, lba);
        } else {
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, bsz);
            if (!pbuf) {
                free(buf);
                return OBMAFS3_ERR_NOMEM;
            }

            rc = obmafs3_block_read(ctx, plba, pbuf, bsz);
            if (rc != OBMAFS3_OK) {
                free(pbuf);
                free(buf);
                return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t   ie_sz = sizeof(struct btree_index_entry);

            if (pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz,
                        pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys
                         - (size_t)pslot - 1) * ie_sz);

            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if (phdr.node_keys == 0 && depth == 1) {
                hdr->root_node_lba = 0;
                hdr->total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else if (phdr.node_keys == 1 && depth == 1) {
                struct btree_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                hdr->root_node_lba = remaining.child_lba;
                hdr->total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
                obmafs3_free_block(ctx, lba);
                obmafs3_free_block(ctx, plba);
            } else {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = obmafs3_block_write(ctx, plba, pbuf, bsz);
                if (rc == OBMAFS3_OK) {
                    hdr->total_nodes--;
                    rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
                }
                obmafs3_free_block(ctx, lba);
            }

            free(pbuf);
        }
    } else {
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if ((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz,
                    data + ((size_t)idx + 1) * rec_sz,
                    ((size_t)leaf_hdr.node_keys
                     - (size_t)idx) * rec_sz);

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

int obmafs3_cd_prefix_get(struct obmafs3_ctx *ctx, uint64_t hash,
                          uint8_t data[CD_PREFIX_DATA_SIZE])
{
    if (ctx->sb.cd_prefix_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    struct cd_prefix_record rec;
    int rc = cd_hash_tree_lookup(ctx, &ctx->cd_prefix_hdr,
                                 hash, &rec,
                                 sizeof(struct cd_prefix_record));
    if (rc != OBMAFS3_OK)
        return rc;

    memcpy(data, rec.data, CD_PREFIX_DATA_SIZE);
    return OBMAFS3_OK;
}

int obmafs3_cd_prefix_put(struct obmafs3_ctx *ctx, uint64_t hash,
                          const uint8_t data[CD_PREFIX_DATA_SIZE])
{
    if (ctx->sb.cd_prefix_lba == 0)
        return OBMAFS3_ERR_INVAL;

    struct cd_prefix_record rec;
    rec.hash = hash;
    memcpy(rec.data, data, CD_PREFIX_DATA_SIZE);

    return cd_hash_tree_put(ctx, &ctx->cd_prefix_hdr,
                            ctx->sb.cd_prefix_lba,
                            &rec, sizeof(struct cd_prefix_record),
                            kBtreeDataTypeCdPrefixEntry);
}

int obmafs3_cd_prefix_delete(struct obmafs3_ctx *ctx, uint64_t hash)
{
    if (ctx->sb.cd_prefix_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    return cd_hash_tree_delete(ctx, &ctx->cd_prefix_hdr,
                               ctx->sb.cd_prefix_lba,
                               hash,
                               sizeof(struct cd_prefix_record));
}

/* ---- CD Suffix ---- */

int obmafs3_cd_suffix_get(struct obmafs3_ctx *ctx, uint64_t hash,
                          uint8_t data[CD_SUFFIX_DATA_SIZE])
{
    if (ctx->sb.cd_suffix_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    struct cd_suffix_record rec;
    int rc = cd_hash_tree_lookup(ctx, &ctx->cd_suffix_hdr,
                                 hash, &rec,
                                 sizeof(struct cd_suffix_record));
    if (rc != OBMAFS3_OK)
        return rc;

    memcpy(data, rec.data, CD_SUFFIX_DATA_SIZE);
    return OBMAFS3_OK;
}

int obmafs3_cd_suffix_put(struct obmafs3_ctx *ctx, uint64_t hash,
                          const uint8_t data[CD_SUFFIX_DATA_SIZE])
{
    if (ctx->sb.cd_suffix_lba == 0)
        return OBMAFS3_ERR_INVAL;

    struct cd_suffix_record rec;
    rec.hash = hash;
    memcpy(rec.data, data, CD_SUFFIX_DATA_SIZE);

    return cd_hash_tree_put(ctx, &ctx->cd_suffix_hdr,
                            ctx->sb.cd_suffix_lba,
                            &rec, sizeof(struct cd_suffix_record),
                            kBtreeDataTypeCdSuffixEntry);
}

int obmafs3_cd_suffix_delete(struct obmafs3_ctx *ctx, uint64_t hash)
{
    if (ctx->sb.cd_suffix_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    return cd_hash_tree_delete(ctx, &ctx->cd_suffix_hdr,
                               ctx->sb.cd_suffix_lba,
                               hash,
                               sizeof(struct cd_suffix_record));
}

/* ---- CD Subchannel ---- */

int obmafs3_cd_subchannel_get(struct obmafs3_ctx *ctx, uint64_t hash,
                              uint8_t data[CD_SUBCHANNEL_DATA_SIZE])
{
    if (ctx->sb.cd_subchannel_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    struct cd_subchannel_record rec;
    int rc = cd_hash_tree_lookup(ctx, &ctx->cd_subchannel_hdr,
                                 hash, &rec,
                                 sizeof(struct cd_subchannel_record));
    if (rc != OBMAFS3_OK)
        return rc;

    memcpy(data, rec.data, CD_SUBCHANNEL_DATA_SIZE);
    return OBMAFS3_OK;
}

int obmafs3_cd_subchannel_put(struct obmafs3_ctx *ctx, uint64_t hash,
                              const uint8_t data[CD_SUBCHANNEL_DATA_SIZE])
{
    if (ctx->sb.cd_subchannel_lba == 0)
        return OBMAFS3_ERR_INVAL;

    struct cd_subchannel_record rec;
    rec.hash = hash;
    memcpy(rec.data, data, CD_SUBCHANNEL_DATA_SIZE);

    return cd_hash_tree_put(ctx, &ctx->cd_subchannel_hdr,
                            ctx->sb.cd_subchannel_lba,
                            &rec, sizeof(struct cd_subchannel_record),
                            kBtreeDataTypeCdSubchannelEntry);
}

int obmafs3_cd_subchannel_delete(struct obmafs3_ctx *ctx, uint64_t hash)
{
    if (ctx->sb.cd_subchannel_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    return cd_hash_tree_delete(ctx, &ctx->cd_subchannel_hdr,
                               ctx->sb.cd_subchannel_lba,
                               hash,
                               sizeof(struct cd_subchannel_record));
}

/* ================================================================== */
/*  Image Metadata B+Trees                                             */
/*                                                                     */
/*  Two trees are maintained in tandem:                                */
/*                                                                     */
/*  1. Per-image metadata tree (metadata_hdr / metadata_lba)           */
/*     Composite key: (inode_id, key).                                 */
/*     Leaf: metadata_record.  Index: metadata_index_entry.            */
/*                                                                     */
/*  2. Reverse-index tree (metadata_idx_hdr / metadata_idx_lba)        */
/*     Composite key: (key, value, inode_id).                          */
/*     Leaf: metadata_idx_record.  Index: metadata_idx_index_entry.    */
/*                                                                     */
/*  Both trees use multi-block nodes (METADATA_NODE_BLOCKS blocks      */
/*  each = 32768 bytes with default 4096-byte block size).             */
/* ================================================================== */

#define METADATA_BTREE_MAX_DEPTH 16

/** Compute node buffer size for metadata trees. */
static size_t meta_node_size(const struct obmafs3_ctx *ctx)
{
    return (size_t)METADATA_NODE_BLOCKS * (size_t)ctx->sb.block_size;
}

/* ---- Per-image metadata tree helpers ---- */

static uint16_t meta_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) -
                       sizeof(struct btree_node_header))
                      / sizeof(struct metadata_record));
}

static uint16_t meta_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) -
                       sizeof(struct btree_node_header))
                      / sizeof(struct metadata_index_entry));
}

/** Compare composite key (inode_id, key). */
static int meta_key_cmp(uint64_t id_a, const char *key_a,
                        uint64_t id_b, const char *key_b)
{
    if (id_a < id_b) return -1;
    if (id_a > id_b) return  1;
    return strncmp(key_a, key_b, METADATA_KEY_MAX);
}

/** Binary search in a metadata leaf node. Returns index or -(ins)-1. */
static int meta_leaf_find(const uint8_t *buf, uint16_t node_keys,
                          uint64_t inode_id, const char *key)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const struct metadata_record *rec =
            (const struct metadata_record *)
            (data + (size_t)mid * sizeof(struct metadata_record));

        int cmp = meta_key_cmp(rec->inode_id, rec->key,
                               inode_id, key);
        if (cmp == 0) return mid;
        if (cmp < 0)  lo = mid + 1;
        else          hi = mid - 1;
    }
    return -(lo + 1);
}

/** Binary search in a metadata index node. Returns slot to descend. */
static uint16_t meta_index_find(const uint8_t *buf, uint16_t node_keys,
                                uint64_t inode_id, const char *key)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;
    uint16_t result = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const struct metadata_index_entry *ie =
            (const struct metadata_index_entry *)
            (data + (size_t)mid * sizeof(struct metadata_index_entry));

        int cmp = meta_key_cmp(ie->inode_id, ie->key,
                               inode_id, key);
        if (cmp <= 0) {
            result = (uint16_t)mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

struct meta_btree_path {
    uint64_t lba;
    uint16_t slot;
};

/** Allocate a multi-block node. */
static int meta_alloc_node(struct obmafs3_ctx *ctx, uint64_t *lba)
{
    return obmafs3_alloc_blocks(ctx, METADATA_NODE_BLOCKS, lba);
}

/** Free a multi-block node. */
static void meta_free_node(struct obmafs3_ctx *ctx, uint64_t lba)
{
    obmafs3_free_blocks(ctx, lba, METADATA_NODE_BLOCKS);
}

/** Read a multi-block node. */
static int meta_node_read(struct obmafs3_ctx *ctx, uint64_t lba,
                          uint8_t *buf)
{
    return obmafs3_block_read(ctx, lba, buf, meta_node_size(ctx));
}

/** Write a multi-block node. */
static int meta_node_write(struct obmafs3_ctx *ctx, uint64_t lba,
                           const uint8_t *buf)
{
    return obmafs3_block_write(ctx, lba, buf, meta_node_size(ctx));
}

/* ---- Per-image metadata tree: lookup ---- */

static int meta_tree_lookup(struct obmafs3_ctx *ctx,
                            uint64_t inode_id, const char *key,
                            struct metadata_record *record)
{
    uint64_t lba = ctx->metadata_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    size_t nsz = meta_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    while (1) {
        int rc = meta_node_read(ctx, lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level > 0) {
            uint16_t slot = meta_index_find(buf, hdr.node_keys,
                                            inode_id, key);
            struct metadata_index_entry ie;
            memcpy(&ie,
                   buf + sizeof(struct btree_node_header)
                       + (size_t)slot * sizeof(ie),
                   sizeof(ie));
            lba = ie.child_lba;
        } else {
            int idx = meta_leaf_find(buf, hdr.node_keys,
                                     inode_id, key);
            if (idx >= 0) {
                memcpy(record,
                       buf + sizeof(struct btree_node_header)
                           + (size_t)idx * sizeof(*record),
                       sizeof(*record));
                free(buf);
                return OBMAFS3_OK;
            }
            free(buf);
            return OBMAFS3_ERR_NOTFOUND;
        }
    }
}

/* ---- Per-image metadata tree: insert / update ---- */

static int meta_tree_put(struct obmafs3_ctx *ctx,
                         const struct metadata_record *rec)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_hdr.root_node_lba;
    int rc;

    /* Empty tree: create a single leaf as root */
    if (root_lba == 0) {
        uint64_t new_lba;
        rc = meta_alloc_node(ctx, &new_lba);
        if (rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, nsz);
        if (!buf) return OBMAFS3_ERR_NOMEM;

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
        if (rc != OBMAFS3_OK) return rc;

        ctx->metadata_hdr.root_node_lba = new_lba;
        ctx->metadata_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba,
                                          &ctx->metadata_hdr);
    }

    /* Traverse from root to leaf, recording path */
    uint8_t *buf = calloc(1, nsz);
    if (!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = meta_node_read(ctx, lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0) break;

        if (depth >= METADATA_BTREE_MAX_DEPTH) {
            free(buf); return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = meta_index_find(buf, hdr.node_keys,
                                        rec->inode_id, rec->key);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* buf holds the leaf node at lba */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for update-in-place */
    int idx = meta_leaf_find(buf, leaf_hdr.node_keys,
                             rec->inode_id, rec->key);
    if (idx >= 0) {
        memcpy(buf + sizeof(struct btree_node_header)
                   + (size_t)idx * sizeof(struct metadata_record),
               rec, sizeof(*rec));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
        free(buf);
        return rc;
    }

    /* Insert */
    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = meta_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct metadata_record);

    if (leaf_hdr.node_keys < max_leaf) {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if (insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz,
                    data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys -
                     (size_t)insert_pos) * rec_sz);
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
    uint16_t total = max_leaf + 1;
    struct metadata_record *all = calloc(total, rec_sz);
    if (!all) { free(buf); return OBMAFS3_ERR_NOMEM; }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(&all[insert_pos], rec, rec_sz);
    memcpy(&all[insert_pos + 1],
           leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    memset(leaf_data, 0, nsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;
    uint64_t new_leaf_lba;
    rc = meta_alloc_node(ctx, &new_leaf_lba);
    if (rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    leaf_hdr.node_keys   = left_count;
    leaf_hdr.keys_length = (uint16_t)(left_count * rec_sz);
    leaf_hdr.right_link  = new_leaf_lba;
    memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, lba, buf);
    if (rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    memset(buf, 0, nsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeMetadataEntry;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), &all[left_count],
           (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, new_leaf_lba, buf);
    if (rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    struct metadata_index_entry push_ie;
    push_ie.inode_id  = all[left_count].inode_id;
    strncpy(push_ie.key, all[left_count].key, METADATA_KEY_MAX);
    push_ie.child_lba = new_leaf_lba;

    struct metadata_index_entry left_ie;
    left_ie.inode_id  = all[0].inode_id;
    strncpy(left_ie.key, all[0].key, METADATA_KEY_MAX);
    left_ie.child_lba = lba;

    uint64_t left_lba = lba;
    free(all);
    ctx->metadata_hdr.total_nodes++;

    /* Propagate split upward */
    while (depth > 0) {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = meta_node_read(ctx, parent_lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = meta_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct metadata_index_entry);

        if (phdr.node_keys < max_idx) {
            uint8_t *id = buf + sizeof(struct btree_node_header);
            if (idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz,
                        id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys -
                         (size_t)idx_insert) * ie_sz);
            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);
            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);
            rc = meta_node_write(ctx, parent_lba, buf);
            free(buf);
            if (rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba,
                                              &ctx->metadata_hdr);
        }

        /* Parent full — split index node */
        uint16_t idx_total = max_idx + 1;
        struct metadata_index_entry *aie = calloc(idx_total, ie_sz);
        if (!aie) { free(buf); return OBMAFS3_ERR_NOMEM; }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1],
               id + (size_t)idx_insert * ie_sz,
               ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        memset(id, 0, nsz - sizeof(struct btree_node_header));
        memcpy(id, aie, (size_t)il * ie_sz);
        phdr.node_keys   = il;
        phdr.keys_length = (uint16_t)(il * ie_sz);
        memcpy(buf, &phdr, sizeof(phdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, parent_lba, buf);
        if (rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        uint64_t new_idx_lba;
        rc = meta_alloc_node(ctx, &new_idx_lba);
        if (rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        memset(buf, 0, nsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeMetadataEntry;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, new_idx_lba, buf);
        if (rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        push_ie.inode_id = aie[il].inode_id;
        strncpy(push_ie.key, aie[il].key, METADATA_KEY_MAX);
        push_ie.child_lba = new_idx_lba;

        left_ie.inode_id = aie[0].inode_id;
        strncpy(left_ie.key, aie[0].key, METADATA_KEY_MAX);
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;
        free(aie);
        ctx->metadata_hdr.total_nodes++;
    }

    /* Create new root */
    uint64_t new_root_lba;
    rc = meta_alloc_node(ctx, &new_root_lba);
    if (rc != OBMAFS3_OK) { free(buf); return rc; }

    rc = meta_node_read(ctx, left_lba, buf);
    if (rc != OBMAFS3_OK) { free(buf); return rc; }
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
    if (rc != OBMAFS3_OK) return rc;

    ctx->metadata_hdr.root_node_lba = new_root_lba;
    ctx->metadata_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba,
                                      &ctx->metadata_hdr);
}

/* ---- Per-image metadata tree: delete ---- */

static int meta_tree_delete(struct obmafs3_ctx *ctx,
                            uint64_t inode_id, const char *key)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_hdr.root_node_lba;
    int rc;

    if (root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, nsz);
    if (!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = meta_node_read(ctx, lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0) break;

        if (depth >= METADATA_BTREE_MAX_DEPTH) {
            free(buf); return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = meta_index_find(buf, hdr.node_keys,
                                        inode_id, key);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = meta_leaf_find(buf, leaf_hdr.node_keys,
                             inode_id, key);
    if (idx < 0) { free(buf); return OBMAFS3_ERR_NOTFOUND; }

    size_t rec_sz = sizeof(struct metadata_record);
    leaf_hdr.node_keys--;

    if (leaf_hdr.node_keys == 0) {
        if (depth == 0) {
            ctx->metadata_hdr.root_node_lba = 0;
            ctx->metadata_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba,
                                            &ctx->metadata_hdr);
            meta_free_node(ctx, lba);
        } else {
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, nsz);
            if (!pbuf) { free(buf); return OBMAFS3_ERR_NOMEM; }

            rc = meta_node_read(ctx, plba, pbuf);
            if (rc != OBMAFS3_OK) {
                free(pbuf); free(buf); return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t ie_sz = sizeof(struct metadata_index_entry);

            if (pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz,
                        pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)pslot - 1) * ie_sz);
            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if (phdr.node_keys == 0 && depth == 1) {
                ctx->metadata_hdr.root_node_lba = 0;
                ctx->metadata_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba,
                                                &ctx->metadata_hdr);
                meta_free_node(ctx, lba);
                meta_free_node(ctx, plba);
            } else if (phdr.node_keys == 1 && depth == 1) {
                struct metadata_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->metadata_hdr.root_node_lba = remaining.child_lba;
                ctx->metadata_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_lba,
                                                &ctx->metadata_hdr);
                meta_free_node(ctx, lba);
                meta_free_node(ctx, plba);
            } else {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = meta_node_write(ctx, plba, pbuf);
                if (rc == OBMAFS3_OK) {
                    ctx->metadata_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(
                        ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr);
                }
                meta_free_node(ctx, lba);
            }
            free(pbuf);
        }
    } else {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if ((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz,
                    data + ((size_t)idx + 1) * rec_sz,
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

static uint16_t midx_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) -
                       sizeof(struct btree_node_header))
                      / sizeof(struct metadata_idx_record));
}

static uint16_t midx_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((meta_node_size(ctx) -
                       sizeof(struct btree_node_header))
                      / sizeof(struct metadata_idx_index_entry));
}

/** Compare composite key (key, value, inode_id). */
static int midx_key_cmp(const char *key_a, const char *val_a, uint64_t id_a,
                         const char *key_b, const char *val_b, uint64_t id_b)
{
    int c = strncmp(key_a, key_b, METADATA_KEY_MAX);
    if (c != 0) return c;
    c = strncmp(val_a, val_b, METADATA_VALUE_MAX);
    if (c != 0) return c;
    if (id_a < id_b) return -1;
    if (id_a > id_b) return  1;
    return 0;
}

static int midx_leaf_find(const uint8_t *buf, uint16_t node_keys,
                           const char *key, const char *value,
                           uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const struct metadata_idx_record *rec =
            (const struct metadata_idx_record *)
            (data + (size_t)mid * sizeof(struct metadata_idx_record));

        int cmp = midx_key_cmp(rec->key, rec->value, rec->inode_id,
                                key, value, inode_id);
        if (cmp == 0) return mid;
        if (cmp < 0)  lo = mid + 1;
        else          hi = mid - 1;
    }
    return -(lo + 1);
}

static uint16_t midx_index_find(const uint8_t *buf, uint16_t node_keys,
                                 const char *key, const char *value,
                                 uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)node_keys - 1;
    uint16_t result = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const struct metadata_idx_index_entry *ie =
            (const struct metadata_idx_index_entry *)
            (data + (size_t)mid *
             sizeof(struct metadata_idx_index_entry));

        int cmp = midx_key_cmp(ie->key, ie->value, ie->inode_id,
                                key, value, inode_id);
        if (cmp <= 0) {
            result = (uint16_t)mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

/* ---- Reverse-index tree: insert ---- */

static int midx_tree_put(struct obmafs3_ctx *ctx,
                          const struct metadata_idx_record *rec)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_idx_hdr.root_node_lba;
    int rc;

    if (root_lba == 0) {
        uint64_t new_lba;
        rc = meta_alloc_node(ctx, &new_lba);
        if (rc != OBMAFS3_OK) return rc;

        uint8_t *buf = calloc(1, nsz);
        if (!buf) return OBMAFS3_ERR_NOMEM;

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
        if (rc != OBMAFS3_OK) return rc;

        ctx->metadata_idx_hdr.root_node_lba = new_lba;
        ctx->metadata_idx_hdr.total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba,
                                          &ctx->metadata_idx_hdr);
    }

    uint8_t *buf = calloc(1, nsz);
    if (!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = meta_node_read(ctx, lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0) break;

        if (depth >= METADATA_BTREE_MAX_DEPTH) {
            free(buf); return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = midx_index_find(buf, hdr.node_keys,
                                         rec->key, rec->value,
                                         rec->inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_idx_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    /* Check for duplicate (should not happen if caller is correct) */
    int idx = midx_leaf_find(buf, leaf_hdr.node_keys,
                              rec->key, rec->value, rec->inode_id);
    if (idx >= 0) {
        /* Already exists — update in place */
        memcpy(buf + sizeof(struct btree_node_header)
                   + (size_t)idx * sizeof(struct metadata_idx_record),
               rec, sizeof(*rec));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, lba, buf);
        free(buf);
        return rc;
    }

    int      insert_pos = -(idx + 1);
    uint16_t max_leaf   = midx_leaf_max_keys(ctx);
    size_t   rec_sz     = sizeof(struct metadata_idx_record);

    if (leaf_hdr.node_keys < max_leaf) {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if (insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz,
                    data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys -
                     (size_t)insert_pos) * rec_sz);
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
    uint16_t total = max_leaf + 1;
    struct metadata_idx_record *all = calloc(total, rec_sz);
    if (!all) { free(buf); return OBMAFS3_ERR_NOMEM; }

    uint8_t *leaf_data = buf + sizeof(struct btree_node_header);
    memcpy(all, leaf_data, (size_t)insert_pos * rec_sz);
    memcpy(&all[insert_pos], rec, rec_sz);
    memcpy(&all[insert_pos + 1],
           leaf_data + (size_t)insert_pos * rec_sz,
           ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    memset(leaf_data, 0, nsz - sizeof(struct btree_node_header));
    memcpy(leaf_data, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;
    uint64_t new_leaf_lba;
    rc = meta_alloc_node(ctx, &new_leaf_lba);
    if (rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    leaf_hdr.node_keys   = left_count;
    leaf_hdr.keys_length = (uint16_t)(left_count * rec_sz);
    leaf_hdr.right_link  = new_leaf_lba;
    memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, lba, buf);
    if (rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    memset(buf, 0, nsz);
    struct btree_node_header nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    nh.record_type = kBtreeDataTypeMetadataIndexEntry;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), &all[left_count],
           (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = meta_node_write(ctx, new_leaf_lba, buf);
    if (rc != OBMAFS3_OK) { free(all); free(buf); return rc; }

    struct metadata_idx_index_entry push_ie;
    strncpy(push_ie.key,   all[left_count].key,   METADATA_KEY_MAX);
    strncpy(push_ie.value, all[left_count].value,  METADATA_VALUE_MAX);
    push_ie.inode_id  = all[left_count].inode_id;
    push_ie.child_lba = new_leaf_lba;

    struct metadata_idx_index_entry left_ie;
    strncpy(left_ie.key,   all[0].key,   METADATA_KEY_MAX);
    strncpy(left_ie.value, all[0].value,  METADATA_VALUE_MAX);
    left_ie.inode_id  = all[0].inode_id;
    left_ie.child_lba = lba;

    uint64_t left_lba = lba;
    free(all);
    ctx->metadata_idx_hdr.total_nodes++;

    /* Propagate split upward */
    while (depth > 0) {
        depth--;
        uint64_t parent_lba  = path[depth].lba;
        uint16_t parent_slot = path[depth].slot;

        rc = meta_node_read(ctx, parent_lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = midx_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct metadata_idx_index_entry);

        if (phdr.node_keys < max_idx) {
            uint8_t *id = buf + sizeof(struct btree_node_header);
            if (idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz,
                        id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys -
                         (size_t)idx_insert) * ie_sz);
            memcpy(id + (size_t)idx_insert * ie_sz, &push_ie, ie_sz);
            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);
            rc = meta_node_write(ctx, parent_lba, buf);
            free(buf);
            if (rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(
                ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr);
        }

        /* Parent full — split index node */
        uint16_t idx_total = max_idx + 1;
        struct metadata_idx_index_entry *aie = calloc(idx_total, ie_sz);
        if (!aie) { free(buf); return OBMAFS3_ERR_NOMEM; }

        uint8_t *id = buf + sizeof(struct btree_node_header);
        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        memcpy(&aie[idx_insert], &push_ie, ie_sz);
        memcpy(&aie[idx_insert + 1],
               id + (size_t)idx_insert * ie_sz,
               ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        memset(id, 0, nsz - sizeof(struct btree_node_header));
        memcpy(id, aie, (size_t)il * ie_sz);
        phdr.node_keys   = il;
        phdr.keys_length = (uint16_t)(il * ie_sz);
        memcpy(buf, &phdr, sizeof(phdr));
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, parent_lba, buf);
        if (rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        uint64_t new_idx_lba;
        rc = meta_alloc_node(ctx, &new_idx_lba);
        if (rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        memset(buf, 0, nsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeMetadataIndexEntry;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = meta_node_write(ctx, new_idx_lba, buf);
        if (rc != OBMAFS3_OK) { free(aie); free(buf); return rc; }

        strncpy(push_ie.key,   aie[il].key,   METADATA_KEY_MAX);
        strncpy(push_ie.value, aie[il].value,  METADATA_VALUE_MAX);
        push_ie.inode_id  = aie[il].inode_id;
        push_ie.child_lba = new_idx_lba;

        strncpy(left_ie.key,   aie[0].key,   METADATA_KEY_MAX);
        strncpy(left_ie.value, aie[0].value,  METADATA_VALUE_MAX);
        left_ie.inode_id  = aie[0].inode_id;
        left_ie.child_lba = parent_lba;

        left_lba = parent_lba;
        free(aie);
        ctx->metadata_idx_hdr.total_nodes++;
    }

    /* Create new root */
    uint64_t new_root_lba;
    rc = meta_alloc_node(ctx, &new_root_lba);
    if (rc != OBMAFS3_OK) { free(buf); return rc; }

    rc = meta_node_read(ctx, left_lba, buf);
    if (rc != OBMAFS3_OK) { free(buf); return rc; }
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, nsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeMetadataIndexEntry;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length =
        (uint16_t)(2 * sizeof(struct metadata_idx_index_entry));
    memcpy(buf, &rh, sizeof(rh));

    left_ie.child_lba = left_lba;
    struct metadata_idx_index_entry roots[2];
    roots[0] = left_ie;
    roots[1] = push_ie;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = meta_node_write(ctx, new_root_lba, buf);
    free(buf);
    if (rc != OBMAFS3_OK) return rc;

    ctx->metadata_idx_hdr.root_node_lba = new_root_lba;
    ctx->metadata_idx_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba,
                                      &ctx->metadata_idx_hdr);
}

/* ---- Reverse-index tree: delete ---- */

static int midx_tree_delete(struct obmafs3_ctx *ctx,
                             const char *key, const char *value,
                             uint64_t inode_id)
{
    size_t   nsz      = meta_node_size(ctx);
    uint64_t root_lba = ctx->metadata_idx_hdr.root_node_lba;
    int rc;

    if (root_lba == 0) return OBMAFS3_ERR_NOTFOUND;

    uint8_t *buf = calloc(1, nsz);
    if (!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_btree_path path[METADATA_BTREE_MAX_DEPTH];
    int depth = 0;
    uint64_t lba = root_lba;

    while (1) {
        rc = meta_node_read(ctx, lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0) break;

        if (depth >= METADATA_BTREE_MAX_DEPTH) {
            free(buf); return OBMAFS3_ERR_INVAL;
        }

        uint16_t slot = midx_index_find(buf, hdr.node_keys,
                                         key, value, inode_id);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct metadata_idx_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int idx = midx_leaf_find(buf, leaf_hdr.node_keys,
                              key, value, inode_id);
    if (idx < 0) { free(buf); return OBMAFS3_ERR_NOTFOUND; }

    size_t rec_sz = sizeof(struct metadata_idx_record);
    leaf_hdr.node_keys--;

    if (leaf_hdr.node_keys == 0) {
        if (depth == 0) {
            ctx->metadata_idx_hdr.root_node_lba = 0;
            ctx->metadata_idx_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.metadata_idx_lba,
                                            &ctx->metadata_idx_hdr);
            meta_free_node(ctx, lba);
        } else {
            uint64_t plba  = path[depth - 1].lba;
            uint16_t pslot = path[depth - 1].slot;

            uint8_t *pbuf = calloc(1, nsz);
            if (!pbuf) { free(buf); return OBMAFS3_ERR_NOMEM; }

            rc = meta_node_read(ctx, plba, pbuf);
            if (rc != OBMAFS3_OK) {
                free(pbuf); free(buf); return rc;
            }

            struct btree_node_header phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));

            uint8_t *pdata = pbuf + sizeof(struct btree_node_header);
            size_t ie_sz = sizeof(struct metadata_idx_index_entry);

            if (pslot < phdr.node_keys - 1)
                memmove(pdata + (size_t)pslot * ie_sz,
                        pdata + ((size_t)pslot + 1) * ie_sz,
                        ((size_t)phdr.node_keys -
                         (size_t)pslot - 1) * ie_sz);
            phdr.node_keys--;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);

            if (phdr.node_keys == 0 && depth == 1) {
                ctx->metadata_idx_hdr.root_node_lba = 0;
                ctx->metadata_idx_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(
                    ctx, ctx->sb.metadata_idx_lba,
                    &ctx->metadata_idx_hdr);
                meta_free_node(ctx, lba);
                meta_free_node(ctx, plba);
            } else if (phdr.node_keys == 1 && depth == 1) {
                struct metadata_idx_index_entry remaining;
                memcpy(&remaining, pdata, sizeof(remaining));
                ctx->metadata_idx_hdr.root_node_lba =
                    remaining.child_lba;
                ctx->metadata_idx_hdr.total_nodes -= 2;
                rc = obmafs3_btree_header_write(
                    ctx, ctx->sb.metadata_idx_lba,
                    &ctx->metadata_idx_hdr);
                meta_free_node(ctx, lba);
                meta_free_node(ctx, plba);
            } else {
                memcpy(pbuf, &phdr, sizeof(phdr));
                compute_node_checksum(pbuf);
                rc = meta_node_write(ctx, plba, pbuf);
                if (rc == OBMAFS3_OK) {
                    ctx->metadata_idx_hdr.total_nodes--;
                    rc = obmafs3_btree_header_write(
                        ctx, ctx->sb.metadata_idx_lba,
                        &ctx->metadata_idx_hdr);
                }
                meta_free_node(ctx, lba);
            }
            free(pbuf);
        }
    } else {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        if ((uint16_t)idx < leaf_hdr.node_keys)
            memmove(data + (size_t)idx * rec_sz,
                    data + ((size_t)idx + 1) * rec_sz,
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

int obmafs3_metadata_get(struct obmafs3_ctx *ctx, uint64_t inode_id,
                         const char *key, char *value, size_t value_size)
{
    if (ctx->sb.metadata_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    struct metadata_record rec;
    int rc = meta_tree_lookup(ctx, inode_id, key, &rec);
    if (rc != OBMAFS3_OK)
        return rc;

    strncpy(value, rec.value, value_size);
    if (value_size > 0)
        value[value_size - 1] = '\0';
    return OBMAFS3_OK;
}

int obmafs3_metadata_put(struct obmafs3_ctx *ctx, uint64_t inode_id,
                         const char *key, const char *value)
{
    if (ctx->sb.metadata_lba == 0 || ctx->sb.metadata_idx_lba == 0)
        return OBMAFS3_ERR_INVAL;
    if (!key || strlen(key) == 0 || strlen(key) > 255)
        return OBMAFS3_ERR_INVAL;
    if (!value || strlen(value) > 1024)
        return OBMAFS3_ERR_INVAL;

    /* If key already exists, remove old index entry first */
    struct metadata_record old_rec;
    int rc = meta_tree_lookup(ctx, inode_id, key, &old_rec);
    if (rc == OBMAFS3_OK) {
        /* Remove old reverse-index entry */
        midx_tree_delete(ctx, old_rec.key, old_rec.value, inode_id);
    } else if (rc != OBMAFS3_ERR_NOTFOUND) {
        return rc;
    }

    /* Build and insert the per-image record */
    struct metadata_record new_rec;
    memset(&new_rec, 0, sizeof(new_rec));
    new_rec.inode_id = inode_id;
    strncpy(new_rec.key, key, METADATA_KEY_MAX - 1);
    strncpy(new_rec.value, value, METADATA_VALUE_MAX - 1);

    rc = meta_tree_put(ctx, &new_rec);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Insert reverse-index entry */
    struct metadata_idx_record idx_rec;
    memset(&idx_rec, 0, sizeof(idx_rec));
    strncpy(idx_rec.key, key, METADATA_KEY_MAX - 1);
    strncpy(idx_rec.value, value, METADATA_VALUE_MAX - 1);
    idx_rec.inode_id = inode_id;

    return midx_tree_put(ctx, &idx_rec);
}

int obmafs3_metadata_delete(struct obmafs3_ctx *ctx, uint64_t inode_id,
                            const char *key)
{
    if (ctx->sb.metadata_lba == 0)
        return OBMAFS3_ERR_NOTFOUND;

    /* Look up the value so we can delete the index entry */
    struct metadata_record rec;
    int rc = meta_tree_lookup(ctx, inode_id, key, &rec);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Delete from per-image tree */
    rc = meta_tree_delete(ctx, inode_id, key);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Delete from reverse-index tree */
    if (ctx->sb.metadata_idx_lba != 0)
        midx_tree_delete(ctx, rec.key, rec.value, inode_id);

    return OBMAFS3_OK;
}

int obmafs3_metadata_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    if (ctx->sb.metadata_lba == 0)
        return OBMAFS3_OK;

    while (1) {
        char **keys;
        uint32_t count;
        int rc = obmafs3_metadata_list(ctx, inode_id, &keys, &count);
        if (rc != OBMAFS3_OK)
            return rc;
        if (count == 0) {
            obmafs3_metadata_list_free(keys, count);
            return OBMAFS3_OK;
        }

        char first_key[METADATA_KEY_MAX];
        strncpy(first_key, keys[0], METADATA_KEY_MAX);
        first_key[METADATA_KEY_MAX - 1] = '\0';
        obmafs3_metadata_list_free(keys, count);

        rc = obmafs3_metadata_delete(ctx, inode_id, first_key);
        if (rc != OBMAFS3_OK)
            return rc;
    }
}

int obmafs3_metadata_list(struct obmafs3_ctx *ctx, uint64_t inode_id,
                          char ***keys, uint32_t *count)
{
    *keys  = NULL;
    *count = 0;

    if (ctx->sb.metadata_lba == 0)
        return OBMAFS3_OK;

    uint64_t lba = ctx->metadata_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_OK;

    size_t nsz = meta_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    /* Traverse to the leaf that would contain (inode_id, "") */
    while (1) {
        int rc = meta_node_read(ctx, lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0) break;

        uint16_t slot = meta_index_find(buf, hdr.node_keys,
                                        inode_id, "");
        struct metadata_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain */
    uint32_t cap = 16;
    char **kl = malloc(cap * sizeof(char *));
    if (!kl) { free(buf); return OBMAFS3_ERR_NOMEM; }

    uint32_t n = 0;

    while (1) {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for (uint16_t i = 0; i < hdr.node_keys; i++) {
            struct metadata_record rec;
            memcpy(&rec,
                   data + (size_t)i * sizeof(rec),
                   sizeof(rec));

            if (rec.inode_id == inode_id) {
                if (n >= cap) {
                    cap *= 2;
                    char **tmp = realloc(kl, cap * sizeof(char *));
                    if (!tmp) {
                        for (uint32_t j = 0; j < n; j++) free(kl[j]);
                        free(kl); free(buf);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    kl = tmp;
                }
                kl[n] = strndup(rec.key, METADATA_KEY_MAX);
                if (!kl[n]) {
                    for (uint32_t j = 0; j < n; j++) free(kl[j]);
                    free(kl); free(buf);
                    return OBMAFS3_ERR_NOMEM;
                }
                n++;
            } else if (rec.inode_id > inode_id) {
                goto list_done;
            }
        }

        if (hdr.right_link == 0) break;

        int rc = meta_node_read(ctx, hdr.right_link, buf);
        if (rc != OBMAFS3_OK) {
            for (uint32_t j = 0; j < n; j++) free(kl[j]);
            free(kl); free(buf);
            return rc;
        }
    }

list_done:
    free(buf);
    *keys  = kl;
    *count = n;
    return OBMAFS3_OK;
}

void obmafs3_metadata_list_free(char **keys, uint32_t count)
{
    if (!keys) return;
    for (uint32_t i = 0; i < count; i++)
        free(keys[i]);
    free(keys);
}

int obmafs3_metadata_query(struct obmafs3_ctx *ctx,
                           const char *key, const char *value,
                           char ***paths, uint32_t *count)
{
    *paths = NULL;
    *count = 0;

    if (ctx->sb.metadata_idx_lba == 0)
        return OBMAFS3_OK;

    uint64_t lba = ctx->metadata_idx_hdr.root_node_lba;
    if (lba == 0)
        return OBMAFS3_OK;

    size_t nsz = meta_node_size(ctx);
    uint8_t *buf = calloc(1, nsz);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    /* Traverse to the leaf that would contain (key, value, 0) */
    while (1) {
        int rc = meta_node_read(ctx, lba, buf);
        if (rc != OBMAFS3_OK) { free(buf); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf); return OBMAFS3_ERR_BADMAGIC;
        }

        if (hdr.level == 0) break;

        uint16_t slot = midx_index_find(buf, hdr.node_keys,
                                         key, value, 0);
        struct metadata_idx_index_entry ie;
        memcpy(&ie,
               buf + sizeof(struct btree_node_header)
                   + (size_t)slot * sizeof(ie),
               sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain for entries matching (key, value) */
    uint32_t cap = 16;
    char **result = malloc(cap * sizeof(char *));
    if (!result) { free(buf); return OBMAFS3_ERR_NOMEM; }

    uint32_t n = 0;

    while (1) {
        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for (uint16_t i = 0; i < hdr.node_keys; i++) {
            struct metadata_idx_record rec;
            memcpy(&rec,
                   data + (size_t)i * sizeof(rec),
                   sizeof(rec));

            int kcmp = strncmp(rec.key, key, METADATA_KEY_MAX);
            if (kcmp < 0) continue;
            if (kcmp > 0) goto query_done;

            int vcmp = strncmp(rec.value, value, METADATA_VALUE_MAX);
            if (vcmp < 0) continue;
            if (vcmp > 0) goto query_done;

            /* key and value match — resolve inode to path */
            char path[4096];
            int prc = obmafs3_resolve_inode_path(ctx, rec.inode_id,
                                                 path, sizeof(path));
            if (prc != OBMAFS3_OK)
                continue;  /* skip unresolvable inodes */

            if (n >= cap) {
                cap *= 2;
                char **tmp = realloc(result, cap * sizeof(char *));
                if (!tmp) {
                    for (uint32_t j = 0; j < n; j++) free(result[j]);
                    free(result); free(buf);
                    return OBMAFS3_ERR_NOMEM;
                }
                result = tmp;
            }
            result[n] = strdup(path);
            if (!result[n]) {
                for (uint32_t j = 0; j < n; j++) free(result[j]);
                free(result); free(buf);
                return OBMAFS3_ERR_NOMEM;
            }
            n++;
        }

        if (hdr.right_link == 0) break;

        int rc = meta_node_read(ctx, hdr.right_link, buf);
        if (rc != OBMAFS3_OK) {
            for (uint32_t j = 0; j < n; j++) free(result[j]);
            free(result); free(buf);
            return rc;
        }
    }

query_done:
    free(buf);
    *paths = result;
    *count = n;
    return OBMAFS3_OK;
}

void obmafs3_metadata_query_free(char **paths, uint32_t count)
{
    if (!paths) return;
    for (uint32_t i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
}
