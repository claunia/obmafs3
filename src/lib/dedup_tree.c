// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_tree.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Leaf scan, tree list management, tree operations, traversal.
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

#include "dedup_internal.h"

/* ------------------------------------------------------------------ */
/*  One-time full leaf scan to warm the key set                        */
/* ------------------------------------------------------------------ */

/**
 * Collect all leaf LBAs by DFS traversal of B+Tree index nodes.
 *
 * Index nodes are read through the node cache (fast after the first
 * write).  Leaf nodes are NOT read here — only their LBAs are
 * collected for batch reading later.
 *
 * @param ctx        Filesystem context.
 * @param hdr        B+Tree header.
 * @param buf        Scratch buffer (at least block_size bytes).
 * @param nc         Node cache (may be NULL — direct I/O fallback).
 * @param out_lbas   Receives allocated array of leaf LBAs (caller frees).
 * @param out_count  Receives the number of leaf LBAs.
 * @return OBMAFS3_OK on success, error code otherwise.
 */
int collect_all_leaf_lbas(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                                 uint8_t *buf, struct dedup_node_cache *nc,
                                 uint64_t **out_lbas, uint64_t *out_count)
{
    size_t bsz = (size_t)ctx->sb.block_size;

    *out_lbas  = NULL;
    *out_count = 0;

    if(hdr->root_node_lba == 0) return OBMAFS3_OK;

    /* Dynamic arrays for the DFS stack and collected leaf LBAs. */
    uint64_t leaf_cap = 4096, leaf_n = 0;
    uint64_t *leaves = malloc(leaf_cap * sizeof(uint64_t));
    if(!leaves) return OBMAFS3_ERR_NOMEM;

    uint64_t stk_cap = 256, stk_n = 0;
    uint64_t *stk = malloc(stk_cap * sizeof(uint64_t));
    if(!stk) { free(leaves); return OBMAFS3_ERR_NOMEM; }

    stk[stk_n++] = hdr->root_node_lba;

    while(stk_n > 0)
    {
        uint64_t lba = stk[--stk_n];

        int rc;
        if(nc) rc = dedup_cache_read(nc, ctx, lba, buf, bsz);
        else   rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(leaves); free(stk); return rc; }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(leaves);
            free(stk);
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic in warmup");
        }

        if(nhdr.level == 0)
        {
            /* Root is a leaf (single-level tree) — record its LBA. */
            if(leaf_n >= leaf_cap)
            {
                leaf_cap *= 2;
                uint64_t *tmp = realloc(leaves, leaf_cap * sizeof(uint64_t));
                if(!tmp) { free(leaves); free(stk); return OBMAFS3_ERR_NOMEM; }
                leaves = tmp;
            }
            leaves[leaf_n++] = lba;
        }
        else
        {
            /* Index node — extract children. */
            const uint8_t *data = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, data + (size_t)i * sizeof(ie), sizeof(ie));

                if(nhdr.level == 1)
                {
                    /* Level-1 node: children are leaves — collect
                     * their LBAs directly without reading them. */
                    if(leaf_n >= leaf_cap)
                    {
                        leaf_cap *= 2;
                        uint64_t *tmp = realloc(leaves, leaf_cap * sizeof(uint64_t));
                        if(!tmp) { free(leaves); free(stk); return OBMAFS3_ERR_NOMEM; }
                        leaves = tmp;
                    }
                    leaves[leaf_n++] = ie.child_lba;
                }
                else
                {
                    /* Level > 1: push child for further traversal. */
                    if(stk_n >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stk, stk_cap * sizeof(uint64_t));
                        if(!tmp) { free(leaves); free(stk); return OBMAFS3_ERR_NOMEM; }
                        stk = tmp;
                    }
                    stk[stk_n++] = ie.child_lba;
                }
            }
        }
    }

    free(stk);
    *out_lbas  = leaves;
    *out_count = leaf_n;
    return OBMAFS3_OK;
}

/**
 * Warm up the key set by reading every leaf in the B+Tree.
 *
 * Traverses all index nodes (cached — fast) to discover leaf LBAs,
 * sorts them for sequential disk access, then reads each uncached
 * leaf directly (NOT through the node cache) and ingests its keys.
 *
 * This avoids bloating the node cache with tens of thousands of
 * leaf buffers that are only needed for existence checks.  The key
 * set alone is sufficient for the hits-only fast path.
 *
 * Typical cost: one-time ~2-15 seconds on HDD (depends on tree size),
 * after which every hit-only write completes in <1ms.
 */
