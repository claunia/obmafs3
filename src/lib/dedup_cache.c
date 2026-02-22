/*
 * dedup_cache.c — Node cache, key set, pending buffer, and cleanup.
 */
#include "dedup_internal.h"

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/** Compute and store the checksum for a btree node block. */
void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr      = (struct btree_node_header *)buf;
    size_t                    data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

/* ------------------------------------------------------------------ */
/*  In-memory write-back cache for dedup B+Tree nodes                  */
/* ------------------------------------------------------------------ */

/** Allocate and initialise a dedup B+Tree node cache. */
struct dedup_node_cache *dedup_cache_create(size_t block_size)
{
    struct dedup_node_cache *nc = calloc(1, sizeof(*nc));
    if(!nc) return NULL;
    nc->capacity   = DEDUP_CACHE_INIT_CAP;
    nc->block_size = block_size;
    nc->slots      = calloc(nc->capacity, sizeof(struct dedup_cache_slot));
    if(!nc->slots)
    {
        free(nc);
        return NULL;
    }
    nc->dirty_cap  = 256;
    nc->dirty_list = malloc(nc->dirty_cap * sizeof(uint32_t));
    if(!nc->dirty_list)
    {
        free(nc->slots);
        free(nc);
        return NULL;
    }
    nc->dirty_count = 0;
    pthread_mutex_init(&nc->lock, NULL);
    return nc;
}

