/*
 * dedup.c - OBMAFS3 deduplication operations
 *
 * Manages the deduplication tree list, per-sector-size dedup trees,
 * dedup data block accumulation, and the media image write path that
 * splits incoming data into sectors for deduplication.
 */
#include "obmafs.h"
#include "debug.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <time.h>
#include <zstd.h>

/* ---- Timing instrumentation ---- */
static inline double timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

/* ------------------------------------------------------------------ */
/*  Background compression via the shared pool                         */
/* ------------------------------------------------------------------ */

/**
 * Argument block for a dedup compression async job.
 * Allocated by dedup_bg_submit(), freed by dedup_bg_wait().
 */
struct dedup_compress_arg
{
    struct obmafs3_ctx *ctx;
    uint8_t  *data;        /**< Buffer to compress and write (owned) */
    uint64_t  block_lba;   /**< Destination LBA */
    uint64_t  offset;      /**< Byte offset (end of payload) */
    uint64_t  capacity;    /**< Full dedup block size */
    uint64_t  free_lba;    /**< Trailing blocks to free (set by worker) */
    uint64_t  free_count;  /**< Number of trailing blocks (set by worker) */
};

/**
 * Compress and write a full dedup data block to disk.
 * Called by the background worker thread (or synchronously as a helper).
 * The caller retains ownership of @data — it is NOT freed here.
 *
 * When @out_free_lba and @out_free_count are non-NULL, the function
 * records the trailing unused standard blocks that can be freed.
 * The caller is responsible for actually freeing them (on the main
 * thread) to avoid a data race on the shared allocation bitmap.
 */
static int bg_do_compress_and_write(struct obmafs3_ctx *ctx, ZSTD_CCtx *cctx, uint8_t *data, uint64_t block_lba,
                                    uint64_t offset, uint64_t capacity, uint64_t *out_free_lba,
                                    uint64_t *out_free_count)
{
    struct block_header bhdr;
    memset(&bhdr, 0, sizeof(bhdr));
    bhdr.magic         = OBMAFS3_BLOCK_MAGIC;
    bhdr.original_size = offset - sizeof(struct block_header);

    uint8_t *disk_buf   = NULL;
    int      compressed = 0;

    uint64_t write_size = offset; /* default: uncompressed payload end */

    if(ctx->compression && bhdr.original_size > 0)
    {
        size_t   comp_bound = ZSTD_compressBound((size_t)bhdr.original_size);
        uint8_t *comp_buf   = malloc(comp_bound);
        if(comp_buf)
        {
            size_t comp_size = comp_bound;
            int    crc       = obmafs3_compress(cctx, data + sizeof(bhdr), (size_t)bhdr.original_size,
                                                comp_buf, &comp_size, ctx->zstd_level);
            if(crc == OBMAFS3_OK && comp_size < bhdr.original_size)
            {
                bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                bhdr.compression_type = kCompressionZstd;
                bhdr.compressed_size  = comp_size;
                obmafs3_checksum_block(comp_buf, comp_size, bhdr.checksum);
                write_size          = sizeof(bhdr) + comp_size;
                uint64_t bs         = ctx->sb.block_size;
                uint64_t alloc_size = ((write_size + bs - 1) / bs) * bs;
                disk_buf            = calloc(1, (size_t)alloc_size);
                if(disk_buf)
                {
                    memcpy(disk_buf, &bhdr, sizeof(bhdr));
                    memcpy(disk_buf + sizeof(bhdr), comp_buf, comp_size);
                    compressed = 1;
                }
            }
            free(comp_buf);
        }
    }

    if(!compressed)
    {
        bhdr.flags           = 0;
        bhdr.compressed_size = bhdr.original_size;
        obmafs3_checksum_block(data + sizeof(bhdr), (size_t)bhdr.original_size, bhdr.checksum);
        memcpy(data, &bhdr, sizeof(bhdr));
        write_size = offset;
    }

    /* Write only the needed standard blocks */
    uint64_t bs         = ctx->sb.block_size;
    uint64_t needed_std = (write_size + bs - 1) / bs;
    uint64_t total_std  = capacity / bs;

    const void *write_src = compressed ? disk_buf : data;
    int         rc        = obmafs3_block_write(ctx, block_lba, write_src, (size_t)(needed_std * bs));
    free(disk_buf);

    /* Report trailing unused blocks to the caller for deferred freeing */
    if(out_free_lba && out_free_count)
    {
        if(rc == OBMAFS3_OK && needed_std < total_std)
        {
            *out_free_lba   = block_lba + needed_std;
            *out_free_count = total_std - needed_std;
        }
        else
        {
            *out_free_lba   = 0;
            *out_free_count = 0;
        }
    }

    return rc;
}

/**
 * Async pool callback — compresses a dedup block and writes it to disk.
 * Called by a pool worker thread with its persistent ZSTD context.
 * Ownership of the data buffer is taken: freed after compression.
 */
static int dedup_async_compress_fn(void *arg, void *cctx)
{
    struct dedup_compress_arg *da = (struct dedup_compress_arg *)arg;
    int rc = bg_do_compress_and_write(da->ctx, cctx, da->data, da->block_lba, da->offset, da->capacity, &da->free_lba,
                                      &da->free_count);
    free(da->data);
    da->data = NULL;
    return rc;
}

/**
 * Wait for a pending pool async dedup job and perform deferred cleanup.
 *
 * After the worker completes, any trailing blocks that the worker
 * determined can be freed are released here on the calling (main)
 * thread, avoiding a data race on the shared allocation bitmap.
 *
 * @param ctx     Filesystem context (for block freeing).
 * @param pjob    Pointer to the pending job slot (set to NULL on return).
 * @return        Result from the compression job.
 */
static int dedup_bg_wait(struct obmafs3_ctx *ctx, void **pjob)
{
    struct pool_async_job *job = (struct pool_async_job *)*pjob;
    if(!job) return OBMAFS3_OK;

    int result = obmafs3_pool_wait_async(job);

    struct dedup_compress_arg *da = (struct dedup_compress_arg *)job->arg;
    /* Free trailing unused blocks on the main thread (bitmap is not
     * thread-safe, so this must not happen in the pool worker). */
    if(da->free_count > 0) obmafs3_free_blocks(ctx, da->free_lba, da->free_count);

    free(da);
    obmafs3_pool_async_job_free(job);
    *pjob = NULL;
    return result;
}

/**
 * Submit a buffer for background compression + write via the pool.
 *
 * Ownership of @data transfers to the pool worker — the caller must
 * NOT free or reuse the buffer after this call.
 *
 * @param pool       Compression pool (must not be NULL).
 * @param pjob       Pointer to pending job slot (receives new job).
 * @param ctx        Filesystem context.
 * @param data       Buffer to compress and write (ownership transferred).
 * @param block_lba  Destination LBA.
 * @param offset     Byte offset (end of payload).
 * @param capacity   Full dedup block size.
 * @return OBMAFS3_OK on success, or error code on allocation failure.
 */