void keyset_warmup(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                   uint8_t *buf, struct dedup_node_cache *nc)
{
    struct dedup_key_set *ks = (struct dedup_key_set *)ctx->dedup_key_set;
    if(!ks || hdr->root_node_lba == 0) return;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    uint64_t *leaf_lbas = NULL;
    uint64_t  leaf_count = 0;
    int rc = collect_all_leaf_lbas(ctx, hdr, buf, nc, &leaf_lbas, &leaf_count);
    if(rc != OBMAFS3_OK || leaf_count == 0) { free(leaf_lbas); return; }

    /* Sort for sequential disk access. */
    qsort(leaf_lbas, (size_t)leaf_count, sizeof(uint64_t), lba_cmp);

    /* Deduplicate (the DFS may visit the root as a leaf in a
     * single-level tree, but mainly this removes nothing). */
    uint64_t unique = 0;
    for(uint64_t i = 0; i < leaf_count; i++)
    {
        if(unique > 0 && leaf_lbas[i] == leaf_lbas[unique - 1]) continue;
        leaf_lbas[unique++] = leaf_lbas[i];
    }
    leaf_count = unique;

    size_t bsz = (size_t)ctx->sb.block_size;

    /* Issue readahead hints for all uncached leaves. */
    for(uint64_t i = 0; i < leaf_count; i++)
    {
        if(nc && cache_find_slot(nc, leaf_lbas[i])) continue;
        posix_fadvise(ctx->fd, (off_t)(leaf_lbas[i] * bsz),
                      (off_t)bsz, POSIX_FADV_WILLNEED);
    }

    /* Read leaves and ingest their keys into the key set.
     * Cached leaves are ingested directly from the cache buffer.
     * Uncached leaves are read from disk (bypassing the node cache
     * to avoid bloating it with tens of thousands of leaf buffers). */
    uint64_t read_count = 0, cached_count = 0;
    for(uint64_t i = 0; i < leaf_count; i++)
    {
        if(ctx->shutdown_requested) { free(leaf_lbas); return; }
        if(nc)
        {
            struct dedup_cache_slot *slot = cache_find_slot(nc, leaf_lbas[i]);
            if(slot)
            {
                keyset_ingest_leaf(ks, slot->buf);
                cached_count++;
                continue;
            }
        }
        rc = obmafs3_block_read(ctx, leaf_lbas[i], buf, bsz);
        if(rc == OBMAFS3_OK)
        {
            keyset_ingest_leaf(ks, buf);
            read_count++;
        }
    }

    free(leaf_lbas);

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    fprintf(stderr,
            "[dedup-warmup] scanned %" PRIu64 " leaves (%" PRIu64 " disk, %" PRIu64 " cached) "
            "in %.1fms — key set now has %u keys\n",
            leaf_count, read_count, cached_count,
            timespec_diff_ms(&t_start, &t_end), ks->count);
}

/**
 * Wrappers that dispatch to the cache when available,
 * falling back to direct I/O when nc is NULL.
 * After reading, leaf nodes are ingested into the key set.
 */
int nc_block_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t bsz)
{
    int rc;
    if(nc) rc = dedup_cache_read(nc, ctx, lba, buf, bsz);
    else   rc = obmafs3_block_read(ctx, lba, buf, bsz);

    if(rc == OBMAFS3_OK)
        keyset_ingest_leaf((struct dedup_key_set *)ctx->dedup_key_set, buf);

    return rc;
}

/** Cache-aware block write: delegates to the node cache or a direct write. */
int nc_block_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, const void *buf,
                          size_t bsz)
{
    if(nc) return dedup_cache_write(nc, ctx, lba, buf, bsz);
    return obmafs3_block_write(ctx, lba, buf, bsz);
}

/* ------------------------------------------------------------------ */
/*  Dedup tree list management                                         */
/* ------------------------------------------------------------------ */

/**
 * Read the dedup tree list header and entries from disk.
 * Caller must free *entries when count > 0.
 */
int dedup_tree_list_read(struct obmafs3_ctx *ctx, struct tree_list_header *hdr, struct tree_list_entry **entries,
                                uint64_t *count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        free(buf);
        return rc;
    }

    memcpy(hdr, buf, sizeof(*hdr));
    if(hdr->magic != OBMAFS3_TREELIST_MAGIC)
    {
        free(buf);
        DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");
    }

    *count = hdr->tree_count;
    if(hdr->tree_count == 0)
    {
        *entries = NULL;
        free(buf);
        return OBMAFS3_OK;
    }

    /* Bounds-check: entries must fit within the block */
    size_t entries_size = (size_t)(hdr->tree_count * sizeof(struct tree_list_entry));
    if(sizeof(struct tree_list_header) + entries_size > (size_t)ctx->sb.block_size)
    {
        free(buf);
        DBG_RETURN(OBMAFS3_ERR_INVAL, "tree_count exceeds block capacity");
    }

    *entries = malloc(entries_size);
    if(!*entries)
    {
        free(buf);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    memcpy(*entries, buf + sizeof(struct tree_list_header), (size_t)(hdr->tree_count * sizeof(struct tree_list_entry)));

    free(buf);
    return OBMAFS3_OK;
}

/**
 * Write the dedup tree list header and entries to disk.
 */
int dedup_tree_list_write(struct obmafs3_ctx *ctx, struct tree_list_entry *entries, uint64_t count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    struct tree_list_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic      = OBMAFS3_TREELIST_MAGIC;
    hdr.tree_count = count;
    /* checksum computed below */

    memcpy(buf, &hdr, sizeof(hdr));
    if(count > 0) memcpy(buf + sizeof(hdr), entries, (size_t)(count * sizeof(struct tree_list_entry)));

    /* Compute checksum over the whole block contents */
    struct tree_list_header *hdr_buf = (struct tree_list_header *)buf;
    memset(hdr_buf->checksum, 0, sizeof(hdr_buf->checksum));
    obmafs3_checksum_block(buf, sizeof(hdr) + (size_t)(count * sizeof(struct tree_list_entry)), hdr_buf->checksum);

    int rc = obmafs3_block_write(ctx, ctx->sb.dedup_lba, buf, (size_t)ctx->sb.block_size);
    free(buf);
    return rc;
}