/** Fibonacci-hashing of an LBA to a table index. */
static uint32_t cache_hash(uint64_t lba, uint32_t mask)
{
    return (uint32_t)((lba * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
}

/** Find the slot for @lba, or NULL if not cached. */
struct dedup_cache_slot *cache_find_slot(struct dedup_node_cache *nc, uint64_t lba)
{
    uint32_t mask = nc->capacity - 1;
    uint32_t idx  = cache_hash(lba, mask);
    for(uint32_t i = 0; i < nc->capacity; i++)
    {
        uint32_t s = (idx + i) & mask;
        if(nc->slots[s].buf == NULL) return NULL; /* end of probe chain */
        if(nc->slots[s].lba == lba) return &nc->slots[s];
    }
    return NULL;
}

/** Append a slot index to the dirty list, growing it if needed. */
static void dirty_list_add(struct dedup_node_cache *nc, uint32_t slot_idx)
{
    if(nc->dirty_count >= nc->dirty_cap)
    {
        uint32_t  new_cap = nc->dirty_cap * 2;
        uint32_t *tmp     = realloc(nc->dirty_list, new_cap * sizeof(uint32_t));
        if(!tmp) return; /* non-fatal: flush will still work, just slower */
        nc->dirty_list = tmp;
        nc->dirty_cap  = new_cap;
    }
    nc->dirty_list[nc->dirty_count++] = slot_idx;
}

/** Double the table capacity and re-hash all entries. */
static int cache_grow(struct dedup_node_cache *nc)
{
    uint32_t                 new_cap = nc->capacity * 2;
    struct dedup_cache_slot *ns      = calloc(new_cap, sizeof(struct dedup_cache_slot));
    if(!ns) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    uint32_t new_mask = new_cap - 1;
    for(uint32_t i = 0; i < nc->capacity; i++)
    {
        if(nc->slots[i].buf == NULL) continue;
        uint32_t idx = cache_hash(nc->slots[i].lba, new_mask);
        for(uint32_t j = 0; j < new_cap; j++)
        {
            uint32_t s = (idx + j) & new_mask;
            if(ns[s].buf == NULL)
            {
                ns[s] = nc->slots[i];
                break;
            }
        }
    }
    free(nc->slots);
    nc->slots    = ns;
    nc->capacity = new_cap;

    /* Rebuild the dirty list — slot indices changed after rehash */
    nc->dirty_count = 0;
    for(uint32_t i = 0; i < new_cap; i++)
    {
        if(ns[i].buf && ns[i].dirty)
            dirty_list_add(nc, i);
    }

    return OBMAFS3_OK;
}

/**
 * Read a tree node through the cache.
 * On a cache miss the block is read from disk and stored in the cache.
 * @buf receives a copy of the cached data.
 */
int dedup_cache_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t bsz)
{
    /* Fast path: cache hit under the cache lock. */
    pthread_mutex_lock(&nc->lock);
    struct dedup_cache_slot *s = cache_find_slot(nc, lba);
    if(s)
    {
        memcpy(buf, s->buf, nc->block_size);
        pthread_mutex_unlock(&nc->lock);
        return OBMAFS3_OK;
    }
    pthread_mutex_unlock(&nc->lock);

    /* Cache miss — read from disk outside the cache lock so
     * concurrent readers can proceed in parallel. */
    int rc = obmafs3_block_read(ctx, lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    /* Re-acquire the lock and insert into the cache.
     * Re-check first: another thread may have inserted this LBA
     * while we were doing disk I/O. */
    pthread_mutex_lock(&nc->lock);
    s = cache_find_slot(nc, lba);
    if(s)
    {
        /* Another thread already cached it — nothing to do. */
        pthread_mutex_unlock(&nc->lock);
        return OBMAFS3_OK;
    }

    /* Grow the table when >= 75 % full */
    if(nc->count * 4 >= nc->capacity * 3)
    {
        rc = cache_grow(nc);
        if(rc != OBMAFS3_OK)
        {
            pthread_mutex_unlock(&nc->lock);
            return OBMAFS3_OK; /* tolerate: data is in buf already */
        }
    }

    uint32_t mask = nc->capacity - 1;
    uint32_t idx  = cache_hash(lba, mask);
    for(uint32_t i = 0; i < nc->capacity; i++)
    {
        uint32_t slot = (idx + i) & mask;
        if(nc->slots[slot].buf == NULL)
        {
            nc->slots[slot].lba = lba;
            nc->slots[slot].buf = malloc(nc->block_size);
            if(nc->slots[slot].buf)
            {
                memcpy(nc->slots[slot].buf, buf, nc->block_size);
                nc->slots[slot].dirty = 0;
                nc->count++;
            }
            break;
        }
    }
    pthread_mutex_unlock(&nc->lock);
    return OBMAFS3_OK;
}

/**
 * Write a tree node through the cache (write-back).
 * The data is stored in the cache and marked dirty; no disk I/O
 * happens until dedup_cache_flush().
 */
int dedup_cache_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, const void *buf,
                             size_t bsz)
{
    (void)ctx;
    (void)bsz; /* used only for fallback / symmetry */

    struct dedup_cache_slot *s = cache_find_slot(nc, lba);
    if(s)
    {
        memcpy(s->buf, buf, nc->block_size);
        if(!s->dirty)
        {
            s->dirty = 1;
            dirty_list_add(nc, (uint32_t)(s - nc->slots));
        }
        return OBMAFS3_OK;
    }

    /* New entry */
    if(nc->count * 4 >= nc->capacity * 3)
    {
        int rc = cache_grow(nc);
        if(rc != OBMAFS3_OK) return rc;
    }

    uint32_t mask = nc->capacity - 1;
    uint32_t idx  = cache_hash(lba, mask);
    for(uint32_t i = 0; i < nc->capacity; i++)
    {
        uint32_t slot = (idx + i) & mask;
        if(nc->slots[slot].buf == NULL)
        {
            nc->slots[slot].lba = lba;
            nc->slots[slot].buf = malloc(nc->block_size);
            if(!nc->slots[slot].buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            memcpy(nc->slots[slot].buf, buf, nc->block_size);
            nc->slots[slot].dirty = 1;
            nc->count++;
            dirty_list_add(nc, slot);
            return OBMAFS3_OK;
        }
    }
    DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); /* table full (shouldn't happen) */
}

/** Flush all dirty entries to disk, clear dirty flags.
 *  Sorts dirty nodes by LBA, then coalesces consecutive LBAs into
 *  single pwritev() calls to minimise syscall overhead and let the
 *  kernel elevator-sort the resulting I/O. */
int dedup_cache_flush(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx)
{
    if(nc->dirty_count == 0) { nc->writes_since_flush = 0; return OBMAFS3_OK; }

    /* Sort dirty list by LBA for sequential disk access (insertion sort). */
    for(uint32_t i = 1; i < nc->dirty_count; i++)
    {
        uint32_t key  = nc->dirty_list[i];
        uint64_t klba = nc->slots[key].lba;
        int32_t  j    = (int32_t)i - 1;
        while(j >= 0 && nc->slots[nc->dirty_list[j]].lba > klba)
        {
            nc->dirty_list[j + 1] = nc->dirty_list[j];
            j--;
        }
        nc->dirty_list[j + 1] = key;
    }

    /* Coalesce runs of consecutive LBAs into pwritev() calls, each
     * capped at DEDUP_NC_IOV_MAX iovecs to stay within the kernel's
     * IOV_MAX limit.  With dedup clump sizes of 1024+ nodes, a
     * single consecutive run can easily exceed IOV_MAX so we split
     * large runs into multiple syscalls. */
    uint32_t iov_cap = nc->dirty_count;
    if(iov_cap > DEDUP_NC_IOV_MAX) iov_cap = DEDUP_NC_IOV_MAX;

    struct iovec *iov = malloc(iov_cap * sizeof(struct iovec));
    if(!iov)
    {
        /* Fallback: write individually if malloc fails. */
        for(uint32_t d = 0; d < nc->dirty_count; d++)
        {
            uint32_t i = nc->dirty_list[d];
            if(nc->slots[i].buf && nc->slots[i].dirty)
            {
                int rc = obmafs3_block_write(ctx, nc->slots[i].lba,
                                             nc->slots[i].buf, nc->block_size);
                if(rc != OBMAFS3_OK) return rc;
                nc->slots[i].dirty = 0;
            }
        }
        nc->dirty_count        = 0;
        nc->writes_since_flush = 0;
        return OBMAFS3_OK;
    }

    uint32_t d = 0;
    while(d < nc->dirty_count)
    {
        /* Skip already-clean slots (shouldn't happen, but be safe). */
        uint32_t si = nc->dirty_list[d];
        if(!nc->slots[si].buf || !nc->slots[si].dirty) { d++; continue; }

        /* Start a new run at this slot's LBA. */
        uint64_t run_start_lba = nc->slots[si].lba;
        uint32_t iov_count     = 0;
        uint64_t expect_lba    = run_start_lba;

        /* Gather consecutive LBAs into the iovec, capped at IOV_MAX. */
        while(d < nc->dirty_count && iov_count < iov_cap)
        {
            uint32_t ci = nc->dirty_list[d];
            if(!nc->slots[ci].buf || !nc->slots[ci].dirty) { d++; continue; }
            if(nc->slots[ci].lba != expect_lba) break;
            iov[iov_count].iov_base = nc->slots[ci].buf;
            iov[iov_count].iov_len  = nc->block_size;
            iov_count++;
            expect_lba++;
            d++;
        }

        if(iov_count == 0) continue;

        /* Issue a single pwritev for this chunk of the run. */
        off_t   off      = (off_t)(run_start_lba * ctx->sb.block_size);
        size_t  expected = nc->block_size * iov_count;
        ssize_t n        = pwritev(ctx->fd, iov, (int)iov_count, off);
        if(n < 0 || (size_t)n != expected)
        {
            /* pwritev failed — fall back to writing each block in
             * this chunk individually so we save as much as possible. */
            fprintf(stderr, "WARNING: pwritev lba=%" PRIu64 " count=%u "
                    "expect=%zu got=%zd (errno=%d %s) — falling back to "
                    "individual writes\n",
                    run_start_lba, iov_count, expected, n,
                    errno, strerror(errno));

            int any_failed = 0;
            for(uint32_t r = 0; r < iov_count; r++)
            {
                uint64_t blk_lba = run_start_lba + r;
                int wrc = obmafs3_block_write(ctx, blk_lba,
                                              iov[r].iov_base, nc->block_size);
                if(wrc == OBMAFS3_OK)
                {
                    struct dedup_cache_slot *s = cache_find_slot(nc, blk_lba);
                    if(s) s->dirty = 0;
                }
                else
                {
                    fprintf(stderr, "ERROR: fallback write lba=%" PRIu64
                            " failed (rc=%d)\n", blk_lba, wrc);
                    any_failed = 1;
                }
            }
            if(any_failed) goto flush_rebuild;
            continue;
        }

        /* Mark all slots in this chunk as clean. */
        for(uint32_t r = 0; r < iov_count; r++)
        {
            uint64_t lba = run_start_lba + r;
            struct dedup_cache_slot *s = cache_find_slot(nc, lba);
            if(s) s->dirty = 0;
        }
    }

    free(iov);
    nc->dirty_count        = 0;
    nc->writes_since_flush = 0;
    return OBMAFS3_OK;

flush_rebuild:
    /* Some individual writes also failed.  Rebuild the dirty list
     * from the slot array so dirty_count accurately reflects only
     * the entries that are still dirty. */
    free(iov);
    nc->dirty_count = 0;
    for(uint32_t i = 0; i < nc->capacity; i++)
    {
        if(nc->slots[i].buf && nc->slots[i].dirty)
            dirty_list_add(nc, i);
    }
    return OBMAFS3_ERR_IO;
}

/** Free all memory held by the node cache. */
void dedup_cache_free(struct dedup_node_cache *nc)
{
    if(!nc) return;
    pthread_mutex_destroy(&nc->lock);
    for(uint32_t i = 0; i < nc->capacity; i++) free(nc->slots[i].buf); /* free(NULL) is safe */
    free(nc->slots);
    free(nc->dirty_list);
    free(nc);
}

/* ------------------------------------------------------------------ */
/*  In-memory hash set of known dedup keys (Bloom-filter replacement)  */
/* ------------------------------------------------------------------ */

/** Allocate a dedup key set. */
struct dedup_key_set *keyset_create(void)
{
    struct dedup_key_set *ks = calloc(1, sizeof(*ks));
    if(!ks) return NULL;
    ks->capacity = KEYSET_INIT_CAP;
    ks->keys     = calloc(ks->capacity, sizeof(uint64_t)); /* 0 = empty */
    if(!ks->keys) { free(ks); return NULL; }
    return ks;
}

/** Fibonacci-hashing of a key to a table index. */
uint32_t keyset_hash(uint64_t key, uint32_t mask)
{
    return (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
}

/** Grow the key set (double capacity, re-insert all entries). */
static int keyset_grow(struct dedup_key_set *ks)
{
    uint32_t  new_cap  = ks->capacity * 2;
    uint64_t *new_keys = calloc(new_cap, sizeof(uint64_t));
    if(!new_keys) return OBMAFS3_ERR_NOMEM;

    uint32_t new_mask = new_cap - 1;
    for(uint32_t i = 0; i < ks->capacity; i++)
    {
        if(ks->keys[i] == KEYSET_EMPTY) continue;
        uint32_t idx = keyset_hash(ks->keys[i], new_mask);
        for(uint32_t j = 0; j < new_cap; j++)
        {
            uint32_t s = (idx + j) & new_mask;
            if(new_keys[s] == KEYSET_EMPTY) { new_keys[s] = ks->keys[i]; break; }
        }
    }
    free(ks->keys);
    ks->keys     = new_keys;
    ks->capacity = new_cap;
    return OBMAFS3_OK;
}

/** Insert a key into the set (no-op if already present). */
void keyset_insert(struct dedup_key_set *ks, uint64_t key)
{
    if(key == KEYSET_EMPTY) return; /* cannot store sentinel */

    /* Grow at 75% load */
    if(ks->count * 4 >= ks->capacity * 3)
    {
        if(keyset_grow(ks) != OBMAFS3_OK) return; /* non-fatal */
    }

    uint32_t mask = ks->capacity - 1;
    uint32_t idx  = keyset_hash(key, mask);
    for(uint32_t i = 0; i < ks->capacity; i++)
    {
        uint32_t s = (idx + i) & mask;
        if(ks->keys[s] == KEYSET_EMPTY) { ks->keys[s] = key; ks->count++; return; }
        if(ks->keys[s] == key) return; /* already present */
    }
}

/** Check if a key exists in the set. */
int keyset_contains(const struct dedup_key_set *ks, uint64_t key)
{
    if(!ks || key == KEYSET_EMPTY) return 0;

    uint32_t mask = ks->capacity - 1;
    uint32_t idx  = keyset_hash(key, mask);
    for(uint32_t i = 0; i < ks->capacity; i++)
    {
        uint32_t s = (idx + i) & mask;
        if(ks->keys[s] == KEYSET_EMPTY) return 0;
        if(ks->keys[s] == key) return 1;
    }
    return 0;
}

/** Free the key set. */
void keyset_free(struct dedup_key_set *ks)
{
    if(!ks) return;
    free(ks->keys);
    free(ks);
}

/**
 * Extract all dedup hash keys from a B+Tree leaf node buffer and
 * insert them into the key set.
 *
 * Called automatically whenever a leaf is read through the cache,
 * so that after one full pass the key set contains every hash in
 * the tree.
 */
void keyset_ingest_leaf(struct dedup_key_set *ks, const void *buf)
{
    if(!ks) return;

    const struct btree_node_header *nhdr = (const struct btree_node_header *)buf;
    if(nhdr->magic != OBMAFS3_BTREE_NODE_MAGIC || nhdr->level != 0) return;

    const uint8_t *data = (const uint8_t *)buf + sizeof(struct btree_node_header);
    for(uint16_t i = 0; i < nhdr->node_keys; i++)
    {
        uint64_t hash;
        memcpy(&hash, data + (size_t)i * sizeof(struct dedup_entry), sizeof(hash));
        keyset_insert(ks, hash);
    }
}

/**
 * Seed the key set from all leaf nodes already present in the node cache.
 *
 * Called once when the key set is first created while the node cache
 * is already warm.  Without this, cached leaves would never be
 * ingested because the prefetch phase skips them (cache_find_slot
 * returns true → nc_block_read is never called → keyset_ingest_leaf
 * is never invoked for those nodes).
 */
void keyset_seed_from_cache(struct dedup_key_set *ks, const struct dedup_node_cache *nc)
{
    if(!ks || !nc) return;
    for(uint32_t i = 0; i < nc->capacity; i++)
    {
        if(nc->slots[i].buf) keyset_ingest_leaf(ks, nc->slots[i].buf);
    }
}

/* ------------------------------------------------------------------ */
/*  Pending insert buffer (deferred B+Tree inserts for keyset misses)  */
/* ------------------------------------------------------------------ */

/** Allocate a pending insert buffer. */
struct dedup_pending_buf *pending_create(void)
{
    struct dedup_pending_buf *pb = calloc(1, sizeof(*pb));
    if(!pb) return NULL;
    pb->capacity = PENDING_INIT_CAP;
    pb->slots    = calloc(pb->capacity, sizeof(struct dedup_entry));
    if(!pb->slots) { free(pb); return NULL; }
    return pb;
}

/** Grow the pending buffer (double capacity, re-insert all entries). */
static int pending_grow(struct dedup_pending_buf *pb)
{
    uint32_t new_cap  = pb->capacity * 2;
    struct dedup_entry *new_slots = calloc(new_cap, sizeof(struct dedup_entry));
    if(!new_slots) return OBMAFS3_ERR_NOMEM;

    uint32_t new_mask = new_cap - 1;
    for(uint32_t i = 0; i < pb->capacity; i++)
    {
        if(pb->slots[i].hash == KEYSET_EMPTY) continue;
        uint32_t idx = keyset_hash(pb->slots[i].hash, new_mask);
        for(uint32_t j = 0; j < new_cap; j++)
        {
            uint32_t s = (idx + j) & new_mask;
            if(new_slots[s].hash == KEYSET_EMPTY) { new_slots[s] = pb->slots[i]; break; }
        }
    }
    free(pb->slots);
    pb->slots    = new_slots;
    pb->capacity = new_cap;
    return OBMAFS3_OK;
}

/** Insert an entry into the pending buffer (no-op if hash already present). */
void pending_insert(struct dedup_pending_buf *pb, const struct dedup_entry *entry)
{
    if(!pb || entry->hash == KEYSET_EMPTY) return;

    /* Grow at 75% load */
    if(pb->count * 4 >= pb->capacity * 3)
    {
        if(pending_grow(pb) != OBMAFS3_OK) return;
    }

    uint32_t mask = pb->capacity - 1;
    uint32_t idx  = keyset_hash(entry->hash, mask);
    for(uint32_t i = 0; i < pb->capacity; i++)
    {
        uint32_t s = (idx + i) & mask;
        if(pb->slots[s].hash == KEYSET_EMPTY) { pb->slots[s] = *entry; pb->count++; return; }
        if(pb->slots[s].hash == entry->hash)   return; /* already present */
    }
}

/** Look up a hash in the pending buffer. Returns entry or NULL. */
const struct dedup_entry *pending_lookup(const struct dedup_pending_buf *pb, uint64_t hash)
{
    if(!pb || hash == KEYSET_EMPTY) return NULL;

    uint32_t mask = pb->capacity - 1;
    uint32_t idx  = keyset_hash(hash, mask);
    for(uint32_t i = 0; i < pb->capacity; i++)
    {
        uint32_t s = (idx + i) & mask;
        if(pb->slots[s].hash == KEYSET_EMPTY) return NULL;
        if(pb->slots[s].hash == hash)         return &pb->slots[s];
    }
    return NULL;
}

/** Free the pending buffer. */
void pending_free(struct dedup_pending_buf *pb)
{
    if(!pb) return;
    free(pb->slots);
    free(pb);
}

/** Comparator for sorting pending entries by hash. */
int pending_entry_cmp(const void *a, const void *b)
{
    const struct dedup_entry *ea = a;
    const struct dedup_entry *eb = b;
    if(ea->hash < eb->hash) return -1;
    if(ea->hash > eb->hash) return  1;
    return 0;
}


/**
 * Flush all pending inserts into the B+Tree using leaf prefetch.
 *
 * Strategy:
 *   1. Extract + sort entries by hash.
 *   2. Find target leaf LBA for each entry via index traversal
 *      (index nodes are cached → in-memory only).
 *   3. Sort + deduplicate leaf LBAs.
 *   4. posix_fadvise(WILLNEED) all unique leaves for sequential
 *      read-ahead on HDD.
 *   5. Pre-read all unique leaves into the node cache.
 *   6. Run all upsert_find + upsert_insert — 100% cache hits.
 *   7. Single dedup_cache_flush + header write at the end.
 *
 * The result: N entries touching K unique leaves generate K
 * sequential disk reads instead of N random seeks.
 *
 * Clears the buffer afterwards.
 * Must be called under tree_lock (or single-threaded context).
 */
int pending_flush(struct dedup_pending_buf *pb, struct obmafs3_ctx *ctx,
                         struct btree_header *dedup_hdr, uint64_t dedup_hdr_lba)
{
    if(!pb || pb->count == 0) return OBMAFS3_OK;

    struct timespec t_start, t_prefetch, t_insert, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* ---- Phase 1: extract + sort entries by hash ---- */

    struct dedup_entry *sorted = malloc((size_t)pb->count * sizeof(struct dedup_entry));
    if(!sorted) return OBMAFS3_ERR_NOMEM;

    uint32_t n = 0;
    for(uint32_t i = 0; i < pb->capacity; i++)
    {
        if(pb->slots[i].hash != KEYSET_EMPTY)
            sorted[n++] = pb->slots[i];
    }
    qsort(sorted, n, sizeof(struct dedup_entry), pending_entry_cmp);

    /* ---- Phase 2: find target leaf LBAs via cached index nodes ---- */

    uint8_t *tree_buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
    size_t bsz = (size_t)ctx->sb.block_size;
    int rc = OBMAFS3_OK;

    uint64_t *leaf_lbas = malloc((size_t)n * sizeof(uint64_t));
    if(!leaf_lbas) { free(sorted); return OBMAFS3_ERR_NOMEM; }

    for(uint32_t i = 0; i < n; i++)
    {
        uint64_t leaf_lba = 0;
        dedup_find_leaf_lba(ctx, dedup_hdr, sorted[i].hash, &leaf_lba, tree_buf, nc);
        leaf_lbas[i] = leaf_lba;
    }

    /* Sort + deduplicate leaf LBAs for sequential I/O. */
    qsort(leaf_lbas, n, sizeof(uint64_t), lba_cmp);

    uint32_t unique_leaves = 0;
    {
        uint64_t prev = 0;
        for(uint32_t i = 0; i < n; i++)
        {
            if(leaf_lbas[i] == 0 || leaf_lbas[i] == prev) continue;
            leaf_lbas[unique_leaves++] = leaf_lbas[i];
            prev = leaf_lbas[i];
        }
    }

    /* ---- Phase 3: posix_fadvise + pre-read all leaves ---- */

    /* Issue WILLNEED for all unique leaves (sorted = sequential). */
    for(uint32_t i = 0; i < unique_leaves; i++)
    {
        if(cache_find_slot(nc, leaf_lbas[i])) continue;
        posix_fadvise(ctx->fd, (off_t)(leaf_lbas[i] * bsz),
                      (off_t)bsz, POSIX_FADV_WILLNEED);
    }

    /* Pre-read all leaves into the node cache (sequential from page cache). */
    uint32_t leaf_reads = 0;
    for(uint32_t i = 0; i < unique_leaves; i++)
    {
        if(cache_find_slot(nc, leaf_lbas[i])) continue;
        nc_block_read(nc, ctx, leaf_lbas[i], tree_buf, bsz);
        leaf_reads++;
    }

    free(leaf_lbas);

    clock_gettime(CLOCK_MONOTONIC, &t_prefetch);

    /* ---- Phase 4: batch insert (all cache hits) ---- */

    for(uint32_t i = 0; i < n; i++)
    {
        struct dedup_upsert_ctx uctx;
        struct dedup_entry      existing;
        rc = dedup_upsert_find(ctx, dedup_hdr, sorted[i].hash, &existing, &uctx, tree_buf, nc);
        if(rc == OBMAFS3_ERR_NOTFOUND)
        {
            rc = dedup_upsert_insert(ctx, dedup_hdr, &sorted[i], &uctx, tree_buf, nc);
            if(rc != OBMAFS3_OK) break;
        }
        else if(rc != OBMAFS3_OK)
        {
            break; /* I/O error */
        }
        /* else: duplicate found — skip */
    }

    free(sorted);

    clock_gettime(CLOCK_MONOTONIC, &t_insert);

    /* ---- Phase 5: single flush of dirty cache nodes ---- */

    /* Only clear the buffer when ALL entries were successfully
     * inserted.  On partial failure, keep the entries so a retry
     * (or pending persistence on unmount) can recover them.
     * Re-inserting already-drained entries is safe — upsert_find
     * will see them as duplicates and skip. */
    if(rc == OBMAFS3_OK)
    {
        memset(pb->slots, 0, (size_t)pb->capacity * sizeof(struct dedup_entry));
        pb->count = 0;
    }

    /* Flush cached tree nodes after the batch insert. */
    if(nc && nc->dirty_count > 0)
    {
        int nc_rc = dedup_cache_flush(nc, ctx);
        if(rc == OBMAFS3_OK) rc = nc_rc;
    }

    /* Write tree header. */
    if(rc == OBMAFS3_OK)
    {
        int hrc = obmafs3_btree_header_write(ctx, dedup_hdr_lba, dedup_hdr);
        if(hrc != OBMAFS3_OK) rc = hrc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    fprintf(stderr,
            "[dedup-pending] flushed %u entries (%u unique leaves, %u reads) "
            "prefetch=%.1fms insert=%.1fms flush=%.1fms TOTAL=%.1fms\n",
            n, unique_leaves, leaf_reads,
            timespec_diff_ms(&t_start, &t_prefetch),
            timespec_diff_ms(&t_prefetch, &t_insert),
            timespec_diff_ms(&t_insert, &t_end),
            timespec_diff_ms(&t_start, &t_end));

    return rc;
}


/* ------------------------------------------------------------------ */
/*  Dedup block cache flush / free                                     */
/* ------------------------------------------------------------------ */

/**
 * Flush a dirty dedup block cache to disk.
 *
 * Waits for any pending background compression, writes the partially
 * filled dedup block, flushes cached B+Tree nodes, and updates the
 * dedup tree header with the current partial-block state.
 *
 * @param ctx          Filesystem context.
 * @param sector_size  Sector size used to select the correct dedup tree.
 * @param db_cache     Dedup block cache to flush.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_flush_dedup_block_cache(struct obmafs3_ctx *ctx, uint16_t sector_size, struct dedup_block_cache *db_cache)
{
    if(!db_cache || !db_cache->initialized) return OBMAFS3_OK;

    /* Build a temporary dedup_block_ctx from the cache */
    struct dedup_block_ctx db;
    db.data       = db_cache->data;
    db.block_lba  = db_cache->block_lba;
    db.offset     = db_cache->offset;
    db.capacity   = db_cache->capacity;
    db.std_blocks = db_cache->std_blocks;
    db.dirty      = db_cache->dirty;

    int rc = OBMAFS3_OK;

    /* Wait for any pending background compression first */
    if(db_cache->pending_job)
    {
        int bg_rc = dedup_bg_wait(ctx, &db_cache->pending_job);
        if(bg_rc != OBMAFS3_OK) rc = bg_rc;
    }

    if(db.dirty)
    {
        rc = dedup_block_flush(ctx, &db);
        if(rc != OBMAFS3_OK)
        {
            db_cache->dirty = db.dirty;
            return rc;
        }
    }

    /* Flush any cached tree nodes to disk */
    if(ctx->dedup_node_cache)
    {
        int nc_rc = dedup_cache_flush((struct dedup_node_cache *)ctx->dedup_node_cache, ctx);
        if(rc == OBMAFS3_OK) rc = nc_rc;
    }

    /* Update the tree header with the current partial block state.
     * Re-read from disk to pick up any changes the housekeeping
     * thread may have made (root_node_lba, free_node_lba, etc.).
     * Only overlay last_block_lba/offset onto the fresh copy. */
    struct btree_header dedup_hdr;
    uint64_t            dedup_hdr_lba;
    int                 hrc;
    if(db_cache->hdr_cached)
        dedup_hdr_lba = db_cache->dedup_hdr_lba;
    else
    {
        /* Need the LBA — look it up once. */
        struct btree_header tmp;
        hrc = obmafs3_dedup_get_tree(ctx, sector_size, &tmp, &dedup_hdr_lba);
        if(hrc != OBMAFS3_OK) goto flush_done;
    }
    hrc = obmafs3_btree_header_read(ctx, dedup_hdr_lba, &dedup_hdr);
    if(hrc == OBMAFS3_OK)
    {
        dedup_hdr.last_block_lba    = db.block_lba;
        dedup_hdr.last_block_offset = db.offset;
        obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);
        /* Update the cached header too */
        db_cache->dedup_hdr     = dedup_hdr;
        db_cache->dedup_hdr_lba = dedup_hdr_lba;
        db_cache->hdr_cached    = 1;
    }

flush_done:
    /* Copy state back */
    db_cache->block_lba = db.block_lba;
    db_cache->offset    = db.offset;
    db_cache->dirty     = db.dirty;

    return rc;
}

/**
 * Free all resources held by a dedup block cache.
 *
 * Waits for any pending pool compression, releases the B+Tree node
 * cache, and frees the data buffer.
 *
 * @param ctx       Filesystem context (for deferred block freeing).
 * @param db_cache  Dedup block cache to free.
 */
void obmafs3_free_dedup_block_cache(struct obmafs3_ctx *ctx, struct dedup_block_cache *db_cache)
{
    if(!db_cache) return;
    /* Wait for any pending pool job */
    if(db_cache->pending_job) dedup_bg_wait(ctx, &db_cache->pending_job);
    free(db_cache->data);
    db_cache->data        = NULL;
    db_cache->initialized = 0;
    db_cache->hdr_cached  = 0;
}

/**
 * Free the global dedup B+Tree node cache stored in the filesystem context.
 * Called during unmount (obmafs3_close) to release memory.
 */
void obmafs3_dedup_node_cache_free(struct obmafs3_ctx *ctx)
{
    if(!ctx || !ctx->dedup_node_cache) return;
    struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
    /* Flush any dirty entries before releasing the cache. */
    int flush_rc = dedup_cache_flush(nc, ctx);
    if(flush_rc != OBMAFS3_OK)
        fprintf(stderr, "WARNING: dedup node cache flush failed at unmount "
                "(rc=%d) — %u dirty entries lost\n",
                flush_rc, nc->dirty_count);
    dedup_cache_free(nc);
    ctx->dedup_node_cache = NULL;
}

/**
 * Free the global dedup key set stored in the filesystem context.
 * Called during unmount (obmafs3_close) to release memory.
 */
void obmafs3_dedup_key_set_free(struct obmafs3_ctx *ctx)
{
    if(!ctx || !ctx->dedup_key_set) return;
    keyset_free((struct dedup_key_set *)ctx->dedup_key_set);
    ctx->dedup_key_set = NULL;
}

/**
 * Persist any pending deferred inserts to disk and free the buffers.
 *
 * Saves both the active pending buffer and any draining buffer to a
 * contiguous on-disk extent (milliseconds), then frees the in-memory
 * buffers.  The entries will be loaded and drained into the B+Tree
 * by the housekeeping thread on the next mount.
 */
void obmafs3_dedup_pending_flush_and_free(struct obmafs3_ctx *ctx)
{
    if(!ctx) return;

    const struct dedup_pending_buf *pb1 =
        (const struct dedup_pending_buf *)ctx->dedup_pending;
    const struct dedup_pending_buf *pb2 =
        (const struct dedup_pending_buf *)ctx->dedup_pending_draining;

    uint32_t total = 0;
    if(pb1) total += pb1->count;
    if(pb2) total += pb2->count;

    if(total > 0 && ctx->bitmap && ctx->fd >= 0)
    {
        int rc = obmafs3_dedup_pending_save(ctx);
        if(rc != OBMAFS3_OK)
            fprintf(stderr, "[dedup-pending] save failed (rc=%d) — %u entries lost\n",
                    rc, total);
    }
    else if(total > 0)
    {
        fprintf(stderr, "[dedup-pending] %u entries lost (no bitmap or fd)\n", total);
    }

    /* Free both buffers. */
    if(ctx->dedup_pending)
    {
        pending_free((struct dedup_pending_buf *)ctx->dedup_pending);
        ctx->dedup_pending = NULL;
    }
    if(ctx->dedup_pending_draining)
    {
        pending_free((struct dedup_pending_buf *)ctx->dedup_pending_draining);
        ctx->dedup_pending_draining = NULL;
    }
}