static int dedup_bg_submit(struct compress_pool *pool, void **pjob, struct obmafs3_ctx *ctx, uint8_t *data,
                           uint64_t block_lba, uint64_t offset, uint64_t capacity)
{
    struct dedup_compress_arg *da = calloc(1, sizeof(*da));
    if(!da) return OBMAFS3_ERR_NOMEM;
    da->ctx       = ctx;
    da->data      = data;
    da->block_lba = block_lba;
    da->offset    = offset;
    da->capacity  = capacity;

    struct pool_async_job *job = obmafs3_pool_async_job_create(dedup_async_compress_fn, da);
    if(!job)
    {
        free(da->data);  /* da took ownership of the data buffer */
        free(da);
        return OBMAFS3_ERR_NOMEM;
    }

    obmafs3_pool_submit_async(pool, job);
    *pjob = job;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/** Compute and store the checksum for a btree node block. */
static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr      = (struct btree_node_header *)buf;
    size_t                    data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

/* ------------------------------------------------------------------ */
/*  In-memory write-back cache for dedup B+Tree nodes                  */
/* ------------------------------------------------------------------ */

/**
 * Slot in the open-addressing hash table.
 * A slot is empty when buf == NULL.
 */
struct dedup_cache_slot
{
    uint64_t lba;   /**< Block LBA */
    uint8_t *buf;   /**< Cached block data (block_size bytes), NULL = empty */
    int      dirty; /**< Needs write-back to disk */
};

/**
 * Write-back cache for dedup B+Tree nodes.
 *
 * Keeps recently-accessed tree nodes in RAM so that the root and
 * index nodes (accessed on every lookup / insert) are read from disk
 * only once.  Dirty entries are flushed before the btree header is
 * written, preserving on-disk consistency.
 */
struct dedup_node_cache
{
    struct dedup_cache_slot *slots;
    uint32_t                 capacity;    /**< Always a power of 2 */
    uint32_t                 count;       /**< Number of occupied slots */
    size_t                   block_size;
    uint32_t                *dirty_list;  /**< Indices of dirty slots */
    uint32_t                 dirty_count; /**< Number of entries in dirty_list */
    uint32_t                 dirty_cap;   /**< Allocated capacity of dirty_list */
};

#define DEDUP_CACHE_INIT_CAP 2048 /* power of 2 */

/** Allocate and initialise a dedup B+Tree node cache. */
static struct dedup_node_cache *dedup_cache_create(size_t block_size)
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
    return nc;
}