/**
 * Find the dedup tree for a given sector_size.
 * If none exists, create one and add it to the list.
 * Returns the tree header and its LBA on disk.
 */
int obmafs3_dedup_get_tree(struct obmafs3_ctx *ctx, uint16_t sector_size, struct btree_header *hdr, uint64_t *hdr_lba)
{
    struct tree_list_header list_hdr;
    struct tree_list_entry *entries = NULL;
    uint64_t                count   = 0;
    int                     rc;

    rc = dedup_tree_list_read(ctx, &list_hdr, &entries, &count);
    if(rc != OBMAFS3_OK) return rc;

    /* Search for existing tree with matching sector_size */
    for(uint64_t i = 0; i < count; i++)
    {
        if(entries[i].sector_size == sector_size)
        {
            *hdr_lba = entries[i].tree_lba;
            free(entries);
            return obmafs3_btree_header_read(ctx, *hdr_lba, hdr);
        }
    }

    /* Not found — create a new dedup tree */

    /* Allocate a block for the tree header */
    uint64_t new_hdr_lba;
    rc = obmafs3_alloc_block(ctx, &new_hdr_lba);
    if(rc != OBMAFS3_OK)
    {
        free(entries);
        return rc;
    }

    /* Allocate a block for the root node (empty sentinel) */
    uint64_t root_lba;
    rc = obmafs3_alloc_block(ctx, &root_lba);
    if(rc != OBMAFS3_OK)
    {
        obmafs3_free_block(ctx, new_hdr_lba);
        free(entries);
        return rc;
    }

    /* Write an empty root node */
    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!node_buf)
    {
        obmafs3_free_block(ctx, new_hdr_lba);
        obmafs3_free_block(ctx, root_lba);
        free(entries);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    struct btree_node_header root_hdr;
    memset(&root_hdr, 0, sizeof(root_hdr));
    root_hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    root_hdr.record_type = kBtreeDataTypeDeduplicationEntry;
    root_hdr.level       = 0; /* leaf node */
    root_hdr.node_keys   = 0; /* empty sentinel */
    root_hdr.keys_length = 0; /* no entries yet */
    memcpy(node_buf, &root_hdr, sizeof(root_hdr));
    compute_node_checksum(node_buf);
    rc = obmafs3_block_write(ctx, root_lba, node_buf, (size_t)ctx->sb.block_size);
    free(node_buf);
    if(rc != OBMAFS3_OK)
    {
        obmafs3_free_block(ctx, new_hdr_lba);
        obmafs3_free_block(ctx, root_lba);
        free(entries);
        return rc;
    }

    /* Write the tree header */
    struct btree_header new_hdr;
    memset(&new_hdr, 0, sizeof(new_hdr));
    new_hdr.magic             = OBMAFS3_BTREE_HDR_MAGIC;
    new_hdr.data_type         = kBtreeDataTypeDeduplicationEntry;
    new_hdr.root_node_lba     = root_lba;
    new_hdr.node_size         = (uint16_t)ctx->sb.block_size;
    new_hdr.total_nodes       = 1;
    new_hdr.tree_type         = kBtreeTypeDeduplication;
    new_hdr.last_block_lba    = 0; /* no partial block yet */
    new_hdr.last_block_offset = 0;

    rc = obmafs3_btree_header_write(ctx, new_hdr_lba, &new_hdr);
    if(rc != OBMAFS3_OK)
    {
        obmafs3_free_block(ctx, new_hdr_lba);
        obmafs3_free_block(ctx, root_lba);
        free(entries);
        return rc;
    }

    /* Add the new entry to the tree list */
    struct tree_list_entry *new_entries = realloc(entries, (size_t)((count + 1) * sizeof(struct tree_list_entry)));
    if(!new_entries)
    {
        free(entries);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }
    entries                    = new_entries;
    entries[count].sector_size = sector_size;
    entries[count].tree_lba    = new_hdr_lba;
    count++;

    rc = dedup_tree_list_write(ctx, entries, count);
    free(entries);
    if(rc != OBMAFS3_OK) return rc;

    *hdr     = new_hdr;
    *hdr_lba = new_hdr_lba;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Dedup tree operations                                              */
/* ------------------------------------------------------------------ */

/**
 * Binary-search the cached leaf for @p hash.
 * Returns OBMAFS3_OK if found, OBMAFS3_ERR_NOTFOUND otherwise.
 */
int dedup_leaf_cache_search(const struct dedup_leaf_cache *lc, uint64_t hash,
                                   struct dedup_entry *entry)
{
    const uint8_t *data = lc->leaf_buf + sizeof(struct btree_node_header);
    int lo = 0, hi = (int)lc->num_keys - 1;
    while(lo <= hi)
    {
        int      mid = lo + (hi - lo) / 2;
        uint64_t mid_hash;
        memcpy(&mid_hash, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(mid_hash));
        if(mid_hash == hash)
        {
            struct dedup_entry de;
            memcpy(&de, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(de));
            *entry = de;
            return OBMAFS3_OK;
        }
        if(mid_hash < hash) lo = mid + 1;
        else                hi = mid - 1;
    }
    return OBMAFS3_ERR_NOTFOUND;
}

/**
 * Populate the leaf cache from a raw leaf-node block buffer.
 */
void dedup_leaf_cache_populate(struct dedup_leaf_cache *lc, const uint8_t *buf,
                                      size_t block_size)
{
    if(!lc->leaf_buf)
    {
        lc->leaf_buf = malloc(block_size);
        if(!lc->leaf_buf) return; /* non-fatal: lookups just bypass the cache */
    }
    memcpy(lc->leaf_buf, buf, block_size);

    struct btree_node_header nhdr;
    memcpy(&nhdr, buf, sizeof(nhdr));
    lc->num_keys = nhdr.node_keys;

    const uint8_t *data = buf + sizeof(struct btree_node_header);
    if(nhdr.node_keys > 0)
    {
        memcpy(&lc->min_key, data, sizeof(uint64_t));
        memcpy(&lc->max_key, data + (size_t)(nhdr.node_keys - 1) * sizeof(struct dedup_entry), sizeof(uint64_t));
    }
    else
    {
        lc->min_key = 0;
        lc->max_key = 0;
    }
}

/**
 * Look up @p hash in the dedup tree, using the leaf cache to skip the
 * full root-to-leaf traversal when the target leaf is already cached.
 *
 * Falls back to the canonical @c obmafs3_dedup_lookup path when the
 * hash falls outside the cached key range, then updates the cache
 * with the newly-visited leaf.
 */
int dedup_lookup_cached(struct obmafs3_ctx *ctx,
                               const struct btree_header *hdr,
                               uint64_t hash,
                               struct dedup_entry *entry,
                               struct dedup_leaf_cache *lc)
{
    /* --- Fast path: check pending buffers first (same as obmafs3_dedup_lookup) --- */
    {
        const struct dedup_pending_buf *pb =
            (const struct dedup_pending_buf *)ctx->dedup_pending;
        if(pb)
        {
            const struct dedup_entry *pe = pending_lookup(pb, hash);
            if(pe) { *entry = *pe; return OBMAFS3_OK; }
        }
        const struct dedup_pending_buf *drain =
            (const struct dedup_pending_buf *)ctx->dedup_pending_draining;
        if(drain)
        {
            const struct dedup_entry *pe = pending_lookup(drain, hash);
            if(pe) { *entry = *pe; return OBMAFS3_OK; }
        }
    }

    /* --- Check leaf cache --- */
    if(lc->leaf_buf && lc->num_keys > 0 &&
       hash >= lc->min_key && hash <= lc->max_key)
    {
        int rc = dedup_leaf_cache_search(lc, hash, entry);
        if(rc == OBMAFS3_OK) return OBMAFS3_OK;
        /* Hash was in range but not found — it doesn't exist in this
         * leaf, so a full traversal would land on the same leaf.
         * Return NOTFOUND directly. */
        return OBMAFS3_ERR_NOTFOUND;
    }

    /* --- Cache miss: full root-to-leaf traversal, capturing the leaf --- */
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
    int use_cache = (nc != NULL);

    uint64_t lba    = hdr->root_node_lba;
    int      result = OBMAFS3_ERR_NOTFOUND;

    while(1)
    {
        int rc;
        if(use_cache)
            rc = dedup_cache_read(nc, ctx, lba, buf, (size_t)ctx->sb.block_size);
        else
            rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) { result = rc; break; }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            if(obmafs3_debug)
                fprintf(stderr, "OBMAFS3 ERR %d [%s:%d %s] bad magic (leaf cache)\n",
                        OBMAFS3_ERR_BADMAGIC, __FILE__, __LINE__, __func__);
            result = OBMAFS3_ERR_BADMAGIC;
            break;
        }

        if(nhdr.level > 0)
        {
            /* Index node — descend */
            const uint8_t *data = buf + sizeof(struct btree_node_header);
            uint16_t       slot = 0;
            int            lo = 0, hi = (int)nhdr.node_keys - 1;
            while(lo <= hi)
            {
                int      mid = lo + (hi - lo) / 2;
                uint64_t mid_key;
                memcpy(&mid_key, data + (size_t)mid * sizeof(struct btree_index_entry), sizeof(mid_key));
                if(mid_key <= hash) { slot = (uint16_t)mid; lo = mid + 1; }
                else                { hi = mid - 1; }
            }
            struct btree_index_entry ie;
            memcpy(&ie, data + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
            continue;
        }

        /* Leaf — cache it and search */
        dedup_leaf_cache_populate(lc, buf, (size_t)ctx->sb.block_size);

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        int            lo = 0, hi = (int)nhdr.node_keys - 1;
        while(lo <= hi)
        {
            int      mid = lo + (hi - lo) / 2;
            uint64_t mid_hash;
            memcpy(&mid_hash, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(mid_hash));
            if(mid_hash == hash)
            {
                struct dedup_entry de;
                memcpy(&de, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(de));
                *entry = de;
                result = OBMAFS3_OK;
                goto done;
            }
            if(mid_hash < hash) lo = mid + 1;
            else                hi = mid - 1;
        }
        result = OBMAFS3_ERR_NOTFOUND;
        break;
    }
done:
    return result;
}

