// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_cache.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 deduplication cache operations.
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
/*                                                                     */
/*  Separate-chaining hash table  +  intrusive LRU doubly-linked list. */
/*  Slots are pre-allocated in a flat pool; a free-slot list tracks    */
/*  unused entries.  On capacity exhaustion the LRU tail is evicted    */
/*  (flushed to disk if dirty) — no rehashing ever required.           */
/* ------------------------------------------------------------------ */

/* ---- LRU helpers (caller holds nc->lock) ---- */

/** Unlink slot @p idx from the LRU doubly-linked list. */
static void nc_lru_unlink(struct dedup_node_cache *nc, uint32_t idx)
{
    struct dedup_cache_slot *s = &nc->slots[idx];
    if(s->lru_prev != DEDUP_NC_NIL)
        nc->slots[s->lru_prev].lru_next = s->lru_next;
    else
        nc->lru_head = s->lru_next;

    if(s->lru_next != DEDUP_NC_NIL)
        nc->slots[s->lru_next].lru_prev = s->lru_prev;
    else
        nc->lru_tail = s->lru_prev;

    s->lru_prev = DEDUP_NC_NIL;
    s->lru_next = DEDUP_NC_NIL;
}

/** Push slot @p idx to the front (MRU position) of the LRU list. */
static void nc_lru_push_front(struct dedup_node_cache *nc, uint32_t idx)
{
    struct dedup_cache_slot *s = &nc->slots[idx];
    s->lru_prev = DEDUP_NC_NIL;
    s->lru_next = nc->lru_head;
    if(nc->lru_head != DEDUP_NC_NIL)
        nc->slots[nc->lru_head].lru_prev = idx;
    nc->lru_head = idx;
    if(nc->lru_tail == DEDUP_NC_NIL)
        nc->lru_tail = idx;
}

/* ---- Hash bucket helpers ---- */