/** Fibonacci-hashing of an LBA to a table index. */
static uint32_t cache_hash(uint64_t lba, uint32_t mask)
{
    return (uint32_t)((lba * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
}

/** Find the slot for @lba, or NULL if not cached. */
static struct dedup_cache_slot *cache_find_slot(struct dedup_node_cache *nc, uint64_t lba)
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
static int dedup_cache_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t bsz)
{
    struct dedup_cache_slot *s = cache_find_slot(nc, lba);
    if(s)
    {
        memcpy(buf, s->buf, nc->block_size);
        return OBMAFS3_OK;
    }

    /* Cache miss — read from disk */
    int rc = obmafs3_block_read(ctx, lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    /* Grow the table when >= 75 % full */
    if(nc->count * 4 >= nc->capacity * 3)
    {
        rc = cache_grow(nc);
        if(rc != OBMAFS3_OK) return OBMAFS3_OK; /* tolerate: data is in buf already */
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
    return OBMAFS3_OK;
}

/**
 * Write a tree node through the cache (write-back).
 * The data is stored in the cache and marked dirty; no disk I/O
 * happens until dedup_cache_flush().
 */
static int dedup_cache_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, const void *buf,
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

/** Flush all dirty entries to disk, clear dirty flags. */
static int dedup_cache_flush(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx)
{
    for(uint32_t d = 0; d < nc->dirty_count; d++)
    {
        uint32_t i = nc->dirty_list[d];
        if(nc->slots[i].buf && nc->slots[i].dirty)
        {
            int rc = obmafs3_block_write(ctx, nc->slots[i].lba, nc->slots[i].buf, nc->block_size);
            if(rc != OBMAFS3_OK) return rc;
            nc->slots[i].dirty = 0;
        }
    }
    nc->dirty_count = 0;
    return OBMAFS3_OK;
}

/** Free all memory held by the node cache. */
static void dedup_cache_free(struct dedup_node_cache *nc)
{
    if(!nc) return;
    for(uint32_t i = 0; i < nc->capacity; i++) free(nc->slots[i].buf); /* free(NULL) is safe */
    free(nc->slots);
    free(nc->dirty_list);
    free(nc);
}

/* ------------------------------------------------------------------ */
/*  In-memory hash set of known dedup keys (Bloom-filter replacement)  */
/* ------------------------------------------------------------------ */

/**
 * Open-addressing hash set storing dedup hash keys (uint64_t).
 *
 * When all leaf nodes have been seen at least once, the set contains
 * every key in the dedup B+Tree.  A lookup in this set answers "does
 * this hash exist?" in O(1) without any disk I/O.
 *
 * For 183k keys at 8 bytes each, the table uses ~3 MiB of RAM.
 */
struct dedup_key_set
{
    uint64_t *keys;      /**< Slot array — 0 means empty */
    uint32_t  capacity;  /**< Always a power of 2 */
    uint32_t  count;     /**< Number of occupied slots */
};

/** Sentinel: hash value 0 is reserved as "empty slot". */
#define KEYSET_EMPTY 0ULL

#define KEYSET_INIT_CAP 4096 /* power of 2 */

/** Allocate a dedup key set. */
static struct dedup_key_set *keyset_create(void)
{
    struct dedup_key_set *ks = calloc(1, sizeof(*ks));
    if(!ks) return NULL;
    ks->capacity = KEYSET_INIT_CAP;
    ks->keys     = calloc(ks->capacity, sizeof(uint64_t)); /* 0 = empty */
    if(!ks->keys) { free(ks); return NULL; }
    return ks;
}

/** Fibonacci-hashing of a key to a table index. */
static uint32_t keyset_hash(uint64_t key, uint32_t mask)
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
static void keyset_insert(struct dedup_key_set *ks, uint64_t key)
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
static int keyset_contains(const struct dedup_key_set *ks, uint64_t key)
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
static void keyset_free(struct dedup_key_set *ks)
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
static void keyset_ingest_leaf(struct dedup_key_set *ks, const void *buf)
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
static void keyset_seed_from_cache(struct dedup_key_set *ks, const struct dedup_node_cache *nc)
{
    if(!ks || !nc) return;
    for(uint32_t i = 0; i < nc->capacity; i++)
    {
        if(nc->slots[i].buf) keyset_ingest_leaf(ks, nc->slots[i].buf);
    }
}

/* ------------------------------------------------------------------ */
/*  One-time full leaf scan to warm the key set                        */
/* ------------------------------------------------------------------ */

/* Forward declaration — defined later alongside tree traversal helpers. */
static int lba_cmp(const void *a, const void *b);
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
static int collect_all_leaf_lbas(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
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
static void keyset_warmup(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
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
static int nc_block_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t bsz)
{
    int rc;
    if(nc) rc = dedup_cache_read(nc, ctx, lba, buf, bsz);
    else   rc = obmafs3_block_read(ctx, lba, buf, bsz);

    if(rc == OBMAFS3_OK)
        keyset_ingest_leaf((struct dedup_key_set *)ctx->dedup_key_set, buf);

    return rc;
}

/** Cache-aware block write: delegates to the node cache or a direct write. */
static int nc_block_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, const void *buf,
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
static int dedup_tree_list_read(struct obmafs3_ctx *ctx, struct tree_list_header *hdr, struct tree_list_entry **entries,
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
static int dedup_tree_list_write(struct obmafs3_ctx *ctx, struct tree_list_entry *entries, uint64_t count)
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
 * Look up a hash in the given dedup tree.
 * Returns OBMAFS3_OK if found, OBMAFS3_ERR_NOTFOUND if not.
 *
 * B+Tree traversal: descend through index nodes to the correct leaf,
 * then binary-search among sorted dedup_entry records.
 */
int obmafs3_dedup_lookup(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash,
                         struct dedup_entry *entry)
{
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    uint64_t lba = hdr->root_node_lba;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

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
                return OBMAFS3_OK;
            }
            if(mid_hash < hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }

        return OBMAFS3_ERR_NOTFOUND;
    }
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

#define DEDUP_BTREE_MAX_DEPTH 8

struct dedup_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/**
 * Saved traversal state from dedup_upsert_find(), used by
 * dedup_upsert_insert() to insert without re-traversing.
 */
struct dedup_upsert_ctx
{
    struct dedup_btree_path  path[DEDUP_BTREE_MAX_DEPTH];
    int                      depth;
    uint64_t                 leaf_lba;
    int                      insert_pos;
    struct btree_node_header leaf_hdr;
};

/* ---- Comparison for sorting LBAs (used by leaf prefetch) ---- */
static int lba_cmp(const void *a, const void *b)
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
static int dedup_find_leaf_lba(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
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
static int dedup_upsert_find(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash,
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
static int dedup_upsert_insert(struct obmafs3_ctx *ctx, struct btree_header *hdr, const struct dedup_entry *entry,
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
        rc = obmafs3_alloc_block(ctx, &new_idx_lba);
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
    rc = obmafs3_alloc_block(ctx, &new_root_lba);
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

/* ------------------------------------------------------------------ */
/*  Dedup data block management                                        */
/* ------------------------------------------------------------------ */

/**
 * Context for in-memory dedup data block accumulation.
 * Loaded from the tree header's last_block_lba/last_block_offset,
 * and flushed back to disk when full or at the end of processing.
 */
struct dedup_block_ctx
{
    uint8_t *data;       /**< In-memory dedup data block buffer */
    uint64_t block_lba;  /**< LBA of this dedup block (0 = not yet allocated) */
    uint64_t offset;     /**< Current byte offset for next sector write */
    uint64_t capacity;   /**< Total capacity in bytes (dedup_block_size) */
    uint64_t std_blocks; /**< Number of standard blocks this dedup block spans */
    int      dirty;      /**< Whether the buffer has been modified */
};

/**
 * Initialize the in-memory dedup block from the tree header.
 * If last_block_lba != 0, reads the partial block from disk.
 * Otherwise sets up for a fresh allocation.
 */
static int dedup_block_init(struct obmafs3_ctx *ctx, const struct btree_header *hdr, struct dedup_block_ctx *db)
{
    db->capacity   = ctx->sb.dedup_block_size;
    db->std_blocks = db->capacity / ctx->sb.block_size;
    db->dirty      = 0;

    db->data = calloc(1, (size_t)db->capacity);
    if(!db->data) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    if(hdr->last_block_lba != 0)
    {
        /* Read the existing partial block from disk */
        db->block_lba = hdr->last_block_lba;
        db->offset    = hdr->last_block_offset;
        int rc        = obmafs3_block_read(ctx, db->block_lba, db->data, (size_t)db->capacity);
        if(rc != OBMAFS3_OK)
        {
            free(db->data);
            db->data = NULL;
            return rc;
        }

        /* If the block was compressed on a previous flush, decompress
         * it back into raw form so we can continue appending data at
         * db->offset.  The dedup entries already stored reference
         * uncompressed offsets, so this preserves correctness. */
        struct block_header bhdr;
        memcpy(&bhdr, db->data, sizeof(bhdr));
        if((bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) && bhdr.original_size > 0)
        {
            uint8_t *temp = malloc((size_t)bhdr.original_size);
            if(!temp)
            {
                free(db->data);
                db->data = NULL;
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }
            rc = obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, db->data + sizeof(bhdr), (size_t)bhdr.compressed_size, temp,
                                    (size_t)bhdr.original_size);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(db->data);
                db->data = NULL;
                return rc;
            }
            /* Rebuild uncompressed layout: [header][raw payload] */
            memset(db->data + sizeof(bhdr), 0, (size_t)db->capacity - sizeof(bhdr));
            memcpy(db->data + sizeof(bhdr), temp, (size_t)bhdr.original_size);
            free(temp);
        }
    }
    else
    {
        db->block_lba = 0;
        db->offset    = 0;
    }

    return OBMAFS3_OK;
}

/**
 * Finalize and flush the in-memory dedup block to disk.
 * Computes the block_header checksum and writes it.
 */
static int dedup_block_flush(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db)
{
    if(!db->dirty || db->block_lba == 0) return OBMAFS3_OK;

    /* Write the block header */
    struct block_header bhdr;
    memset(&bhdr, 0, sizeof(bhdr));
    bhdr.magic         = OBMAFS3_BLOCK_MAGIC;
    bhdr.original_size = db->offset - sizeof(struct block_header);

    /* Try ZSTD compression.
     *
     * IMPORTANT: We must NOT overwrite db->data with the compressed
     * payload.  db->data holds the uncompressed accumulator and may
     * still be appended to if this is a partial (not-yet-full) block.
     * Instead, build the on-disk image in a separate buffer.  */
    uint8_t *disk_buf   = NULL;
    int      compressed = 0;

    uint64_t write_size = db->offset; /* default: uncompressed payload end */

    if(ctx->compression && bhdr.original_size > 0)
    {
        size_t   comp_bound = ZSTD_compressBound((size_t)bhdr.original_size);
        uint8_t *comp_buf   = malloc(comp_bound);
        if(comp_buf)
        {
            size_t comp_size = comp_bound;
            int    crc = obmafs3_compress(obmafs3_get_thread_bufs(ctx)->zstd_cctx, db->data + sizeof(bhdr), (size_t)bhdr.original_size,
                                          comp_buf, &comp_size, ctx->zstd_level);
            if(crc == OBMAFS3_OK && comp_size < bhdr.original_size)
            {
                bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                bhdr.compression_type = kCompressionZstd;
                bhdr.compressed_size  = comp_size;
                obmafs3_checksum_block(comp_buf, comp_size, bhdr.checksum);

                write_size          = sizeof(bhdr) + comp_size;
                uint64_t bs         = ctx->sb.block_size;
                uint64_t alloc_size = ((write_size + bs - 1) / bs) * bs;
                disk_buf            = calloc(1, (size_t)alloc_size);
                if(disk_buf)
                {
                    memcpy(disk_buf, &bhdr, sizeof(bhdr));
                    memcpy(disk_buf + sizeof(bhdr), comp_buf, comp_size);
                    compressed = 1;
                }
            }
            free(comp_buf);
        }
    }

    if(!compressed)
    {
        bhdr.flags           = 0;
        bhdr.compressed_size = bhdr.original_size;
        obmafs3_checksum_block(db->data + sizeof(bhdr), (size_t)bhdr.original_size, bhdr.checksum);
        memcpy(db->data, &bhdr, sizeof(bhdr));
        write_size = db->offset;
    }

    /* Write only the needed standard blocks.
     * Do NOT free trailing blocks here — the partial block may be
     * resumed on next mount via dedup_block_init, which requires
     * all std_per_dedup blocks to remain allocated contiguously. */
    uint64_t bs         = ctx->sb.block_size;
    uint64_t needed_std = (write_size + bs - 1) / bs;

    const void *write_src = compressed ? disk_buf : db->data;
    int         rc        = obmafs3_block_write(ctx, db->block_lba, write_src, (size_t)(needed_std * bs));
    free(disk_buf);
    if(rc != OBMAFS3_OK) return rc;

    db->dirty = 0;
    return OBMAFS3_OK;
}

/**
 * Allocate a new dedup data block and prepare it for writing.
 * If there was a previous block, it is flushed first.
 */
static int dedup_block_new(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db)
{
    /* Flush any existing block */
    if(db->dirty)
    {
        int rc = dedup_block_flush(ctx, db);
        if(rc != OBMAFS3_OK) return rc;
    }

    /* Allocate contiguous standard blocks for the dedup block */
    uint64_t start_lba;
    int      rc = obmafs3_alloc_blocks(ctx, db->std_blocks, &start_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Reset the buffer */
    memset(db->data, 0, (size_t)db->capacity);
    db->block_lba = start_lba;
    db->offset    = sizeof(struct block_header);
    db->dirty     = 0;

    return OBMAFS3_OK;
}

/**
 * Store a sector into the dedup data block.
 * Allocates a new block if the current one is full or doesn't exist.
 * Returns the block_lba and block_offset where the sector was stored.
 *
 * When @pool is non-NULL and the current block is full, the buffer is
 * submitted to the compression pool for background processing while a
 * fresh buffer is allocated immediately for continued accumulation.
 *
 * @param pending_job  Pointer to the pending async job slot (in db_cache).
 */
static int dedup_block_store(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db, const void *sector_data,
                             size_t sector_len, uint64_t *out_lba, uint64_t *out_offset, struct compress_pool *pool,
                             void **pending_job)
{
    int rc;

    /* Need a new block? */
    if(db->block_lba == 0)
    {
        rc = dedup_block_new(ctx, db);
        if(rc != OBMAFS3_OK) return rc;
    }
    else if(db->offset + sector_len > db->capacity)
    {
        /* Current block is full */
        if(pool)
        {
            /* Wait for any previous background job */
            rc = dedup_bg_wait(ctx, pending_job);
            if(rc != OBMAFS3_OK) return rc;
            /* Submit the current buffer to the pool for compression.
             * Ownership of db->data transfers to the pool worker. */
            rc = dedup_bg_submit(pool, pending_job, ctx, db->data, db->block_lba, db->offset, db->capacity);
            if(rc != OBMAFS3_OK) return rc;
            /* Allocate a fresh buffer and new LBA */
            db->data = calloc(1, (size_t)db->capacity);
            if(!db->data)
            {
                /* Mark block as clean so the error recovery path doesn't
                 * try to flush through a NULL data pointer. */
                db->dirty     = 0;
                db->block_lba = 0;
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }
            db->dirty = 0;
            uint64_t start_lba;
            rc = obmafs3_alloc_blocks(ctx, db->std_blocks, &start_lba);
            if(rc != OBMAFS3_OK)
            {
                free(db->data);
                db->data      = NULL;
                db->dirty     = 0;
                db->block_lba = 0;
                return rc;
            }
            db->block_lba = start_lba;
            db->offset    = sizeof(struct block_header);
        }
        else
        {
            /* Synchronous path */
            rc = dedup_block_flush(ctx, db);
            if(rc != OBMAFS3_OK) return rc;
            rc = dedup_block_new(ctx, db);
            if(rc != OBMAFS3_OK) return rc;
        }
    }

    /* Append sector data at current offset */
    memcpy(db->data + db->offset, sector_data, sector_len);
    *out_lba    = db->block_lba;
    *out_offset = db->offset;
    db->offset += sector_len;
    db->dirty = 1;

    return OBMAFS3_OK;
}

/** Free the data buffer of a dedup block context. */
static void dedup_block_free(struct dedup_block_ctx *db)
{
    free(db->data);
    db->data = NULL;
}

/* ------------------------------------------------------------------ */
/*  Sector map entry writing                                           */
/* ------------------------------------------------------------------ */

/**
 * Write a batch of sector_map_entries to the file's data blocks.
 * Uses the inode's extents and sector_map_size to determine position.
 * Allocates new blocks as needed.
 *
 * Writing all entries in one call avoids interleaving block allocations
 * with dedup tree node allocations, preventing inode extent fragmentation.
 */
static int write_sector_map_batch(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                  const struct sector_map_entry *entries, uint64_t count)
{
    if(count == 0) return OBMAFS3_OK;

    size_t   entry_size  = sizeof(struct sector_map_entry);
    uint64_t map_offset  = inode->sector_map_size * entry_size;
    size_t   total_bytes = (size_t)(count * entry_size);

    /*
     * Temporarily set file_size to the current sector map byte size
     * so block allocation is computed correctly for the extent-based
     * storage of sector_map data.
     */
    uint64_t saved_file_size = inode->file_size;
    inode->file_size         = map_offset;

    int rc = obmafs3_write_file_data(ctx, inode, map_offset, entries, total_bytes);

    /* Restore the logical image size */
    inode->file_size = saved_file_size;

    if(rc == OBMAFS3_OK) inode->sector_map_size += count;

    return rc;
}

/* ------------------------------------------------------------------ */
/*  Media image write path                                             */
/* ------------------------------------------------------------------ */

/**
 * Write data to a media image file with sector-level deduplication.
 *
 * Incoming data is split into sectors of @sector_size bytes.  Each
 * sector is hashed and looked up in the dedup tree for that sector
 * size.  New sectors are stored in dedup data blocks; duplicate
 * sectors are discarded.  A sector_map_entry is appended for every
 * sector regardless.
 *
 * If @cache is non-NULL, sector_map_entries are accumulated in the
 * cache instead of being written to disk.  Call
 * obmafs3_flush_sector_map_cache() to write them out.
 *
 * If @db_cache is non-NULL, the dedup data block accumulator is kept
 * alive across calls instead of being re-read from disk each time.
 * The caller must call obmafs3_flush_dedup_block_cache() on close.
 *
 * The last sector of the file may be smaller than sector_size if the
 * file size is not a multiple of sector_size.
 */
int obmafs3_write_media_image_data(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t offset,
                                   const void *buf, size_t size, uint16_t sector_size, struct sector_map_cache *cache,
                                   struct dedup_block_cache *db_cache)
{
    int rc = OBMAFS3_OK;

    /* Update file_size (the logical image size) */
    uint64_t new_end = offset + size;
    if(new_end > inode->file_size) inode->file_size = new_end;

    /* Get (or create) the dedup tree for this sector size.
     * When a persistent cache is provided and already has a valid
     * header, skip the expensive disk read entirely. */
    struct btree_header dedup_hdr;
    uint64_t            dedup_hdr_lba;
    if(db_cache && db_cache->hdr_cached)
    {
        dedup_hdr     = db_cache->dedup_hdr;
        dedup_hdr_lba = db_cache->dedup_hdr_lba;
    }
    else
    {
        rc = obmafs3_dedup_get_tree(ctx, sector_size, &dedup_hdr, &dedup_hdr_lba);
        if(rc != OBMAFS3_OK) return rc;
        if(db_cache)
        {
            db_cache->dedup_hdr     = dedup_hdr;
            db_cache->dedup_hdr_lba = dedup_hdr_lba;
            db_cache->hdr_cached    = 1;
        }
    }

    /* Initialize the in-memory dedup block — use the persistent cache
     * if the caller provided one, otherwise fall back to a local ctx
     * that is read from disk each call. */
    struct dedup_block_ctx  db_local;
    struct dedup_block_ctx *db;
    int                     db_is_cached = 0;

    if(db_cache)
    {
        if(!db_cache->initialized)
        {
            /* First call — bootstrap the cache from the tree header */
            db_local.data       = NULL;
            db_local.block_lba  = 0;
            db_local.offset     = 0;
            db_local.capacity   = 0;
            db_local.std_blocks = 0;
            db_local.dirty      = 0;
            rc                  = dedup_block_init(ctx, &dedup_hdr, &db_local);
            if(rc != OBMAFS3_OK) return rc;
            /* Migrate into the persistent cache struct */
            db_cache->data        = db_local.data;
            db_cache->block_lba   = db_local.block_lba;
            db_cache->offset      = db_local.offset;
            db_cache->capacity    = db_local.capacity;
            db_cache->std_blocks  = db_local.std_blocks;
            db_cache->dirty       = db_local.dirty;
            db_cache->initialized = 1;
        }
        /* Wrap the cache fields into a stack-local dedup_block_ctx
         * that points to the same buffer.  We copy back at the end. */
        db_local.data       = db_cache->data;
        db_local.block_lba  = db_cache->block_lba;
        db_local.offset     = db_cache->offset;
        db_local.capacity   = db_cache->capacity;
        db_local.std_blocks = db_cache->std_blocks;
        db_local.dirty      = db_cache->dirty;
        db                  = &db_local;
        db_is_cached        = 1;
    }
    else
    {
        rc = dedup_block_init(ctx, &dedup_hdr, &db_local);
        if(rc != OBMAFS3_OK) return rc;
        db = &db_local;
    }

    /*
     * Pre-allocate a buffer for sector_map_entries.
     * When cache is NULL but db_cache is provided (CD image path),
     * the caller manages its own sector map — skip entirely.
     * Maximum number of sectors in this write = size / sector_size + 1.
     */
    int                      skip_sme = (cache == NULL && db_cache != NULL);
    struct sector_map_entry *sme_buf  = NULL;
    uint64_t                 sme_count = 0;
    if(!skip_sme)
    {
        uint64_t max_sectors = size / sector_size + 1;
        sme_buf = malloc((size_t)(max_sectors * sizeof(struct sector_map_entry)));
        if(!sme_buf)
        {
            if(!db_is_cached) dedup_block_free(db);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }
    }

    /* Wait for the background warmup thread to finish populating
     * the node cache and key set.  If warmup is already done, this
     * returns immediately (just a mutex lock + flag check). */
    obmafs3_dedup_warmup_wait(ctx);

    /* Lazy fallback: if warmup wasn't started (e.g. mkobmafs path),
     * create the node cache and key set inline. */
    if(!ctx->dedup_node_cache)
    {
        struct dedup_node_cache *nc = dedup_cache_create((size_t)ctx->sb.block_size);
        if(nc) ctx->dedup_node_cache = nc;
    }

    const uint8_t *data = (const uint8_t *)buf;
    size_t         bsz  = (size_t)ctx->sb.block_size;

    /* Use the thread-local node buffer for dedup tree traversal. */
    uint8_t *tree_buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct timespec t_prefetch_start, t_prefetch_end;
    struct timespec t_phase1_start, t_phase1_end;
    struct timespec t_phase2_start;
    struct timespec t_bgwait_end, t_flush_end, t_ncflush_end, t_hdr_end, t_sme_end;
    uint64_t dedup_hits = 0, dedup_misses = 0;
    uint64_t prefetch_advised = 0;

    /*
     * Pre-compute per-sector metadata: hash, data pointer, length,
     * sector number.  This array feeds both the leaf prefetch and the
     * hash-sorted main loop, eliminating double-hashing.
     */
    uint64_t num_sectors = size / sector_size + (size % sector_size ? 1 : 0);

    struct sector_work
    {
        uint64_t       hash;
        const uint8_t *data;
        size_t         len;
        int64_t        sector_num;
    };

    struct sector_work *sw = malloc((size_t)(num_sectors * sizeof(struct sector_work)));
    if(!sw)
    {
        free(sme_buf);
        if(!db_is_cached) dedup_block_free(db);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    {
        size_t bp = 0;
        uint64_t si = 0;
        while(bp < size && si < num_sectors)
        {
            uint64_t wp  = offset + bp;
            size_t   ois = (size_t)(wp % sector_size);
            size_t   riw = size - bp;
            size_t   ris = (size_t)sector_size - ois;
            size_t   ch  = (riw < ris) ? riw : ris;

            sw[si].data       = data + bp;
            sw[si].len        = ch;
            sw[si].sector_num = (int64_t)(wp / sector_size);
            sw[si].hash       = obmafs3_checksum_xxh64(sw[si].data, sw[si].len);
            si++;
            bp += ch;
        }
        num_sectors = si;
    }

    /* Build a hash-sorted index for processing order.
     * Processing sectors in hash order groups tree lookups/inserts
     * that target the same leaf node, turning N separate
     * read-modify-write cycles into 1 read + N inserts + 1 write. */
    uint32_t *sorted_idx = malloc((size_t)(num_sectors * sizeof(uint32_t)));
    if(!sorted_idx)
    {
        free(sw);
        free(sme_buf);
        if(!db_is_cached) dedup_block_free(db);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }
    for(uint64_t i = 0; i < num_sectors; i++) sorted_idx[i] = (uint32_t)i;

    /* Indirect sort by hash — keeps sw[] in original order for sme_buf. */
    {
        /* Use sw pointer via a file-scope helper (qsort_r is non-portable) */
        const struct sector_work *g_sw = sw;
        /* Simple insertion sort: num_sectors is small (≤512) and the
         * array is often nearly sorted by hash already. */
        for(uint64_t i = 1; i < num_sectors; i++)
        {
            uint32_t key = sorted_idx[i];
            uint64_t kh  = g_sw[key].hash;
            int64_t  j   = (int64_t)i - 1;
            while(j >= 0 && g_sw[sorted_idx[j]].hash > kh)
            {
                sorted_idx[j + 1] = sorted_idx[j];
                j--;
            }
            sorted_idx[j + 1] = key;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_prefetch_start);

    /*
     * Fast-path: check how many hashes are already in the key set.
     * If all are known, we can skip the entire prefetch phase —
     * the Phase 1 loop will resolve them via keyset_contains()
     * without any tree traversal or disk I/O.
     */
    uint64_t keyset_fast_hits = 0;
    {
        const struct dedup_key_set *ks = (const struct dedup_key_set *)ctx->dedup_key_set;
        if(ks)
        {
            for(uint64_t i = 0; i < num_sectors; i++)
            {
                if(keyset_contains(ks, sw[i].hash)) keyset_fast_hits++;
            }
        }
    }

    /*
     * Leaf prefetch: use the pre-computed hashes to find target leaf
     * LBAs and batch-prefetch them with posix_fadvise.
     * Skipped entirely when all hashes are already in the key set.
     */
    if(keyset_fast_hits < num_sectors)
    {
        struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
        if(nc && dedup_hdr.root_node_lba != 0)
        {
            uint64_t *pf_lbas = malloc((size_t)(num_sectors * sizeof(uint64_t)));
            if(pf_lbas)
            {
                for(uint64_t i = 0; i < num_sectors; i++)
                {
                    uint64_t leaf_lba = 0;
                    dedup_find_leaf_lba(ctx, &dedup_hdr, sw[i].hash, &leaf_lba, tree_buf, nc);
                    pf_lbas[i] = leaf_lba;
                }

                /* Sort leaf LBAs for sequential disk access */
                qsort(pf_lbas, (size_t)num_sectors, sizeof(uint64_t), lba_cmp);

                uint64_t prev_lba = 0;
                for(uint64_t i = 0; i < num_sectors; i++)
                {
                    if(pf_lbas[i] == 0 || pf_lbas[i] == prev_lba) continue;
                    prev_lba = pf_lbas[i];
                    if(cache_find_slot(nc, pf_lbas[i])) continue;
                    posix_fadvise(ctx->fd, (off_t)(pf_lbas[i] * bsz),
                                  (off_t)bsz, POSIX_FADV_WILLNEED);
                    prefetch_advised++;
                }

                prev_lba = 0;
                for(uint64_t i = 0; i < num_sectors; i++)
                {
                    if(pf_lbas[i] == 0 || pf_lbas[i] == prev_lba) continue;
                    prev_lba = pf_lbas[i];
                    struct dedup_cache_slot *slot = cache_find_slot(nc, pf_lbas[i]);
                    if(slot)
                    {
                        /* Leaf is cached — still ingest its keys into
                         * the key set in case it was cached before the
                         * set was created or last seeded. */
                        keyset_ingest_leaf((struct dedup_key_set *)ctx->dedup_key_set, slot->buf);
                        continue;
                    }
                    nc_block_read(nc, ctx, pf_lbas[i], tree_buf, bsz);
                }

                free(pf_lbas);
            }
        }
    } /* keyset_fast_hits < num_sectors */

    clock_gettime(CLOCK_MONOTONIC, &t_prefetch_end);
    clock_gettime(CLOCK_MONOTONIC, &t_phase1_start);

    /*
     * Phase 1: Process sectors in hash-sorted order — dedup lookup,
     *          store new sectors, insert into tree.  Hash ordering
     *          groups accesses to the same leaf node, so consecutive
     *          inserts typically share a single cached leaf read.
     *
     *          sector_map_entries are filled at their original index
     *          to preserve positional ordering for reads.
     */
    sme_count = num_sectors; /* all slots will be filled */
    for(uint64_t si = 0; si < num_sectors; si++)
    {
        uint32_t idx = sorted_idx[si];
        uint64_t hash       = sw[idx].hash;
        const uint8_t *sdata = sw[idx].data;
        size_t         slen  = sw[idx].len;
        int64_t        snum  = sw[idx].sector_num;

        /* Fast path: if the key set confirms this hash exists,
         * skip the full tree traversal entirely — zero disk I/O. */
        struct dedup_key_set *ks = (struct dedup_key_set *)ctx->dedup_key_set;
        if(keyset_contains(ks, hash))
        {
            dedup_hits++;
        }
        else
        {
            /* Full tree lookup, saving traversal for potential insert */
            struct dedup_entry       existing;
            struct dedup_upsert_ctx  uctx;
            struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
            rc = dedup_upsert_find(ctx, &dedup_hdr, hash, &existing, &uctx, tree_buf, nc);

            if(rc == OBMAFS3_OK) { dedup_hits++; }
            else if(rc == OBMAFS3_ERR_NOTFOUND)
            {
                dedup_misses++;
                uint64_t             stored_lba, stored_offset;
                struct compress_pool *pool = db_cache ? ctx->compress_pool : NULL;
                void                **pjob = db_cache ? &db_cache->pending_job : NULL;
                rc = dedup_block_store(ctx, db, sdata, slen, &stored_lba, &stored_offset, pool, pjob);
                if(rc != OBMAFS3_OK) { free(sw); free(sorted_idx); goto out; }

                struct dedup_entry new_entry;
                new_entry.hash         = hash;
                new_entry.block_lba    = stored_lba;
                new_entry.block_offset = stored_offset;

                rc = dedup_upsert_insert(ctx, &dedup_hdr, &new_entry, &uctx, tree_buf, nc);
                if(rc != OBMAFS3_OK) { free(sw); free(sorted_idx); goto out; }

                /* Add the newly inserted key to the set for future lookups */
                if(ks) keyset_insert(ks, hash);
            }
            else
            {
                free(sw); free(sorted_idx); goto out;
            }
        }

        /* Fill sme_buf at the original position (sector order) */
        if(!skip_sme)
        {
            sme_buf[idx].sector      = snum;
            sme_buf[idx].sector_size = sector_size;
            sme_buf[idx].hash        = hash;
        }

        /* Update sector count */
        if((uint64_t)(snum + 1) > inode->sector_count) inode->sector_count = (uint64_t)(snum + 1);
    }

    free(sw);
    free(sorted_idx);

    /* Adjust sme_count: when skip_sme, it stays 0 */
    if(skip_sme) sme_count = 0;

    clock_gettime(CLOCK_MONOTONIC, &t_phase1_end);

    /*
     * Phase 2: Flush dedup data, then write all sector_map_entries in
     *          one bulk call.  This ensures sector map block allocations
     *          are contiguous (single extent), since no other allocations
     *          happen between them.
     */
    clock_gettime(CLOCK_MONOTONIC, &t_phase2_start);

    /* Wait for any pending background compression before syncing */
    if(db_cache && db_cache->pending_job)
    {
        int bg_rc = dedup_bg_wait(ctx, &db_cache->pending_job);
        if(rc == OBMAFS3_OK) rc = bg_rc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_bgwait_end);

    /* Flush any remaining dedup block data to disk (synchronous).
     * Skip intermediate flushes when using a persistent cache —
     * the block will be flushed when it fills up (in dedup_block_store)
     * or on file close (obmafs3_flush_dedup_block_cache).  Flushing
     * on every FUSE write is extremely expensive because it re-compresses
     * the entire partial block (up to 4 MiB at ZSTD level 15). */
    if(!db_is_cached && db->dirty)
    {
        int flush_rc = dedup_block_flush(ctx, db);
        if(rc == OBMAFS3_OK) rc = flush_rc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_flush_end);

    /* Flush cached tree nodes to disk before updating the header */
    if(ctx->dedup_node_cache)
    {
        int nc_rc = dedup_cache_flush((struct dedup_node_cache *)ctx->dedup_node_cache, ctx);
        if(rc == OBMAFS3_OK) rc = nc_rc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_ncflush_end);

    /* Update the tree header with the current partial block state.
     * Skip the disk write when nothing changed (all duplicates and
     * the dedup block position is unchanged). */
    dedup_hdr.last_block_lba    = db->block_lba;
    dedup_hdr.last_block_offset = db->offset;
    if(dedup_misses > 0 || !db_is_cached)
    {
        int hdr_rc = obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);
        if(rc == OBMAFS3_OK) rc = hdr_rc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_hdr_end);

    /* Keep the cached header in sync */
    if(db_cache) db_cache->dedup_hdr = dedup_hdr;

    /* Now write or cache the sector map entries (skipped for CD images) */
    if(!skip_sme && rc == OBMAFS3_OK && sme_count > 0)
    {
        if(cache)
        {
            /* Append to in-memory cache */
            uint64_t need = cache->count + sme_count;
            if(need > cache->capacity)
            {
                uint64_t new_cap = cache->capacity;
                if(new_cap == 0) new_cap = 1024;
                while(new_cap < need) new_cap *= 2;
                struct sector_map_entry *tmp = realloc(cache->entries, (size_t)(new_cap * sizeof(*tmp)));
                if(!tmp) { rc = OBMAFS3_ERR_NOMEM; }
                else
                {
                    cache->entries  = tmp;
                    cache->capacity = new_cap;
                }
            }
            if(rc == OBMAFS3_OK)
            {
                memcpy(cache->entries + cache->count, sme_buf, (size_t)(sme_count * sizeof(*sme_buf)));
                cache->count += sme_count;
            }
        }
        else
        {
            rc = write_sector_map_batch(ctx, inode, sme_buf, sme_count);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_sme_end);

    free(sme_buf);

    /* Print timing instrumentation */
    {
        struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
        struct dedup_key_set    *ks = (struct dedup_key_set *)ctx->dedup_key_set;
        uint32_t nc_count = nc ? nc->count : 0;
        uint32_t nc_cap   = nc ? nc->capacity : 0;
        uint32_t ks_count = ks ? ks->count : 0;
        uint32_t ks_cap   = ks ? ks->capacity : 0;
        fprintf(stderr,
                "[dedup-timing] write %zu bytes @ %" PRIu64 ": "
                "prefetch=%.1fms(%" PRIu64 " leaves)  "
                "phase1=%.1fms  bg_wait=%.1fms  blk_flush=%.1fms  "
                "nc_flush=%.1fms  hdr=%.1fms  sme=%.1fms  TOTAL=%.1fms  "
                "hits=%" PRIu64 " misses=%" PRIu64
                " nc_count=%u/%u ks=%u/%u ks_fast=%" PRIu64 "\n",
                size, offset,
                timespec_diff_ms(&t_prefetch_start, &t_prefetch_end), prefetch_advised,
                timespec_diff_ms(&t_phase1_start, &t_phase1_end),
                timespec_diff_ms(&t_phase2_start, &t_bgwait_end),
                timespec_diff_ms(&t_bgwait_end, &t_flush_end),
                timespec_diff_ms(&t_flush_end, &t_ncflush_end),
                timespec_diff_ms(&t_ncflush_end, &t_hdr_end),
                timespec_diff_ms(&t_hdr_end, &t_sme_end),
                timespec_diff_ms(&t_prefetch_start, &t_sme_end),
                dedup_hits, dedup_misses, nc_count, nc_cap,
                ks_count, ks_cap, keyset_fast_hits);
    }

    /* Copy updated state back to the persistent cache if used */
    if(db_is_cached)
    {
        db_cache->data       = db->data;
        db_cache->block_lba  = db->block_lba;
        db_cache->offset     = db->offset;
        db_cache->capacity   = db->capacity;
        db_cache->std_blocks = db->std_blocks;
        db_cache->dirty      = db->dirty;
    }
    else
    {
        dedup_block_free(db);
    }

    return rc;

out:
    /* Error path — wait for bg, then flush dedup state */
    if(db_cache && db_cache->pending_job)
    {
        int bg_rc = dedup_bg_wait(ctx, &db_cache->pending_job);
        if(bg_rc != OBMAFS3_OK && rc == OBMAFS3_OK) rc = bg_rc;
    }
    if(db->data && db->dirty) dedup_block_flush(ctx, db);

    /* Flush cached tree nodes even on error to keep disk consistent */
    if(ctx->dedup_node_cache) dedup_cache_flush((struct dedup_node_cache *)ctx->dedup_node_cache, ctx);

    dedup_hdr.last_block_lba    = db->block_lba;
    dedup_hdr.last_block_offset = db->offset;
    obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);

    /* Keep the cached header in sync */
    if(db_cache) db_cache->dedup_hdr = dedup_hdr;

    free(sme_buf);
    if(db_is_cached)
    {
        db_cache->data       = db->data;
        db_cache->block_lba  = db->block_lba;
        db_cache->offset     = db->offset;
        db_cache->capacity   = db->capacity;
        db_cache->std_blocks = db->std_blocks;
        db_cache->dirty      = db->dirty;
    }
    else
    {
        dedup_block_free(db);
    }
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
     * Use the cached header if available to avoid a disk read. */
    struct btree_header dedup_hdr;
    uint64_t            dedup_hdr_lba;
    int                 hrc;
    if(db_cache->hdr_cached)
    {
        dedup_hdr     = db_cache->dedup_hdr;
        dedup_hdr_lba = db_cache->dedup_hdr_lba;
        hrc           = OBMAFS3_OK;
    }
    else
    {
        hrc = obmafs3_dedup_get_tree(ctx, sector_size, &dedup_hdr, &dedup_hdr_lba);
    }
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
    dedup_cache_free((struct dedup_node_cache *)ctx->dedup_node_cache);
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

/* ------------------------------------------------------------------ */
/*  Background dedup key set warmup                                    */
/* ------------------------------------------------------------------ */

/**
 * Background thread entry point: create the node cache and key set,
 * then scan every dedup tree's leaves to populate the key set.
 *
 * Runs entirely independently of the FUSE write path.  The write path
 * calls obmafs3_dedup_warmup_wait() which blocks until this thread
 * signals completion.
 */
static void *warmup_thread_func(void *arg)
{
    struct obmafs3_ctx *ctx = (struct obmafs3_ctx *)arg;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Create the node cache if it doesn't exist yet. */
    if(!ctx->dedup_node_cache)
    {
        struct dedup_node_cache *nc = dedup_cache_create((size_t)ctx->sb.block_size);
        if(nc) ctx->dedup_node_cache = nc;
    }

    /* Create the key set. */
    struct dedup_key_set *ks = keyset_create();
    if(!ks) goto done;
    ctx->dedup_key_set = ks;

    /* Seed from any already-cached nodes (may be 0). */
    keyset_seed_from_cache(ks, (const struct dedup_node_cache *)ctx->dedup_node_cache);

    /* Read the dedup tree list and warm up every tree. */
    struct tree_list_header list_hdr;
    struct tree_list_entry *entries = NULL;
    uint64_t                count   = 0;
    int rc = dedup_tree_list_read(ctx, &list_hdr, &entries, &count);
    if(rc == OBMAFS3_OK && count > 0)
    {
        /* Allocate a scratch buffer for tree traversal. */
        uint8_t *buf = malloc((size_t)ctx->sb.block_size);
        if(buf)
        {
            for(uint64_t i = 0; i < count; i++)
            {
                struct btree_header hdr;
                rc = obmafs3_btree_header_read(ctx, entries[i].tree_lba, &hdr);
                if(rc == OBMAFS3_OK && hdr.root_node_lba != 0)
                {
                    keyset_warmup(ctx, &hdr, buf,
                                  (struct dedup_node_cache *)ctx->dedup_node_cache);
                }
            }
            free(buf);
        }
        free(entries);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    fprintf(stderr, "[dedup-warmup] background warmup completed in %.1fms — key set has %u keys\n",
            timespec_diff_ms(&t_start, &t_end), ks->count);

done:
    pthread_mutex_lock(&ctx->warmup_mutex);
    ctx->warmup_done    = 1;
    ctx->warmup_running = 0;
    pthread_cond_broadcast(&ctx->warmup_cond);
    pthread_mutex_unlock(&ctx->warmup_mutex);
    return NULL;
}

/**
 * Start the background dedup key set warmup thread.
 *
 * Called from FUSE init (after daemonisation) so the warmup runs
 * concurrently with any early FUSE operations.  The write path
 * calls obmafs3_dedup_warmup_wait() to block until completion.
 */
void obmafs3_dedup_warmup_start(struct obmafs3_ctx *ctx)
{
    if(!ctx) return;

    pthread_mutex_lock(&ctx->warmup_mutex);
    ctx->warmup_done    = 0;
    ctx->warmup_running = 1;
    pthread_mutex_unlock(&ctx->warmup_mutex);

    int rc = pthread_create(&ctx->warmup_thread, NULL, warmup_thread_func, ctx);
    if(rc != 0)
    {
        /* Thread creation failed — fall back to lazy init in write path. */
        fprintf(stderr, "[dedup-warmup] failed to create background thread (rc=%d)\n", rc);
        pthread_mutex_lock(&ctx->warmup_mutex);
        ctx->warmup_running = 0;
        ctx->warmup_done    = 1;
        pthread_cond_broadcast(&ctx->warmup_cond);
        pthread_mutex_unlock(&ctx->warmup_mutex);
    }
    else
    {
        ctx->warmup_started = 1;
    }
}

/**
 * Wait for the background warmup thread to finish.
 *
 * Called at the top of the write path and during unmount.  If warmup
 * is already done, this returns immediately (just a mutex lock +
 * flag check).  Safe to call multiple times.
 */
void obmafs3_dedup_warmup_wait(struct obmafs3_ctx *ctx)
{
    if(!ctx || !ctx->warmup_started) return;

    pthread_mutex_lock(&ctx->warmup_mutex);
    while(!ctx->warmup_done)
        pthread_cond_wait(&ctx->warmup_cond, &ctx->warmup_mutex);
    pthread_mutex_unlock(&ctx->warmup_mutex);
}

/* ------------------------------------------------------------------ */
/*  Background compression start / stop                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Sector map cache flush / free                                      */
/* ------------------------------------------------------------------ */

/**
 * Flush cached sector map entries to disk.
 *
 * Writes all accumulated @c sector_map_entry records from @p cache to
 * the inode's data blocks in a single batch and resets the cache count.
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode record to update (modified in place).
 * @param cache  Sector map cache to flush.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_flush_sector_map_cache(struct obmafs3_ctx *ctx, struct inode_record *inode, struct sector_map_cache *cache)
{
    if(!cache || cache->count == 0) return OBMAFS3_OK;

    int rc = write_sector_map_batch(ctx, inode, cache->entries, cache->count);
    if(rc == OBMAFS3_OK) { cache->count = 0; /* keep the buffer for potential reuse */ }
    return rc;
}

/**
 * Free all resources held by a sector map cache.
 *
 * @param cache  Sector map cache to free.
 */
void obmafs3_free_sector_map_cache(struct sector_map_cache *cache)
{
    if(!cache) return;
    free(cache->entries);
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}

/* ------------------------------------------------------------------ */
/*  Media image read path                                              */
/* ------------------------------------------------------------------ */

/**
 * Read data from a media image file.
 *
 * The inode's data blocks store sector_map_entry records that map
 * each logical sector to a hash.  The hash is used to look up the
 * actual sector data in the dedup tree.
 *
 * @param ctx         Filesystem context.
 * @param inode       Inode of the media image file.
 * @param offset      Byte offset in the original file.
 * @param buf         Output buffer.
 * @param size        Number of bytes to read.
 * @param sector_size Size of each sector in the image.
 * @return OBMAFS3_OK on success, error code otherwise.
 */
int obmafs3_read_media_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                                  size_t size, uint16_t sector_size)
{
    if(offset >= inode->file_size) return OBMAFS3_OK;

    if(offset + size > inode->file_size) size = (size_t)(inode->file_size - offset);

    if(size == 0) return OBMAFS3_OK;

    /* Get the dedup tree for this sector size */
    struct btree_header dedup_hdr;
    uint64_t            dedup_hdr_lba;
    int                 rc = obmafs3_dedup_get_tree(ctx, sector_size, &dedup_hdr, &dedup_hdr_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Buffer for reading the dedup data block (dedup_block_size bytes) */
    uint8_t *dedup_buf = malloc((size_t)ctx->sb.dedup_block_size);
    if(!dedup_buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Decompressed payload buffer (allocated on first compressed block) */
    uint8_t *decomp_buf = NULL;

    /* Cache the last read dedup block LBA to avoid re-reading */
    uint64_t cached_dedup_lba  = 0;
    int      cached_compressed = 0;

    /* Temporary inode copy for reading sector map (need to adjust file_size) */
    struct inode_record map_inode;
    memcpy(&map_inode, inode, sizeof(map_inode));
    map_inode.file_size = inode->sector_map_size * sizeof(struct sector_map_entry);

    uint8_t *out        = (uint8_t *)buf;
    size_t   bytes_read = 0;

    while(bytes_read < size)
    {
        uint64_t read_pos         = offset + bytes_read;
        int64_t  sector_num       = (int64_t)(read_pos / sector_size);
        size_t   offset_in_sector = (size_t)(read_pos % sector_size);

        /* How many bytes remain in this sector */
        size_t remaining_in_sector = (size_t)sector_size - offset_in_sector;
        size_t remaining_in_read   = size - bytes_read;
        size_t chunk               = remaining_in_read < remaining_in_sector ? remaining_in_read : remaining_in_sector;

        /* Read the sector_map_entry for this sector from inode data */
        struct sector_map_entry sme;
        uint64_t                sme_offset = (uint64_t)sector_num * sizeof(struct sector_map_entry);
        rc                                 = obmafs3_read_file_data(ctx, &map_inode, sme_offset, &sme, sizeof(sme));
        if(rc != OBMAFS3_OK)
        {
            free(decomp_buf);
            free(dedup_buf);
            return rc;
        }

        /* Look up the hash in the dedup tree */
        struct dedup_entry de;
        rc = obmafs3_dedup_lookup(ctx, &dedup_hdr, sme.hash, &de);
        if(rc != OBMAFS3_OK)
        {
            free(decomp_buf);
            free(dedup_buf);
            return rc;
        }

        /* Read the dedup data block if not already cached */
        if(de.block_lba != cached_dedup_lba)
        {
            /* Read first standard block to get the header */
            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK)
            {
                free(decomp_buf);
                free(dedup_buf);
                return rc;
            }

            /* Check if the block is compressed */
            struct block_header bhdr;
            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

            /* Determine actual on-disk payload size and read remaining */
            uint64_t payload_size;
            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                payload_size = bhdr.compressed_size;
            else
                payload_size = bhdr.original_size;

            uint64_t total_on_disk = sizeof(bhdr) + payload_size;
            uint64_t bs            = ctx->sb.block_size;
            uint64_t needed_std    = (total_on_disk + bs - 1) / bs;

            /* Read remaining standard blocks beyond the first */
            if(needed_std > 1)
            {
                rc = obmafs3_block_read(ctx, de.block_lba + 1, dedup_buf + bs, (size_t)((needed_std - 1) * bs));
                if(rc != OBMAFS3_OK)
                {
                    free(decomp_buf);
                    free(dedup_buf);
                    return rc;
                }
            }

            cached_dedup_lba = de.block_lba;

            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                if(!decomp_buf)
                {
                    decomp_buf = malloc((size_t)ctx->sb.dedup_block_size);
                    if(!decomp_buf)
                    {
                        free(dedup_buf);
                        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                    }
                }
                rc = obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, dedup_buf + sizeof(bhdr), (size_t)bhdr.compressed_size,
                                        decomp_buf, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK)
                {
                    free(decomp_buf);
                    free(dedup_buf);
                    return rc;
                }
                cached_compressed = 1;
            }
            else
            {
                cached_compressed = 0;
            }
        }

        /* Copy sector data from the dedup block at the stored offset.
         * block_offset includes the header prefix; for compressed blocks
         * decomp_buf holds only the payload so subtract the header. */
        if(cached_compressed)
        {
            size_t decomp_off = de.block_offset - sizeof(struct block_header);
            memcpy(out + bytes_read, decomp_buf + decomp_off + offset_in_sector, chunk);
        }
        else
        {
            memcpy(out + bytes_read, dedup_buf + de.block_offset + offset_in_sector, chunk);
        }
        bytes_read += chunk;
    }

    free(decomp_buf);
    free(dedup_buf);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  CD sector map cache flush / free                                   */
/* ------------------------------------------------------------------ */

/**
 * Write a batch of CD sector map entries to the inode's data blocks.
 *
 * Appends @p count entries at the current @c sector_map_size offset
 * and advances the map size accordingly.
 *
 * @param ctx      Filesystem context.
 * @param inode    Inode record to update (modified in place).
 * @param entries  Array of CD sector map entries to write.
 * @param count    Number of entries.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int write_cd_sector_map_batch(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                     const struct cd_sector_map_entry *entries, uint64_t count)
{
    if(count == 0) return OBMAFS3_OK;

    size_t   entry_size  = sizeof(struct cd_sector_map_entry);
    uint64_t map_offset  = inode->sector_map_size * entry_size;
    size_t   total_bytes = (size_t)(count * entry_size);

    uint64_t saved_file_size = inode->file_size;
    inode->file_size         = map_offset;

    int rc = obmafs3_write_file_data(ctx, inode, map_offset, entries, total_bytes);

    inode->file_size = saved_file_size;

    if(rc == OBMAFS3_OK) inode->sector_map_size += count;

    return rc;
}

/**
 * Flush cached CD sector map entries to disk.
 *
 * Writes all accumulated @c cd_sector_map_entry records from @p cache
 * to disk and resets the cache count.
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode record to update.
 * @param cache  CD sector map cache to flush.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_flush_cd_sector_map_cache(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                      struct cd_sector_map_cache *cache)
{
    if(!cache || cache->count == 0) return OBMAFS3_OK;

    int rc = write_cd_sector_map_batch(ctx, inode, cache->entries, cache->count);
    if(rc == OBMAFS3_OK) { cache->count = 0; }
    return rc;
}

/**
 * Free all resources held by a CD sector map cache.
 *
 * @param cache  CD sector map cache to free.
 */
void obmafs3_free_cd_sector_map_cache(struct cd_sector_map_cache *cache)
{
    if(!cache) return;
    free(cache->entries);
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}