/**
 * Issue a @c posix_fadvise(POSIX_FADV_WILLNEED) hint for the dedup
 * data block that the @e next sector will need.
 *
 * Called after reading the current dedup block so the kernel can
 * prefetch the next one into the page cache while we decompress and
 * copy the current one.  The lookup uses the leaf cache, so in the
 * common case (next hash in the same leaf) it is a pure in-memory
 * binary search with no I/O overhead.
 *
 * If the next sector's hash resolves to the same block we just read,
 * or if the lookup fails (e.g. hash outside cached leaf), we skip
 * the hint — it's purely advisory so errors are silently ignored.
 *
 * @param ctx           Filesystem context.
 * @param hdr           Dedup tree header.
 * @param next_hash     Hash of the next sector's data.
 * @param current_lba   LBA of the dedup block we just read.
 * @param lc            Leaf-level lookup cache.
 */
void dedup_readahead_next(struct obmafs3_ctx *ctx,
                                 const struct btree_header *hdr,
                                 uint64_t next_hash,
                                 uint64_t current_lba,
                                 struct dedup_leaf_cache *lc)
{
    struct dedup_entry de;
    int rc = dedup_lookup_cached(ctx, hdr, next_hash, &de, lc);
    if(rc != OBMAFS3_OK || de.block_lba == current_lba) return;