/** Fibonacci-hashing of an LBA to a bucket index. */
static uint32_t cache_hash(uint64_t lba, uint32_t mask)
{
    return (uint32_t)((lba * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
}

/** Remove slot @p idx from its hash bucket chain. */
static void nc_chain_remove(struct dedup_node_cache *nc, uint32_t idx)
{
    struct dedup_cache_slot *s = &nc->slots[idx];
    uint32_t b = cache_hash(s->lba, nc->bucket_count - 1);
    uint32_t *pp = &nc->buckets[b];
    while(*pp != DEDUP_NC_NIL)
    {
        if(*pp == idx)
        {
            *pp = s->hash_next;
            s->hash_next = DEDUP_NC_NIL;
            return;
        }
        pp = &nc->slots[*pp].hash_next;
    }
}

/* ---- Dirty list ---- */

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

/* ---- Eviction ---- */

/**
 * Evict a single clean entry from the LRU tail.
 * Returns OBMAFS3_OK if one entry was freed, OBMAFS3_ERR_NOMEM if
 * the LRU tail was dirty (caller should flush it first).
 */
static int nc_evict_one_clean(struct dedup_node_cache *nc)
{
    /* Walk from the LRU tail (coldest) towards the head, looking for
     * the first clean entry.  Usually the tail itself is clean. */
    uint32_t idx = nc->lru_tail;
    while(idx != DEDUP_NC_NIL)
    {
        struct dedup_cache_slot *s = &nc->slots[idx];
        if(!s->dirty)
        {
            /* Found a clean entry — evict it. */
            nc_lru_unlink(nc, idx);
            nc_chain_remove(nc, idx);
            free(s->buf);
            s->buf = NULL;
            s->lba = 0;
            /* Return to free list. */
            s->hash_next = nc->free_head;
            nc->free_head = idx;
            nc->count--;
            return OBMAFS3_OK;
        }
        idx = s->lru_prev;
    }
    return OBMAFS3_ERR_NOMEM; /* all entries are dirty */
}

/**
 * Evict a single dirty entry from the LRU tail by flushing it to disk.
 * This is the "last resort" eviction for when every entry is dirty.
 * Writes one block via pwrite, then frees the slot.
 */
static int nc_evict_one_dirty(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx)
{
    uint32_t idx = nc->lru_tail;
    if(idx == DEDUP_NC_NIL) return OBMAFS3_ERR_NOMEM;

    struct dedup_cache_slot *s = &nc->slots[idx];
    /* Flush this single block to disk. */
    int rc = obmafs3_block_write(ctx, s->lba, s->buf, nc->block_size);
    if(rc != OBMAFS3_OK) return rc;
    s->dirty = 0;

    /* Now evict it. */
    nc_lru_unlink(nc, idx);
    nc_chain_remove(nc, idx);
    free(s->buf);
    s->buf = NULL;
    s->lba = 0;
    s->hash_next = nc->free_head;
    nc->free_head = idx;
    nc->count--;
    return OBMAFS3_OK;
}

/**
 * Ensure at least one free slot is available.
 * Tries clean eviction first, falls back to dirty eviction.
 */
static int nc_ensure_free_slot(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx)
{
    if(nc->free_head != DEDUP_NC_NIL) return OBMAFS3_OK;

    /* Try evicting a clean LRU entry. */
    int rc = nc_evict_one_clean(nc);
    if(rc == OBMAFS3_OK) return OBMAFS3_OK;

    /* All dirty — flush one dirty entry to disk and evict it. */
    return nc_evict_one_dirty(nc, ctx);
}

/* ---- Find ---- */

/** Find the slot for @lba, or NULL if not cached.
 *  Does NOT promote in LRU (callers that need promotion call
 *  nc_lru_unlink + nc_lru_push_front explicitly). */
struct dedup_cache_slot *cache_find_slot(struct dedup_node_cache *nc, uint64_t lba)
{
    uint32_t b   = cache_hash(lba, nc->bucket_count - 1);
    uint32_t idx = nc->buckets[b];
    while(idx != DEDUP_NC_NIL)
    {
        struct dedup_cache_slot *s = &nc->slots[idx];
        if(s->lba == lba) return s;
        idx = s->hash_next;
    }
    return NULL;
}

/* ---- Create / Destroy ---- */

/** Allocate and initialise a dedup B+Tree node cache.
 *  @param block_size  Filesystem block size in bytes.
 *  @param max_bytes   Maximum memory budget in bytes (0 = default 8 GiB).
 *                     Converted to a max slot count internally. */
struct dedup_node_cache *dedup_cache_create(size_t block_size, uint64_t max_bytes)
{
    struct dedup_node_cache *nc = calloc(1, sizeof(*nc));
    if(!nc) return NULL;
    nc->block_size = block_size;

    /* Compute capacity from byte budget.  Each slot holds one
     * block_size buffer + sizeof(dedup_cache_slot) of overhead. */
    if(max_bytes == 0) max_bytes = DEDUP_NC_DEFAULT_BYTES;
    {
        size_t   per_entry   = block_size + sizeof(struct dedup_cache_slot);
        uint64_t max_entries = max_bytes / per_entry;
        if(max_entries < DEDUP_CACHE_INIT_CAP) max_entries = DEDUP_CACHE_INIT_CAP;
        if(max_entries > UINT32_MAX / 2) max_entries = UINT32_MAX / 2;
        /* Round down to the nearest power-of-two. */
        uint32_t pot = 1u;
        while((uint64_t)pot * 2 <= max_entries) pot *= 2;
        nc->capacity     = pot;
        nc->max_capacity = pot;
    }

    /* Allocate the slot pool. */
    nc->slots = calloc(nc->capacity, sizeof(struct dedup_cache_slot));
    if(!nc->slots) { free(nc); return NULL; }

    /* Allocate hash buckets (2× capacity for ~0.5 avg chain length). */
    nc->bucket_count = nc->capacity * DEDUP_NC_BUCKET_FACTOR;
    nc->buckets = malloc(nc->bucket_count * sizeof(uint32_t));
    if(!nc->buckets) { free(nc->slots); free(nc); return NULL; }
    for(uint32_t i = 0; i < nc->bucket_count; i++)
        nc->buckets[i] = DEDUP_NC_NIL;

    /* Thread all slots into the free list (via hash_next). */
    for(uint32_t i = 0; i < nc->capacity - 1; i++)
    {
        nc->slots[i].hash_next = i + 1;
        nc->slots[i].lru_prev  = DEDUP_NC_NIL;
        nc->slots[i].lru_next  = DEDUP_NC_NIL;
    }
    nc->slots[nc->capacity - 1].hash_next = DEDUP_NC_NIL;
    nc->slots[nc->capacity - 1].lru_prev  = DEDUP_NC_NIL;
    nc->slots[nc->capacity - 1].lru_next  = DEDUP_NC_NIL;
    nc->free_head = 0;

    nc->lru_head = DEDUP_NC_NIL;
    nc->lru_tail = DEDUP_NC_NIL;
    nc->count    = 0;

    nc->dirty_cap  = 256;
    nc->dirty_list = malloc(nc->dirty_cap * sizeof(uint32_t));
    if(!nc->dirty_list)
    {
        free(nc->buckets);
        free(nc->slots);
        free(nc);
        return NULL;
    }
    nc->dirty_count = 0;
    pthread_mutex_init(&nc->lock, NULL);

    fprintf(stderr, "[nc] node cache created: block_size=%zu  capacity=%u (%.0f MiB budget)\n", block_size,
            nc->capacity,
            (double)nc->capacity * (block_size + sizeof(struct dedup_cache_slot)) / (1024.0 * 1024.0));

    return nc;
}

/* ---- Cache read / insert / write ---- */

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
        /* Promote to MRU. */
        nc_lru_unlink(nc, (uint32_t)(s - nc->slots));
        nc_lru_push_front(nc, (uint32_t)(s - nc->slots));
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
        /* Another thread already cached it — just promote. */
        nc_lru_unlink(nc, (uint32_t)(s - nc->slots));
        nc_lru_push_front(nc, (uint32_t)(s - nc->slots));
        pthread_mutex_unlock(&nc->lock);
        return OBMAFS3_OK;
    }

    /* Ensure a free slot is available (evict if necessary). */
    rc = nc_ensure_free_slot(nc, ctx);
    if(rc != OBMAFS3_OK)
    {
        pthread_mutex_unlock(&nc->lock);
        return OBMAFS3_OK; /* tolerate: data is in buf already */
    }

    /* Pop a slot from the free list. */
    uint32_t idx = nc->free_head;
    nc->free_head = nc->slots[idx].hash_next;

    nc->slots[idx].lba = lba;
    nc->slots[idx].buf = malloc(nc->block_size);
    if(nc->slots[idx].buf)
    {
        memcpy(nc->slots[idx].buf, buf, nc->block_size);
        nc->slots[idx].dirty = 0;
        nc->slots[idx].hash_next = DEDUP_NC_NIL;

        /* Insert into hash bucket. */
        uint32_t b = cache_hash(lba, nc->bucket_count - 1);
        nc->slots[idx].hash_next = nc->buckets[b];
        nc->buckets[b] = idx;

        /* Push to MRU. */
        nc_lru_push_front(nc, idx);
        nc->count++;
    }
    else
    {
        /* malloc failed — return slot to free list. */
        nc->slots[idx].hash_next = nc->free_head;
        nc->free_head = idx;
    }
    pthread_mutex_unlock(&nc->lock);
    return OBMAFS3_OK;
}