    /* Advise the kernel to prefetch the next dedup block.  We don't
     * know its on-disk size yet, so use dedup_block_size as the upper
     * bound — the kernel will clamp to the file size automatically. */
    off_t    off = (off_t)(de.block_lba * ctx->sb.block_size);
    off_t    len = (off_t)ctx->sb.dedup_block_size;
    posix_fadvise(ctx->fd, off, len, POSIX_FADV_WILLNEED);
}

/**
 * Look up a hash in the given dedup tree.
 * Returns OBMAFS3_OK if found, OBMAFS3_ERR_NOTFOUND if not.
 *
 * B+Tree traversal: descend through index nodes to the correct leaf,
 * then binary-search among sorted dedup_entry records.
 *
 * When the node cache is available, traversal goes through the cache
 * under tree_lock to guarantee a consistent view even when dirty
 * nodes have not yet been flushed to disk.
 */
int obmafs3_dedup_lookup(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash,
                         struct dedup_entry *entry)
{
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
    int use_cache = (nc != NULL);

    /* Callers hold tree_lock (rdlock from FUSE readers, wrlock from
     * writers), so we must NOT take tree_lock here — that would
     * deadlock.  The cache has its own internal mutex for thread
     * safety; the tree_lock already guarantees tree-structure
     * stability. */

    /* Check pending insert buffers first — O(1), no disk I/O.
     * Entries deferred by the write path live in the active buffer;
     * entries being drained by the housekeeping thread live in the
     * draining buffer.  Check both.
     *
     * These accesses are safe because the housekeeping thread only
     * swaps the pending/draining pointers under wrlock, and readers
     * hold rdlock which prevents that swap. */
    {
        const struct dedup_pending_buf *pb =
            (const struct dedup_pending_buf *)ctx->dedup_pending;
        if(pb)
        {
            const struct dedup_entry *pe = pending_lookup(pb, hash);
            if(pe)
            {
                *entry = *pe;
                return OBMAFS3_OK;
            }
        }
        const struct dedup_pending_buf *drain =
            (const struct dedup_pending_buf *)ctx->dedup_pending_draining;
        if(drain)
        {
            const struct dedup_entry *pe = pending_lookup(drain, hash);
            if(pe)
            {
                *entry = *pe;
                return OBMAFS3_OK;
            }
        }
    }

    uint64_t lba = hdr->root_node_lba;
    int      result = OBMAFS3_ERR_NOTFOUND;

    while(1)
    {
        int rc;
        if(use_cache)
            rc = dedup_cache_read(nc, ctx, lba, buf, (size_t)ctx->sb.block_size);
        else
            rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) { result = rc; break; }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            if(obmafs3_debug)
                fprintf(stderr, "OBMAFS3 ERR %d [%s:%d %s] bad magic\n",
                        OBMAFS3_ERR_BADMAGIC, __FILE__, __LINE__, __func__);
            result = OBMAFS3_ERR_BADMAGIC;
            break;
        }

        if(nhdr.level > 0)
        {
            /* Index node: binary search for the child to follow */
            const uint8_t *data = buf + sizeof(struct btree_node_header);
            uint16_t       slot = 0;
            int            lo = 0, hi = (int)nhdr.node_keys - 1;
            while(lo <= hi)
            {
                int      mid = lo + (hi - lo) / 2;
                uint64_t mid_key;
                memcpy(&mid_key, data + (size_t)mid * sizeof(struct btree_index_entry), sizeof(mid_key));
                if(mid_key <= hash)
                {
                    slot = (uint16_t)mid;
                    lo   = mid + 1;
                }
                else
                {
                    hi = mid - 1;
                }
            }

            struct btree_index_entry ie;
            memcpy(&ie, data + (size_t)slot * sizeof(ie), sizeof(ie));

            lba = ie.child_lba;
            continue;
        }

        /* Leaf node: binary search among sorted dedup_entry records */
        const uint8_t *data = buf + sizeof(struct btree_node_header);
        int            lo = 0, hi = (int)nhdr.node_keys - 1;
        while(lo <= hi)
        {
            int      mid = lo + (hi - lo) / 2;
            uint64_t mid_hash;
            memcpy(&mid_hash, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(mid_hash));
            if(mid_hash == hash)
            {
                struct dedup_entry de;
                memcpy(&de, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(de));
                *entry = de;
                result = OBMAFS3_OK;
                goto done;
            }
            if(mid_hash < hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }

        result = OBMAFS3_ERR_NOTFOUND;
        break;
    }

done:
    return result;
}

/**
 * Maximum number of dedup_entry records that fit in one leaf node.
 */
static uint16_t dedup_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct dedup_entry));
}

/**
 * Maximum number of btree_index_entry records in one index node.
 */
static uint16_t dedup_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct btree_index_entry));
}

/* dedup_btree_path, DEDUP_BTREE_MAX_DEPTH and dedup_upsert_ctx are
 * defined earlier in this file (above pending_flush). */

/* ---- Comparison for sorting LBAs (used by leaf prefetch) ---- */
int lba_cmp(const void *a, const void *b)
{
    uint64_t la = *(const uint64_t *)a;
    uint64_t lb = *(const uint64_t *)b;
    return (la < lb) ? -1 : (la > lb) ? 1 : 0;
}

/**
 * Lightweight tree traversal: find which leaf node would contain @hash
 * by descending through index nodes only.  Stops at level 1 and returns
 * the child_lba (the leaf), so the leaf itself is NOT read from disk.
 *
 * Index nodes are read through the node cache (nc), so after the first
 * write most or all index reads are cache hits.
 *
 * For a single-level tree (root is the leaf), the root LBA is returned
 * and it IS read (unavoidable).
 *
 * @param out_leaf_lba  Receives the LBA of the target leaf node.
 * @return OBMAFS3_OK on success, error code otherwise.
 */
int dedup_find_leaf_lba(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                               uint64_t hash, uint64_t *out_leaf_lba,
                               uint8_t *buf, struct dedup_node_cache *nc)
{
    size_t   bsz = (size_t)ctx->sb.block_size;
    uint64_t lba = hdr->root_node_lba;

    if(lba == 0) { *out_leaf_lba = 0; return OBMAFS3_OK; }

    while(1)
    {
        int rc = nc_block_read(nc, ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.level == 0)
        {
            /* Root is the only node (single-level tree). */
            *out_leaf_lba = lba;
            return OBMAFS3_OK;
        }

        /* Index node — binary search for the correct child. */
        const uint8_t *data = buf + sizeof(struct btree_node_header);
        uint16_t       slot = 0;
        int            lo = 0, hi = (int)nhdr.node_keys - 1;
        while(lo <= hi)
        {
            int      mid = lo + (hi - lo) / 2;
            uint64_t mid_key;
            memcpy(&mid_key, data + (size_t)mid * sizeof(struct btree_index_entry), sizeof(mid_key));
            if(mid_key <= hash) { slot = (uint16_t)mid; lo = mid + 1; }
            else                { hi = mid - 1; }
        }

        struct btree_index_entry ie;
        memcpy(&ie, data + (size_t)slot * sizeof(ie), sizeof(ie));

        if(nhdr.level == 1)
        {
            /* Next level is leaf — return its LBA without reading it. */
            *out_leaf_lba = ie.child_lba;
            return OBMAFS3_OK;
        }

        lba = ie.child_lba;
    }
}

/**
 * Phase 1 of upsert: traverse from root to leaf looking for @hash.
 *
 * If found, fills @existing and returns OBMAFS3_OK.
 * If not found, saves traversal state in @uctx (path, leaf LBA,
 * insert position, leaf header) and returns OBMAFS3_ERR_NOTFOUND.
 * The caller can then call dedup_upsert_insert() to store a new
 * entry without re-traversing the tree.
 *
 * @buf is a caller-owned buffer of at least block_size bytes.
 * On return it holds the leaf node.
 */
int dedup_upsert_find(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash,
                             struct dedup_entry *existing, struct dedup_upsert_ctx *uctx, uint8_t *buf,
                             struct dedup_node_cache *nc)
{
    size_t   bsz = (size_t)ctx->sb.block_size;
    uint64_t lba = hdr->root_node_lba;
    uctx->depth  = 0;