/**
 * Insert a block into the node cache directly from a caller-owned buffer.
 *
 * Unlike dedup_cache_read() this does NOT perform any disk I/O —
 * the data is assumed to already be in @p data.  If the LBA is already
 * cached the call is a no-op.  Used by the leaf-prefetch path which
 * reads a large contiguous range with a single pread() and then
 * distributes individual blocks into the cache.
 */
void dedup_cache_insert(struct dedup_node_cache *nc, uint64_t lba, const void *data)
{
    pthread_mutex_lock(&nc->lock);

    /* Already cached? */
    if(cache_find_slot(nc, lba))
    {
        pthread_mutex_unlock(&nc->lock);
        return;
    }

    /* Ensure a free slot (evict clean LRU if needed).
     * Insert is best-effort — if eviction fails, we tolerate. */
    if(nc->free_head == DEDUP_NC_NIL)
    {
        if(nc_evict_one_clean(nc) != OBMAFS3_OK)
        {
            pthread_mutex_unlock(&nc->lock);
            return; /* tolerate: all dirty, don't do I/O here */
        }
    }

    uint32_t idx = nc->free_head;
    nc->free_head = nc->slots[idx].hash_next;

    nc->slots[idx].lba = lba;
    nc->slots[idx].buf = malloc(nc->block_size);
    if(nc->slots[idx].buf)
    {
        memcpy(nc->slots[idx].buf, data, nc->block_size);
        nc->slots[idx].dirty = 0;
        nc->slots[idx].hash_next = DEDUP_NC_NIL;

        uint32_t b = cache_hash(lba, nc->bucket_count - 1);
        nc->slots[idx].hash_next = nc->buckets[b];
        nc->buckets[b] = idx;

        nc_lru_push_front(nc, idx);
        nc->count++;
    }
    else
    {
        nc->slots[idx].hash_next = nc->free_head;
        nc->free_head = idx;
    }
    pthread_mutex_unlock(&nc->lock);
}

/**
 * Write a tree node through the cache (write-back).
 * The data is stored in the cache and marked dirty; no disk I/O
 * happens until dedup_cache_flush().
 *
 * If the cache is full, evicts the LRU tail (flushing it to disk
 * if dirty).  Always succeeds unless the underlying disk write fails.
 */
int dedup_cache_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, const void *buf, size_t bsz)
{
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
        /* Promote to MRU. */
        nc_lru_unlink(nc, (uint32_t)(s - nc->slots));
        nc_lru_push_front(nc, (uint32_t)(s - nc->slots));
        return OBMAFS3_OK;
    }

    /* New entry — ensure a free slot (may evict + flush one dirty). */
    int rc = nc_ensure_free_slot(nc, ctx);
    if(rc != OBMAFS3_OK) return rc;

    uint32_t idx = nc->free_head;
    nc->free_head = nc->slots[idx].hash_next;

    nc->slots[idx].lba = lba;
    nc->slots[idx].buf = malloc(nc->block_size);
    if(!nc->slots[idx].buf)
    {
        /* Return slot to free list. */
        nc->slots[idx].hash_next = nc->free_head;
        nc->free_head = idx;
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }
    memcpy(nc->slots[idx].buf, buf, nc->block_size);
    nc->slots[idx].dirty = 1;
    nc->slots[idx].hash_next = DEDUP_NC_NIL;

    /* Insert into hash bucket. */
    uint32_t b = cache_hash(lba, nc->bucket_count - 1);
    nc->slots[idx].hash_next = nc->buckets[b];
    nc->buckets[b] = idx;

    /* Push to MRU. */
    nc_lru_push_front(nc, idx);
    nc->count++;
    dirty_list_add(nc, idx);
    return OBMAFS3_OK;
}

/** Flush all dirty entries to disk, clear dirty flags.
 *  Sorts dirty nodes by LBA, then coalesces consecutive LBAs into
 *  single pwritev() calls to minimise syscall overhead and let the
 *  kernel elevator-sort the resulting I/O. */