    while(1)
    {
        int rc = nc_block_read(nc, ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
            DBG_RETURN(OBMAFS3_ERR_BADMAGIC,
                       "lba=%" PRIu64 " got=0x%" PRIx64 " expected=0x%" PRIx64,
                       lba, nhdr.magic, (uint64_t)OBMAFS3_BTREE_NODE_MAGIC);

        if(nhdr.level > 0)
        {
            /* Index node */
            if(uctx->depth >= DEDUP_BTREE_MAX_DEPTH) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

            const uint8_t *data = buf + sizeof(struct btree_node_header);
            uint16_t       slot = 0;
            int            lo = 0, hi = (int)nhdr.node_keys - 1;
            while(lo <= hi)
            {
                int      mid = lo + (hi - lo) / 2;
                uint64_t mid_key;
                memcpy(&mid_key, data + (size_t)mid * sizeof(struct btree_index_entry), sizeof(mid_key));
                if(mid_key <= hash)
                {
                    slot = (uint16_t)mid;
                    lo   = mid + 1;
                }
                else
                {
                    hi = mid - 1;
                }
            }

            uctx->path[uctx->depth].lba  = lba;
            uctx->path[uctx->depth].slot = slot;
            uctx->depth++;

            struct btree_index_entry ie;
            memcpy(&ie, data + (size_t)slot * sizeof(ie), sizeof(ie));
            lba = ie.child_lba;
            continue;
        }

        /* Leaf node: binary search */
        const uint8_t *data = buf + sizeof(struct btree_node_header);
        int            lo = 0, hi = (int)nhdr.node_keys - 1;
        while(lo <= hi)
        {
            int      mid = lo + (hi - lo) / 2;
            uint64_t mid_hash;
            memcpy(&mid_hash, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(mid_hash));
            if(mid_hash == hash)
            {
                memcpy(existing, data + (size_t)mid * sizeof(struct dedup_entry), sizeof(*existing));
                return OBMAFS3_OK;
            }
            if(mid_hash < hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }

        /* Not found — save state for insert */
        uctx->leaf_lba   = lba;
        uctx->insert_pos = lo;
        memcpy(&uctx->leaf_hdr, &nhdr, sizeof(nhdr));
        return OBMAFS3_ERR_NOTFOUND;
    }
}

/**
 * Phase 2 of upsert: insert @entry at the position found by
 * dedup_upsert_find().
 *
 * @buf must still contain the leaf node from the find phase.
 * Updates @hdr in memory (total_nodes, root_node_lba) but does NOT
 * write the btree header to disk — the caller is responsible for that.
 */
int dedup_upsert_insert(struct obmafs3_ctx *ctx, struct btree_header *hdr, const struct dedup_entry *entry,
                               struct dedup_upsert_ctx *uctx, uint8_t *buf, struct dedup_node_cache *nc)
{
    size_t                   bsz        = (size_t)ctx->sb.block_size;
    uint64_t                 lba        = uctx->leaf_lba;
    int                      insert_pos = uctx->insert_pos;
    struct btree_node_header leaf_hdr   = uctx->leaf_hdr;
    int                      rc;

    uint16_t max_leaf = dedup_leaf_max_keys(ctx);
    size_t   rec_sz   = sizeof(struct dedup_entry);

    if(leaf_hdr.node_keys < max_leaf)
    {
        /* Room in leaf — sorted insert */
        uint8_t *data = buf + sizeof(struct btree_node_header);

        if(insert_pos < leaf_hdr.node_keys)
            memmove(data + ((size_t)insert_pos + 1) * rec_sz, data + (size_t)insert_pos * rec_sz,
                    ((size_t)leaf_hdr.node_keys - (size_t)insert_pos) * rec_sz);

        memcpy(data + (size_t)insert_pos * rec_sz, entry, rec_sz);
        leaf_hdr.node_keys++;
        leaf_hdr.keys_length = (uint16_t)(leaf_hdr.node_keys * rec_sz);
        memcpy(buf, &leaf_hdr, sizeof(leaf_hdr));
        compute_node_checksum(buf);

        return nc_block_write(nc, ctx, lba, buf, bsz);
    }

    /* ---- Leaf is full: split ---- */
    uint16_t            total = max_leaf + 1;
    struct dedup_entry *all   = calloc(total, rec_sz);
    if(!all) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    uint8_t *ld = buf + sizeof(struct btree_node_header);

    /* Build sorted array including the new entry */
    memcpy(all, ld, (size_t)insert_pos * rec_sz);
    all[insert_pos] = *entry;
    memcpy(&all[insert_pos + 1], ld + (size_t)insert_pos * rec_sz, ((size_t)max_leaf - (size_t)insert_pos) * rec_sz);

    uint16_t left_count  = total / 2;
    uint16_t right_count = total - left_count;

    /* Rewrite old leaf with left half */
    memset(ld, 0, bsz - sizeof(struct btree_node_header));
    memcpy(ld, all, (size_t)left_count * rec_sz);

    uint64_t old_right = leaf_hdr.right_link;

    uint64_t new_leaf_lba;
    rc = obmafs3_btree_alloc_node(ctx, hdr, 0, &new_leaf_lba);
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
    rc = nc_block_write(nc, ctx, lba, buf, bsz);
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
    nh.record_type = kBtreeDataTypeDeduplicationEntry;
    nh.level       = 0;
    nh.node_keys   = right_count;
    nh.keys_length = (uint16_t)(right_count * rec_sz);
    nh.left_link   = lba;
    nh.right_link  = old_right;
    memcpy(buf, &nh, sizeof(nh));
    memcpy(buf + sizeof(nh), &all[left_count], (size_t)right_count * rec_sz);
    compute_node_checksum(buf);
    rc = nc_block_write(nc, ctx, new_leaf_lba, buf, bsz);
    if(rc != OBMAFS3_OK)
    {
        free(all);
        return rc;
    }

    uint64_t push_key       = all[left_count].hash;
    uint64_t push_child     = new_leaf_lba;
    uint64_t left_first_key = all[0].hash;
    uint64_t left_lba       = lba;

    free(all);
    hdr->total_nodes++;

    /* Update old right neighbor's left_link */
    if(old_right != 0)
    {
        rc = nc_block_read(nc, ctx, old_right, buf, bsz);
        if(rc == OBMAFS3_OK)
        {
            struct btree_node_header rnh;
            memcpy(&rnh, buf, sizeof(rnh));
            rnh.left_link = new_leaf_lba;
            memcpy(buf, &rnh, sizeof(rnh));
            compute_node_checksum(buf);
            nc_block_write(nc, ctx, old_right, buf, bsz);
        }
    }

    /* ---- Propagate split upward through index nodes ---- */
    int depth = uctx->depth;
    while(depth > 0)
    {
        depth--;
        uint64_t parent_lba  = uctx->path[depth].lba;
        uint16_t parent_slot = uctx->path[depth].slot;

        rc = nc_block_read(nc, ctx, parent_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header phdr;
        memcpy(&phdr, buf, sizeof(phdr));

        uint16_t max_idx    = dedup_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct btree_index_entry);

        if(phdr.node_keys < max_idx)
        {
            /* Room in parent — insert */
            uint8_t *id = buf + sizeof(struct btree_node_header);

            /* Update the key at parent_slot to the left child's
             * actual minimum.  Without this, the parent key can
             * be stale (higher than the true minimum) after the
             * leftmost child accumulated entries with keys below
             * the original index key. */
            struct btree_index_entry upd;
            memcpy(&upd, id + (size_t)parent_slot * ie_sz, sizeof(upd));
            upd.key = left_first_key;
            memcpy(id + (size_t)parent_slot * ie_sz, &upd, sizeof(upd));

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

            return nc_block_write(nc, ctx, parent_lba, buf, bsz);
        }

        /* Parent is full — split the index node */
        uint16_t                  idx_total = max_idx + 1;
        struct btree_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        uint8_t *id = buf + sizeof(struct btree_node_header);

        /* Update the key at parent_slot to the left child's
         * actual minimum before building the merged array. */
        struct btree_index_entry upd;
        memcpy(&upd, id + (size_t)parent_slot * ie_sz, sizeof(upd));
        upd.key = left_first_key;
        memcpy(id + (size_t)parent_slot * ie_sz, &upd, sizeof(upd));

        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        aie[idx_insert].key       = push_key;
        aie[idx_insert].child_lba = push_child;
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        /* Allocate new index node before writing so we can set sibling links */
        uint64_t idx_old_right = phdr.right_link;
        uint64_t new_idx_lba;
        rc = obmafs3_btree_alloc_node(ctx, hdr, 0, &new_idx_lba);
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
        rc = nc_block_write(nc, ctx, parent_lba, buf, bsz);
        if(rc != OBMAFS3_OK)
        {
            free(aie);
            return rc;
        }

        memset(buf, 0, bsz);
        struct btree_node_header nih;
        memset(&nih, 0, sizeof(nih));
        nih.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nih.record_type = kBtreeDataTypeDeduplicationEntry;
        nih.level       = phdr.level;
        nih.node_keys   = ir;
        nih.keys_length = (uint16_t)(ir * ie_sz);
        nih.left_link   = parent_lba;
        nih.right_link  = idx_old_right;
        memcpy(buf, &nih, sizeof(nih));
        memcpy(buf + sizeof(nih), &aie[il], (size_t)ir * ie_sz);
        compute_node_checksum(buf);
        rc = nc_block_write(nc, ctx, new_idx_lba, buf, bsz);
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
        hdr->total_nodes++;

        /* Update old right neighbor's left_link */
        if(idx_old_right != 0)
        {
            rc = nc_block_read(nc, ctx, idx_old_right, buf, bsz);
            if(rc == OBMAFS3_OK)
            {
                struct btree_node_header rnh;
                memcpy(&rnh, buf, sizeof(rnh));
                rnh.left_link = new_idx_lba;
                memcpy(buf, &rnh, sizeof(rnh));
                compute_node_checksum(buf);
                nc_block_write(nc, ctx, idx_old_right, buf, bsz);
            }
        }
    }

    /* ---- Create new root ---- */
    uint64_t new_root_lba;
    rc = obmafs3_btree_alloc_node(ctx, hdr, 0, &new_root_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Read old root to get its level */
    rc = nc_block_read(nc, ctx, left_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, bsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeDeduplicationEntry;
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

    rc = nc_block_write(nc, ctx, new_root_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    hdr->root_node_lba = new_root_lba;
    hdr->total_nodes++;
    return OBMAFS3_OK;
}