int dedup_cache_flush(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx)
{
    if(nc->dirty_count == 0)
    {
        nc->writes_since_flush = 0;
        return OBMAFS3_OK;
    }

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
                int rc = obmafs3_block_write(ctx, nc->slots[i].lba, nc->slots[i].buf, nc->block_size);
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
        if(!nc->slots[si].buf || !nc->slots[si].dirty)
        {
            d++;
            continue;
        }

        /* Start a new run at this slot's LBA. */
        uint64_t run_start_lba = nc->slots[si].lba;
        uint32_t iov_count     = 0;
        uint64_t expect_lba    = run_start_lba;

        /* Gather consecutive LBAs into the iovec, capped at IOV_MAX. */
        while(d < nc->dirty_count && iov_count < iov_cap)
        {
            uint32_t ci = nc->dirty_list[d];
            if(!nc->slots[ci].buf || !nc->slots[ci].dirty)
            {
                d++;
                continue;
            }
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
            fprintf(stderr,
                    "WARNING: pwritev lba=%" PRIu64 " count=%u "
                    "expect=%zu got=%zd (errno=%d %s) — falling back to "
                    "individual writes\n",
                    run_start_lba, iov_count, expected, n, errno, strerror(errno));

            int any_failed = 0;
            for(uint32_t r = 0; r < iov_count; r++)
            {
                uint64_t blk_lba = run_start_lba + r;
                int      wrc     = obmafs3_block_write(ctx, blk_lba, iov[r].iov_base, nc->block_size);
                if(wrc == OBMAFS3_OK)
                {
                    struct dedup_cache_slot *fs = cache_find_slot(nc, blk_lba);
                    if(fs) fs->dirty = 0;
                }
                else
                {
                    fprintf(stderr, "ERROR: fallback write lba=%" PRIu64 " failed (rc=%d)\n", blk_lba, wrc);
                    any_failed = 1;
                }
            }
            if(any_failed) goto flush_rebuild;
            continue;
        }

        /* Mark all slots in this chunk as clean. */
        for(uint32_t r = 0; r < iov_count; r++)
        {
            uint64_t                 clba = run_start_lba + r;
            struct dedup_cache_slot *fs   = cache_find_slot(nc, clba);
            if(fs) fs->dirty = 0;
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
        if(nc->slots[i].buf && nc->slots[i].dirty) dirty_list_add(nc, i);
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
    free(nc->buckets);
    free(nc->dirty_list);
    free(nc);
}

/* ------------------------------------------------------------------ */
/*  In-memory hash set of known dedup keys (chained hash + LRU)        */
/* ------------------------------------------------------------------ */

/** Fibonacci-hashing of a key to a bucket index. */
static uint32_t ks_bucket(uint64_t key, uint32_t bucket_count)
{
    return (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> 32) % bucket_count;
}

/** Promote slot @p idx to the MRU (head) position in the LRU list. */
static void ks_lru_touch(struct dedup_key_set *ks, uint32_t idx)
{
    if(ks->lru_head == idx) return; /* already MRU */

    /* Unlink from current position */
    uint32_t p = ks->slots[idx].lru_prev;
    uint32_t n = ks->slots[idx].lru_next;
    if(p != DEDUP_KS_NIL)
        ks->slots[p].lru_next = n;
    if(n != DEDUP_KS_NIL)
        ks->slots[n].lru_prev = p;
    else
        ks->lru_tail = p; /* idx was tail */

    /* Insert at head */
    ks->slots[idx].lru_prev = DEDUP_KS_NIL;
    ks->slots[idx].lru_next = ks->lru_head;
    if(ks->lru_head != DEDUP_KS_NIL)
        ks->slots[ks->lru_head].lru_prev = idx;
    ks->lru_head = idx;
    if(ks->lru_tail == DEDUP_KS_NIL) ks->lru_tail = idx;
}

/** Remove slot @p idx from its hash-bucket chain. */
static void ks_chain_remove(struct dedup_key_set *ks, uint32_t idx)
{
    uint32_t b = ks_bucket(ks->slots[idx].key, ks->bucket_count);
    uint32_t prev = DEDUP_KS_NIL;
    uint32_t cur  = ks->buckets[b];
    while(cur != DEDUP_KS_NIL)
    {
        if(cur == idx)
        {
            if(prev == DEDUP_KS_NIL)
                ks->buckets[b] = ks->slots[cur].chain_next;
            else
                ks->slots[prev].chain_next = ks->slots[cur].chain_next;
            return;
        }
        prev = cur;
        cur  = ks->slots[cur].chain_next;
    }
}

/**
 * Allocate a fixed-capacity dedup key set.
 *
 * @param max_bytes  RAM budget in bytes (0 = DEDUP_KS_DEFAULT_BYTES).
 *                   The capacity is computed so that total allocation
 *                   fits within this budget.
 */
struct dedup_key_set *keyset_create(uint64_t max_bytes)
{
    if(max_bytes == 0) max_bytes = DEDUP_KS_DEFAULT_BYTES;

    /* Per-slot cost: sizeof(struct ks_slot).
     * Per-bucket cost: sizeof(uint32_t).
     * With DEDUP_KS_BUCKET_FACTOR=2, bucket count = 2 × capacity.
     * Total ≈ capacity × (sizeof(ks_slot) + 2 × sizeof(uint32_t)) + fixed overhead.
     */
    size_t per_slot = sizeof(struct ks_slot) + DEDUP_KS_BUCKET_FACTOR * sizeof(uint32_t);
    uint64_t cap64  = max_bytes / per_slot;
    if(cap64 == 0) cap64 = 1;
    if(cap64 > UINT32_MAX) cap64 = UINT32_MAX;
    uint32_t capacity     = (uint32_t)cap64;
    uint32_t bucket_count = capacity * DEDUP_KS_BUCKET_FACTOR;
    if(bucket_count < capacity) bucket_count = UINT32_MAX; /* overflow guard */

    struct dedup_key_set *ks = calloc(1, sizeof(*ks));
    if(!ks) return NULL;

    ks->slots = malloc((size_t)capacity * sizeof(struct ks_slot));
    if(!ks->slots) { free(ks); return NULL; }

    ks->buckets = malloc((size_t)bucket_count * sizeof(uint32_t));
    if(!ks->buckets) { free(ks->slots); free(ks); return NULL; }

    ks->capacity     = capacity;
    ks->bucket_count = bucket_count;
    ks->count        = 0;
    ks->lru_head     = DEDUP_KS_NIL;
    ks->lru_tail     = DEDUP_KS_NIL;

    /* Initialise all buckets to NIL. */
    for(uint32_t i = 0; i < bucket_count; i++) ks->buckets[i] = DEDUP_KS_NIL;

    /* Build the free list through chain_next (singly linked). */
    for(uint32_t i = 0; i < capacity; i++)
    {
        ks->slots[i].key        = KEYSET_EMPTY;
        ks->slots[i].chain_next = (i + 1 < capacity) ? i + 1 : DEDUP_KS_NIL;
        ks->slots[i].lru_prev   = DEDUP_KS_NIL;
        ks->slots[i].lru_next   = DEDUP_KS_NIL;
    }
    ks->free_head = 0;

    fprintf(stderr, "[dedup-keyset] created: capacity=%u  buckets=%u  budget=%.1f GiB\n",
            capacity, bucket_count, (double)max_bytes / (1024.0 * 1024 * 1024));

    return ks;
}

/**
 * Insert a key into the set (no-op if already present).
 * When the set is full, the LRU (least-recently-used) entry is evicted.
 */
void keyset_insert(struct dedup_key_set *ks, uint64_t key)
{
    if(!ks || key == KEYSET_EMPTY) return; /* cannot store sentinel */

    uint32_t b = ks_bucket(key, ks->bucket_count);

    /* Walk the chain — if key exists, just touch it. */
    for(uint32_t cur = ks->buckets[b]; cur != DEDUP_KS_NIL; cur = ks->slots[cur].chain_next)
    {
        if(ks->slots[cur].key == key)
        {
            ks_lru_touch(ks, cur);
            return;
        }
    }

    /* Need a free slot. */
    uint32_t idx;
    if(ks->free_head != DEDUP_KS_NIL)
    {
        idx = ks->free_head;
        ks->free_head = ks->slots[idx].chain_next;
    }
    else
    {
        /* Evict LRU tail. */
        if(ks->lru_tail == DEDUP_KS_NIL) return; /* should never happen */
        idx = ks->lru_tail;
        /* Unlink from LRU. */
        uint32_t p = ks->slots[idx].lru_prev;
        if(p != DEDUP_KS_NIL) ks->slots[p].lru_next = DEDUP_KS_NIL;
        ks->lru_tail = p;
        if(ks->lru_head == idx) ks->lru_head = DEDUP_KS_NIL;
        /* Remove from its old hash chain. */
        ks_chain_remove(ks, idx);
        ks->count--;
    }

    /* Populate slot and insert at head of bucket chain. */
    ks->slots[idx].key        = key;
    ks->slots[idx].chain_next = ks->buckets[b];
    ks->buckets[b]            = idx;
    ks->count++;

    /* Insert at LRU head (MRU position). */
    ks->slots[idx].lru_prev = DEDUP_KS_NIL;
    ks->slots[idx].lru_next = ks->lru_head;
    if(ks->lru_head != DEDUP_KS_NIL)
        ks->slots[ks->lru_head].lru_prev = idx;
    ks->lru_head = idx;
    if(ks->lru_tail == DEDUP_KS_NIL) ks->lru_tail = idx;
}

/** Check if a key exists in the set (read-only, no LRU mutation). */
int keyset_contains(const struct dedup_key_set *ks, uint64_t key)
{
    if(!ks || key == KEYSET_EMPTY) return 0;

    uint32_t b = ks_bucket(key, ks->bucket_count);
    for(uint32_t cur = ks->buckets[b]; cur != DEDUP_KS_NIL; cur = ks->slots[cur].chain_next)
    {
        if(ks->slots[cur].key == key) return 1;
    }
    return 0;
}

/** Free the key set. */
void keyset_free(struct dedup_key_set *ks)
{
    if(!ks) return;
    free(ks->slots);
    free(ks->buckets);
    free(ks);
}

/**
 * Extract all dedup hash keys from a B+Tree leaf node buffer and
 * insert them into the key set.
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

/** Fibonacci-hashing for the pending buffer (open-addressing). */
static uint32_t pending_hash(uint64_t key, uint32_t mask)
{
    return (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
}

/** Allocate a pending insert buffer. */
struct dedup_pending_buf *pending_create(void)
{
    struct dedup_pending_buf *pb = calloc(1, sizeof(*pb));
    if(!pb) return NULL;
    pb->capacity = PENDING_INIT_CAP;
    pb->slots    = calloc(pb->capacity, sizeof(struct dedup_entry));
    if(!pb->slots)
    {
        free(pb);
        return NULL;
    }
    return pb;
}

/**
 * Allocate a pending insert buffer pre-sized for at least @p min_entries
 * entries without triggering any grow/rehash operations.
 *
 * The capacity is rounded up to the next power of two that keeps the
 * load factor below 75 %.  Falls back to PENDING_INIT_CAP when the
 * computed value would be smaller.
 */
struct dedup_pending_buf *pending_create_presized(uint64_t min_entries)
{
    /* Target capacity = ceil(min_entries / 0.75) rounded to next pow2. */
    uint64_t target = (min_entries * 4 + 2) / 3; /* ceil(n / 0.75) */
    if(target < PENDING_INIT_CAP) target = PENDING_INIT_CAP;

    /* Round up to next power of two. */
    uint32_t cap = PENDING_INIT_CAP;
    while((uint64_t)cap < target && cap < (UINT32_MAX / 2)) cap *= 2;

    struct dedup_pending_buf *pb = calloc(1, sizeof(*pb));
    if(!pb) return NULL;
    pb->capacity = cap;
    pb->slots    = calloc(cap, sizeof(struct dedup_entry));
    if(!pb->slots)
    {
        free(pb);
        return NULL;
    }
    return pb;
}

/** Grow the pending buffer (double capacity, re-insert all entries). */
static int pending_grow(struct dedup_pending_buf *pb)
{
    uint32_t            new_cap   = pb->capacity * 2;
    struct dedup_entry *new_slots = calloc(new_cap, sizeof(struct dedup_entry));
    if(!new_slots) return OBMAFS3_ERR_NOMEM;

    uint32_t new_mask = new_cap - 1;
    for(uint32_t i = 0; i < pb->capacity; i++)
    {
        if(pb->slots[i].hash == KEYSET_EMPTY) continue;
        uint32_t idx = pending_hash(pb->slots[i].hash, new_mask);
        for(uint32_t j = 0; j < new_cap; j++)
        {
            uint32_t s = (idx + j) & new_mask;
            if(new_slots[s].hash == KEYSET_EMPTY)
            {
                new_slots[s] = pb->slots[i];
                break;
            }
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
    uint32_t idx  = pending_hash(entry->hash, mask);
    for(uint32_t i = 0; i < pb->capacity; i++)
    {
        uint32_t s = (idx + i) & mask;
        if(pb->slots[s].hash == KEYSET_EMPTY)
        {
            pb->slots[s] = *entry;
            pb->count++;
            return;
        }
        if(pb->slots[s].hash == entry->hash) return; /* already present */
    }
}

/** Look up a hash in the pending buffer. Returns entry or NULL. */
const struct dedup_entry *pending_lookup(const struct dedup_pending_buf *pb, uint64_t hash)
{
    if(!pb || hash == KEYSET_EMPTY) return NULL;

    uint32_t mask = pb->capacity - 1;
    uint32_t idx  = pending_hash(hash, mask);
    for(uint32_t i = 0; i < pb->capacity; i++)
    {
        uint32_t s = (idx + i) & mask;
        if(pb->slots[s].hash == KEYSET_EMPTY) return NULL;
        if(pb->slots[s].hash == hash) return &pb->slots[s];
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
    if(ea->hash > eb->hash) return 1;
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
int pending_flush(struct dedup_pending_buf *pb, struct obmafs3_ctx *ctx, struct btree_header *dedup_hdr,
                  uint64_t dedup_hdr_lba)
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
        if(pb->slots[i].hash != KEYSET_EMPTY) sorted[n++] = pb->slots[i];
    }
    qsort(sorted, n, sizeof(struct dedup_entry), pending_entry_cmp);

    /* ---- Phase 2: find target leaf LBAs via cached index nodes ---- */

    uint8_t                 *tree_buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    struct dedup_node_cache *nc       = (struct dedup_node_cache *)ctx->dedup_node_cache;
    size_t                   bsz      = (size_t)ctx->sb.block_size;
    int                      rc       = OBMAFS3_OK;

    uint64_t *leaf_lbas = malloc((size_t)n * sizeof(uint64_t));
    if(!leaf_lbas)
    {
        free(sorted);
        return OBMAFS3_ERR_NOMEM;
    }

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
            prev                       = leaf_lbas[i];
        }
    }

    /* ---- Phase 3: posix_fadvise + pre-read all leaves ---- */

    /* Issue WILLNEED for all unique leaves (sorted = sequential). */
    for(uint32_t i = 0; i < unique_leaves; i++)
    {
        if(cache_find_slot(nc, leaf_lbas[i])) continue;
        posix_fadvise(ctx->fd, (off_t)(leaf_lbas[i] * bsz), (off_t)bsz, POSIX_FADV_WILLNEED);
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

    /* Flush any pre-existing dirty entries from the write path or a
     * prior operation, so the cache has room for new dirty nodes
     * created by splits during the insert loop. */
    if(nc && nc->dirty_count > 0)
    {
        rc = dedup_cache_flush(nc, ctx);
        if(rc != OBMAFS3_OK)
        {
            free(sorted);
            return rc;
        }
    }

    /* Flush periodically between inserts to keep the dirty set
     * small — prevents NOMEM when cache_evict_clean finds nothing
     * clean to reclaim.  Same interval as housekeeping_drain_batch. */
#define PENDING_FLUSH_INTERVAL 16
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
        else if(rc != OBMAFS3_OK) { break; /* I/O error */ }
        /* else: duplicate found — skip */

        if((i + 1) % PENDING_FLUSH_INTERVAL == 0 && nc->dirty_count > 0)
        {
            int frc = dedup_cache_flush(nc, ctx);
            if(frc != OBMAFS3_OK) { rc = frc; break; }
        }
    }
#undef PENDING_FLUSH_INTERVAL

    free(sorted);

    clock_gettime(CLOCK_MONOTONIC, &t_insert);

    /* ---- Phase 5: final flush of dirty cache nodes ---- */

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

    /* Flush any remaining dirty cache entries. */
    if(nc && nc->dirty_count > 0)
    {
        int nc_rc = dedup_cache_flush(nc, ctx);
        if(rc == OBMAFS3_OK) rc = nc_rc;
    }

    /* Always write the tree header — even on partial failure.
     * alloc_node may have popped nodes from the free list (updating
     * hdr->free_node_lba in memory) whose btree data was flushed to
     * disk above.  If we skip the header write, the on-disk
     * free_node_lba still points to those now-overwritten nodes,
     * corrupting the free list for the next reader. */
    {
        int hrc = obmafs3_btree_header_write(ctx, dedup_hdr_lba, dedup_hdr);
        if(rc == OBMAFS3_OK) rc = hrc;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    fprintf(stderr,
            "[dedup-pending] flushed %u entries (%u unique leaves, %u reads) "
            "prefetch=%.1fms insert=%.1fms flush=%.1fms TOTAL=%.1fms\n",
            n, unique_leaves, leaf_reads, timespec_diff_ms(&t_start, &t_prefetch),
            timespec_diff_ms(&t_prefetch, &t_insert), timespec_diff_ms(&t_insert, &t_end),
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
 * Initialise the global dedup B+Tree node cache in the filesystem context.
 * Called during obmafs3_open so that the read path can cache index nodes
 * and avoid repeated pread() syscalls for the same tree nodes.
 *
 * Also initialises the global dedup lookup cache (hash→dedup_entry)
 * which provides O(1) lookups for previously-seen hashes.
 *
 * Safe to call when a cache already exists (e.g. created by the write
 * or housekeeping paths) — in that case this is a no-op.
 */
void obmafs3_dedup_node_cache_init(struct obmafs3_ctx *ctx)
{
    if(!ctx) return;
    if(!ctx->dedup_node_cache)
    {
        struct dedup_node_cache *nc = dedup_cache_create((size_t)ctx->sb.block_size, ctx->cache_limit);
        if(nc) ctx->dedup_node_cache = nc;
    }
    if(!ctx->dedup_lookup_cache)
    {
        struct dedup_lookup_cache *lc = dedup_lc_create();
        if(lc) ctx->dedup_lookup_cache = lc;
    }

    /* Populate the DLC from all dedup tree leaves so that subsequent
     * reads hit the in-memory cache instead of doing random leaf I/O. */
    dlc_warmup(ctx);
}

/**
 * Free the global dedup B+Tree node cache stored in the filesystem context.
 * Called during unmount (obmafs3_close) to release memory.
 */
void obmafs3_dedup_node_cache_free(struct obmafs3_ctx *ctx)
{
    if(!ctx) return;
    if(ctx->dedup_node_cache)
    {
        struct dedup_node_cache *nc       = (struct dedup_node_cache *)ctx->dedup_node_cache;
        /* Flush any dirty entries before releasing the cache. */
        int                      flush_rc = dedup_cache_flush(nc, ctx);
        if(flush_rc != OBMAFS3_OK)
            fprintf(stderr,
                    "WARNING: dedup node cache flush failed at unmount "
                    "(rc=%d) — %u dirty entries lost\n",
                    flush_rc, nc->dirty_count);
        dedup_cache_free(nc);
        ctx->dedup_node_cache = NULL;
    }
    if(ctx->dedup_lookup_cache)
    {
        dedup_lc_free((struct dedup_lookup_cache *)ctx->dedup_lookup_cache);
        ctx->dedup_lookup_cache = NULL;
    }
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

    const struct dedup_pending_buf *pb1 = (const struct dedup_pending_buf *)ctx->dedup_pending;
    const struct dedup_pending_buf *pb2 = (const struct dedup_pending_buf *)ctx->dedup_pending_draining;

    uint32_t total = 0;
    if(pb1) total += pb1->count;
    if(pb2) total += pb2->count;

    if(total > 0 && ctx->bitmap && ctx->fd >= 0)
    {
        int rc = obmafs3_dedup_pending_save(ctx);
        if(rc != OBMAFS3_OK) fprintf(stderr, "[dedup-pending] save failed (rc=%d) — %u entries lost\n", rc, total);
    }
    else if(total > 0) { fprintf(stderr, "[dedup-pending] %u entries lost (no bitmap or fd)\n", total); }

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

/* ------------------------------------------------------------------ */
/*  Global dedup lookup cache — LRU hash table                         */
/* ------------------------------------------------------------------ */

/** Fibonacci-hashing of a key to a bucket index. */
static uint32_t lc_bucket(uint64_t hash, uint32_t mask)
{
    return (uint32_t)((hash * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
}

/* ---- LRU list helpers (caller holds lc->lock) ---- */

/** Unlink node @p idx from the LRU doubly-linked list. */
static void lru_unlink(struct dedup_lookup_cache *lc, uint32_t idx)
{
    struct dedup_lc_node *n = &lc->nodes[idx];
    if(n->lru_prev != DEDUP_LC_NIL)
        lc->nodes[n->lru_prev].lru_next = n->lru_next;
    else
        lc->lru_head = n->lru_next;

    if(n->lru_next != DEDUP_LC_NIL)
        lc->nodes[n->lru_next].lru_prev = n->lru_prev;
    else
        lc->lru_tail = n->lru_prev;
}

/** Push node @p idx to the front (MRU position) of the LRU list. */
static void lru_push_front(struct dedup_lookup_cache *lc, uint32_t idx)
{
    struct dedup_lc_node *n = &lc->nodes[idx];
    n->lru_prev             = DEDUP_LC_NIL;
    n->lru_next             = lc->lru_head;
    if(lc->lru_head != DEDUP_LC_NIL) lc->nodes[lc->lru_head].lru_prev = idx;
    lc->lru_head = idx;
    if(lc->lru_tail == DEDUP_LC_NIL) lc->lru_tail = idx;
}

/* ---- Hash chain helpers (caller holds lc->lock) ---- */

/** Remove node @p idx from its hash bucket chain. */
static void chain_remove(struct dedup_lookup_cache *lc, uint32_t idx)
{
    struct dedup_lc_node *n  = &lc->nodes[idx];
    uint32_t              b  = lc_bucket(n->hash, DEDUP_LC_BUCKETS - 1);
    uint32_t             *pp = &lc->buckets[b];
    while(*pp != DEDUP_LC_NIL)
    {
        if(*pp == idx)
        {
            *pp = n->chain_next;
            return;
        }
        pp = &lc->nodes[*pp].chain_next;
    }
}

/**
 * Allocate and initialise the global dedup lookup cache (LRU).
 *
 * Allocates one contiguous node pool and one bucket array, then
 * threads all nodes into a free list.
 */
struct dedup_lookup_cache *dedup_lc_create(void)
{
    struct dedup_lookup_cache *lc = calloc(1, sizeof(*lc));
    if(!lc) return NULL;

    lc->capacity = DEDUP_LC_CAPACITY;
    lc->count    = 0;
    lc->lru_head = DEDUP_LC_NIL;
    lc->lru_tail = DEDUP_LC_NIL;

    lc->nodes = malloc((size_t)lc->capacity * sizeof(struct dedup_lc_node));
    if(!lc->nodes)
    {
        free(lc);
        return NULL;
    }

    lc->buckets = malloc((size_t)DEDUP_LC_BUCKETS * sizeof(uint32_t));
    if(!lc->buckets)
    {
        free(lc->nodes);
        free(lc);
        return NULL;
    }

    /* Initialise buckets to empty. */
    for(uint32_t i = 0; i < DEDUP_LC_BUCKETS; i++) lc->buckets[i] = DEDUP_LC_NIL;

    /* Thread all nodes into the free list via chain_next. */
    for(uint32_t i = 0; i < lc->capacity - 1; i++) lc->nodes[i].chain_next = i + 1;
    lc->nodes[lc->capacity - 1].chain_next = DEDUP_LC_NIL;
    lc->free_head                          = 0;

    pthread_mutex_init(&lc->lock, NULL);

    fprintf(stderr, "[dlc] LRU cache created: %u slots (%.0f MiB)\n", lc->capacity,
            ((double)lc->capacity * sizeof(struct dedup_lc_node) + (double)DEDUP_LC_BUCKETS * sizeof(uint32_t)) /
                (1024.0 * 1024.0));
    return lc;
}

/** Free the global dedup lookup cache. */
void dedup_lc_free(struct dedup_lookup_cache *lc)
{
    if(!lc) return;
    pthread_mutex_destroy(&lc->lock);
    free(lc->nodes);
    free(lc->buckets);
    free(lc);
}

/**
 * Look up a (hash, tree_lba) pair in the LRU cache.
 * On hit, moves the entry to the MRU position.
 * Returns 1 if found (and fills @out), 0 if not found.
 */
int dedup_lc_get(struct dedup_lookup_cache *lc, uint64_t hash, uint64_t tree_lba, struct dedup_entry *out)
{
    pthread_mutex_lock(&lc->lock);
    uint32_t b   = lc_bucket(hash, DEDUP_LC_BUCKETS - 1);
    uint32_t idx = lc->buckets[b];
    while(idx != DEDUP_LC_NIL)
    {
        struct dedup_lc_node *n = &lc->nodes[idx];
        if(n->hash == hash && n->tree_lba == tree_lba)
        {
            out->hash         = n->hash;
            out->block_lba    = n->block_lba;
            out->block_offset = n->block_offset;
            /* Move to MRU position. */
            lru_unlink(lc, idx);
            lru_push_front(lc, idx);
            pthread_mutex_unlock(&lc->lock);
            return 1;
        }
        idx = n->chain_next;
    }
    pthread_mutex_unlock(&lc->lock);
    return 0;
}

/**
 * Insert a (hash, tree_lba) → dedup_entry into the LRU cache.
 *
 * If the entry already exists, it is moved to MRU position.
 * If the cache is full, the LRU entry is evicted first.
 */
void dedup_lc_put(struct dedup_lookup_cache *lc, uint64_t hash, uint64_t tree_lba, const struct dedup_entry *entry)
{
    pthread_mutex_lock(&lc->lock);

    uint32_t b = lc_bucket(hash, DEDUP_LC_BUCKETS - 1);

    /* Check if already present. */
    uint32_t idx = lc->buckets[b];
    while(idx != DEDUP_LC_NIL)
    {
        struct dedup_lc_node *n = &lc->nodes[idx];
        if(n->hash == hash && n->tree_lba == tree_lba)
        {
            /* Already cached — just promote to MRU. */
            lru_unlink(lc, idx);
            lru_push_front(lc, idx);
            pthread_mutex_unlock(&lc->lock);
            return;
        }
        idx = n->chain_next;
    }

    /* Need a free node.  If the free list is empty, evict LRU tail. */
    if(lc->free_head == DEDUP_LC_NIL)
    {
        uint32_t victim = lc->lru_tail;
        /* Remove victim from LRU list. */
        lru_unlink(lc, victim);
        /* Remove victim from its hash bucket chain. */
        chain_remove(lc, victim);
        /* Return victim to the free list. */
        lc->nodes[victim].chain_next = lc->free_head;
        lc->free_head                = victim;
        lc->count--;
    }

    /* Pop a node from the free list. */
    uint32_t new_idx = lc->free_head;
    lc->free_head    = lc->nodes[new_idx].chain_next;

    /* Populate the node. */
    struct dedup_lc_node *n = &lc->nodes[new_idx];
    n->hash                 = hash;
    n->tree_lba             = tree_lba;
    n->block_lba            = entry->block_lba;
    n->block_offset         = entry->block_offset;

    /* Insert into hash bucket chain. */
    n->chain_next  = lc->buckets[b];
    lc->buckets[b] = new_idx;

    /* Push to MRU position. */
    lru_push_front(lc, new_idx);

    lc->count++;
    pthread_mutex_unlock(&lc->lock);
}
