// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_housekeeping.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 deduplication housekeeping operations.
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
/*  Background housekeeping thread (pending → B+Tree drain)            */
/* ------------------------------------------------------------------ */

/** Maximum entries per housekeeping batch.
 *  Kept small so the tree_lock is held for only a few milliseconds
 *  per batch — avoiding long stalls on the write path. */
#define HOUSEKEEPING_BATCH_SIZE 128

/** Sleep interval (seconds) when idle. */
#define HOUSEKEEPING_IDLE_SEC 5

/** Pause between batches (microseconds) to yield I/O to writes. */
#define HOUSEKEEPING_YIELD_US 25000 /* 50 ms */

/**
 * Lock-free B+Tree traversal to find the leaf LBA for a given hash.
 *
 * Uses direct pread() — does NOT touch the shared node cache.
 * Safe to call without tree_lock held because:
 *  - In steady state only the housekeeping thread modifies the tree
 *    (the write path uses the deferred pending-buffer insert).
 *  - Even if a concurrent split rearranges nodes, we only use the
 *    result to warm the kernel page cache; the actual insert under
 *    tree_lock re-traverses via the node cache.
 */
static int dedup_find_leaf_lba_direct(int fd, uint64_t root_lba, uint64_t hash, uint64_t *out_leaf_lba, uint8_t *buf,
                                      size_t bsz)
{
    uint64_t lba = root_lba;
    if(lba == 0)
    {
        *out_leaf_lba = 0;
        return OBMAFS3_OK;
    }

    while(1)
    {
        ssize_t rd = pread(fd, buf, bsz, (off_t)(lba * bsz));
        if(rd != (ssize_t)bsz) return OBMAFS3_ERR_IO;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.level == 0)
        {
            *out_leaf_lba = lba;
            return OBMAFS3_OK;
        }

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

        if(nhdr.level == 1)
        {
            *out_leaf_lba = ie.child_lba;
            return OBMAFS3_OK;
        }

        lba = ie.child_lba;
    }
}

/**
 * Pre-warm the kernel page cache for a batch of entries.
 *
 * Called WITHOUT tree_lock.  Walks the B+Tree using direct pread()
 * to find target leaf LBAs, issues posix_fadvise(WILLNEED), then
 * reads them into a throwaway buffer so the data sits in the page
 * cache.  The subsequent drain_batch under tree_lock will find these
 * pages already present and avoid blocking disk I/O.
 *
 * @param ctx       Filesystem context (only ctx->fd and ctx->sb used).
 * @param root_lba  Current root node of the dedup tree.
 * @param entries   Sorted hash entries.
 * @param count     Number of entries.
 */
static void housekeeping_prefetch_batch(struct obmafs3_ctx *ctx, uint64_t root_lba, const struct dedup_entry *entries,
                                        uint32_t count)
{
    if(count == 0 || root_lba == 0) return;

    size_t bsz = (size_t)ctx->sb.block_size;

    /* Private traversal buffer — not shared with any other thread. */
    uint8_t *buf = malloc(bsz);
    if(!buf) return;

    uint64_t *leaf_lbas = malloc((size_t)count * sizeof(uint64_t));
    if(!leaf_lbas)
    {
        free(buf);
        return;
    }

    for(uint32_t i = 0; i < count; i++)
    {
        uint64_t leaf_lba = 0;
        dedup_find_leaf_lba_direct(ctx->fd, root_lba, entries[i].hash, &leaf_lba, buf, bsz);
        leaf_lbas[i] = leaf_lba;
    }

    /* Sort + dedup for sequential I/O. */
    qsort(leaf_lbas, count, sizeof(uint64_t), lba_cmp);

    uint32_t unique = 0;
    {
        uint64_t prev = 0;
        for(uint32_t i = 0; i < count; i++)
        {
            if(leaf_lbas[i] == 0 || leaf_lbas[i] == prev) continue;
            leaf_lbas[unique++] = leaf_lbas[i];
            prev                = leaf_lbas[i];
        }
    }

    /* Advise + pre-read into page cache. */
    for(uint32_t i = 0; i < unique; i++)
        posix_fadvise(ctx->fd, (off_t)(leaf_lbas[i] * bsz), (off_t)bsz, POSIX_FADV_WILLNEED);
    for(uint32_t i = 0; i < unique; i++) pread(ctx->fd, buf, bsz, (off_t)(leaf_lbas[i] * bsz));

    free(leaf_lbas);
    free(buf);
}

/**
 * Drain a batch of entries from the draining buffer into the B+Tree.
 *
 * Assumes the caller has already called housekeeping_prefetch_batch()
 * WITHOUT the lock so that relevant tree nodes are in the kernel page
 * cache.  This function only does the actual B+Tree inserts (using
 * the node cache) and must be called under tree_lock.
 *
 * @param ctx      Filesystem context.
 * @param entries  Sorted array of entries to insert.
 * @param count    Number of entries.
 * @param hdr      B+Tree header (updated in place on splits).
 * @param hdr_lba  LBA where the header lives.
 * @return @c OBMAFS3_OK on success.
 */
static int housekeeping_drain_batch(struct obmafs3_ctx *ctx, struct dedup_entry *entries, uint32_t count,
                                    struct btree_header *hdr, uint64_t hdr_lba)
{
    if(count == 0) return OBMAFS3_OK;

    uint8_t                 *tree_buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    struct dedup_node_cache *nc       = (struct dedup_node_cache *)ctx->dedup_node_cache;

    /* Flush any pre-existing dirty entries left by the write path or
     * a prior interrupted drain.  Without this, the cache can be full
     * of dirty nodes before we even start inserting, causing the very
     * first insert to fail with NOMEM when cache_evict_clean() finds
     * nothing clean to reclaim. */
    {
        int prc = dedup_cache_flush(nc, ctx);
        if(prc != OBMAFS3_OK) return prc;
    }

    /* Insert all entries — tree nodes should be in the kernel page
     * cache thanks to housekeeping_prefetch_batch(), so nc_block_read
     * cache-miss pread() calls will be served from RAM.
     *
     * We flush the node cache every DRAIN_FLUSH_INTERVAL inserts to
     * keep the dirty-entry high-water mark bounded.  Flushing here —
     * between complete inserts — is safe because each upsert_insert
     * writes a self-consistent set of nodes (leaf, siblings, parents,
     * possibly a new root); by the time we reach this point all nodes
     * from the previous insert form a valid tree state on disk.
     * Flushing mid-insert (e.g. inside cache_evict_clean) would NOT
     * be safe because a partially-written split could hit disk. */
#define DRAIN_FLUSH_INTERVAL 16
    for(uint32_t i = 0; i < count; i++)
    {
        struct dedup_entry      existing;
        struct dedup_upsert_ctx uctx;
        int                     rc = dedup_upsert_find(ctx, hdr, entries[i].hash, &existing, &uctx, tree_buf, nc);
        if(rc == OBMAFS3_ERR_NOTFOUND)
        {
            rc = dedup_upsert_insert(ctx, hdr, &entries[i], &uctx, tree_buf, nc);
            if(rc != OBMAFS3_OK) return rc;
        }
        else if(rc != OBMAFS3_OK)
        {
            /* Propagate I/O or other errors — do not silently drop
             * entries.  The caller will preserve the draining buffer
             * so these entries survive for the next mount. */
            return rc;
        }

        /* Periodic mid-batch flush: write all dirty nodes to disk and
         * mark them clean so cache_evict_clean() can reclaim them if
         * the node cache approaches max_capacity.  This prevents the
         * NOMEM failure that occurs when the entire cache is dirty and
         * eviction finds nothing to free. */
        if((i + 1) % DRAIN_FLUSH_INTERVAL == 0)
        {
            int frc = dedup_cache_flush(nc, ctx);
            if(frc != OBMAFS3_OK) return frc;
        }
    }
#undef DRAIN_FLUSH_INTERVAL

    /* Final flush for any remaining dirty entries. */
    int flush_rc = dedup_cache_flush(nc, ctx);
    if(flush_rc != OBMAFS3_OK) return flush_rc;

    /* Write updated header. */
    return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
}

/**
 * Background housekeeping thread entry point.
 *
 * Waits for warmup to complete, then continuously drains the pending
 * buffer into the B+Tree in small batches.  Each batch acquires
 * tree_lock for the actual B+Tree inserts and releases it between
 * batches so the write path is never blocked for long.
 *
 * The thread swaps the active pending buffer to a "draining" pointer
 * (visible to the read path for lookups) and processes it.  When
 * draining completes, the draining pointer is freed.
 */
static void *housekeeping_thread_func(void *arg)
{
    struct obmafs3_ctx *ctx = (struct obmafs3_ctx *)arg;

    /* Wait for warmup to finish before starting drain work. */
    obmafs3_dedup_warmup_wait(ctx);

    fprintf(stderr, "[housekeeping] started\n");

    while(!ctx->shutdown_requested)
    {
        /* --- check for work --- */
        int have_work = 0;
        pthread_rwlock_wrlock(&ctx->tree_lock);
        {
            struct dedup_pending_buf *pb = (struct dedup_pending_buf *)ctx->dedup_pending;
            /* Start draining when active buffer has entries and no drain
             * is already in progress. */
            if(pb && pb->count > 0 && !ctx->dedup_pending_draining)
            {
                /* Swap: move active buffer to draining, create fresh one. */
                ctx->dedup_pending_draining = pb;
                ctx->dedup_pending          = pending_create();
                if(ctx->dedup_pending)
                {
                    /* Preserve sector_size for new buffer. */
                    ((struct dedup_pending_buf *)ctx->dedup_pending)->sector_size = pb->sector_size;
                }
                have_work = 1;
            }
            else if(ctx->dedup_pending_draining)
            {
                /* Previous drain cycle not yet consumed — shouldn't
                 * happen but guard anyway. */
                have_work = 1;
            }
        }
        pthread_rwlock_unlock(&ctx->tree_lock);

        if(!have_work)
        {
            /* Sleep until woken or timeout. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += HOUSEKEEPING_IDLE_SEC;
            pthread_mutex_lock(&ctx->housekeeping_mutex);
            if(!ctx->shutdown_requested) pthread_cond_timedwait(&ctx->housekeeping_cond, &ctx->housekeeping_mutex, &ts);
            pthread_mutex_unlock(&ctx->housekeeping_mutex);
            continue;
        }

        /* --- drain the buffer in batches --- */
        struct dedup_pending_buf *drain = (struct dedup_pending_buf *)ctx->dedup_pending_draining;
        struct dedup_entry       *sorted         = NULL;
        uint32_t                  extracted       = 0;
        uint32_t                  total_inserted  = 0;

        if(!drain || drain->count == 0) goto finish_drain;

        /* Extract all entries and sort by hash (no lock needed —
         * only this thread touches the draining buffer). */
        uint32_t n = drain->count;
        sorted = malloc((size_t)n * sizeof(struct dedup_entry));
        if(!sorted) goto finish_drain;

        for(uint32_t i = 0; i < drain->capacity && extracted < n; i++)
        {
            if(drain->slots[i].hash != KEYSET_EMPTY) sorted[extracted++] = drain->slots[i];
        }
        qsort(sorted, extracted, sizeof(struct dedup_entry), pending_entry_cmp);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        uint32_t next_progress_msg = 100000;
        for(uint32_t off = 0; off < extracted && !ctx->shutdown_requested; off += HOUSEKEEPING_BATCH_SIZE)
        {
            uint32_t batch = extracted - off;
            if(batch > HOUSEKEEPING_BATCH_SIZE) batch = HOUSEKEEPING_BATCH_SIZE;

            /* Phase A — snapshot root LBA under a brief lock. */
            uint64_t root_lba = 0;
            {
                pthread_rwlock_wrlock(&ctx->tree_lock);
                struct btree_header hdr_snap;
                uint64_t            hdr_lba_snap;
                int                 rc = obmafs3_dedup_get_tree(ctx, drain->sector_size, &hdr_snap, &hdr_lba_snap);
                if(rc == OBMAFS3_OK) root_lba = hdr_snap.root_node_lba;
                pthread_rwlock_unlock(&ctx->tree_lock);
            }

            /* Phase B — prefetch WITHOUT lock (direct pread). */
            if(root_lba != 0) housekeeping_prefetch_batch(ctx, root_lba, sorted + off, batch);

            /* Phase C — insert under lock (page cache should be warm). */
            int drain_failed = 0;
            pthread_rwlock_wrlock(&ctx->tree_lock);
            {
                struct btree_header hdr;
                uint64_t            hdr_lba;
                int                 rc = obmafs3_dedup_get_tree(ctx, drain->sector_size, &hdr, &hdr_lba);
                if(rc == OBMAFS3_OK)
                {
                    rc = housekeeping_drain_batch(ctx, sorted + off, batch, &hdr, hdr_lba);
                    if(rc == OBMAFS3_OK)
                    {
                        total_inserted += batch;
                        if(total_inserted >= next_progress_msg)
                        {
                            struct timespec tnow;
                            clock_gettime(CLOCK_MONOTONIC, &tnow);
                            double elapsed = (tnow.tv_sec - t0.tv_sec) * 1000.0 + (tnow.tv_nsec - t0.tv_nsec) / 1e6;
                            fprintf(stderr,
                                    "[housekeeping] progress: %u/%u entries drained (%.1f ms)\n",
                                    total_inserted, extracted, elapsed);
                            next_progress_msg = (total_inserted / 100000 + 1) * 100000;
                        }
                    }
                    else
                    {
                        fprintf(stderr,
                                "[housekeeping] drain batch failed "
                                "(rc=%d) — stopping drain cycle\n",
                                rc);
                        drain_failed = 1;
                    }
                }
                else
                {
                    drain_failed = 1;
                }
            }
            pthread_rwlock_unlock(&ctx->tree_lock);

            /* Stop processing further batches — the tree's on-disk
             * state is behind the in-cache state after a flush failure.
             * Re-reading the header for the next batch would use a
             * stale root.  The draining buffer is preserved for retry. */
            if(drain_failed) break;

            /* Yield I/O to the write path between batches. */
            if(off + HOUSEKEEPING_BATCH_SIZE < extracted && !ctx->shutdown_requested) usleep(HOUSEKEEPING_YIELD_US);
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr, "[housekeeping] drained %u/%u entries in %.1f ms\n", total_inserted, extracted, ms);

    finish_drain:
        /* Ensure all B+Tree writes from the drain are durable before
         * we potentially trim the draining buffer.  Without this
         * fsync, a power failure could lose tree writes while the
         * trimmed (smaller) pending buffer survives — those entries
         * would be gone from both places. */
        if(total_inserted > 0) fsync(ctx->fd);

        /* Replace the draining buffer based on how far we got.
         * When all entries were drained, just free the buffer.
         * When only a prefix was drained (shutdown or error), build
         * a new smaller pending buffer containing only the remaining
         * entries so that persistence doesn't waste disk and the next
         * mount doesn't redundantly re-drain already-inserted entries. */
        if(total_inserted >= extracted)
        {
            /* Complete drain — discard the draining buffer. */
            free(sorted);
            pthread_rwlock_wrlock(&ctx->tree_lock);
            {
                struct dedup_pending_buf *old = (struct dedup_pending_buf *)ctx->dedup_pending_draining;
                ctx->dedup_pending_draining   = NULL;
                pending_free(old);
            }
            pthread_rwlock_unlock(&ctx->tree_lock);
        }
        else
        {
            /* Partial drain — build a trimmed buffer from the
             * remaining (not-yet-inserted) entries in the sorted
             * array and swap it in as the draining buffer. */
            uint32_t remaining = extracted - total_inserted;

            fprintf(stderr,
                    "[housekeeping] drain incomplete (%u/%u) — "
                    "building trimmed buffer with %u remaining entries\n",
                    total_inserted, extracted, remaining);

            struct dedup_pending_buf *trimmed = pending_create_presized((uint64_t)remaining);
            if(trimmed)
            {
                trimmed->sector_size = drain->sector_size;
                for(uint32_t i = total_inserted; i < extracted; i++)
                    pending_insert(trimmed, &sorted[i]);

                pthread_rwlock_wrlock(&ctx->tree_lock);
                {
                    struct dedup_pending_buf *old = (struct dedup_pending_buf *)ctx->dedup_pending_draining;
                    ctx->dedup_pending_draining   = trimmed;
                    pending_free(old);
                }
                pthread_rwlock_unlock(&ctx->tree_lock);

                fprintf(stderr,
                        "[housekeeping] trimmed draining buffer: "
                        "%u → %u entries for persistence\n",
                        extracted, trimmed->count);
            }
            else
            {
                /* Could not allocate trimmed buffer — fall back to
                 * preserving the original draining buffer as before.
                 * Re-draining already-inserted entries on the next
                 * mount is safe because dedup_upsert_find skips
                 * duplicates. */
                fprintf(stderr,
                        "[housekeeping] could not allocate trimmed buffer — "
                        "preserving original draining buffer (%u entries)\n",
                        extracted);
            }
            free(sorted);
        }
    }

    fprintf(stderr, "[housekeeping] stopped\n");
    return NULL;
}

/**
 * Start the background housekeeping thread.
 *
 * Should be called after warmup has been started — the housekeeping
 * thread will internally wait for warmup completion before doing any
 * drain work.
 */
void obmafs3_housekeeping_start(struct obmafs3_ctx *ctx)
{
    if(!ctx) return;

    pthread_mutex_init(&ctx->housekeeping_mutex, NULL);
    pthread_cond_init(&ctx->housekeeping_cond, NULL);
    ctx->housekeeping_started = 0;

    int rc = pthread_create(&ctx->housekeeping_thread, NULL, housekeeping_thread_func, ctx);
    if(rc != 0)
    {
        fprintf(stderr, "[housekeeping] failed to create thread (rc=%d)\n", rc);
        return;
    }
    ctx->housekeeping_started = 1;
}

/**
 * Signal the housekeeping thread to stop and wait for it to exit.
 */
void obmafs3_housekeeping_stop(struct obmafs3_ctx *ctx)
{
    if(!ctx || !ctx->housekeeping_started) return;

    /* Wake the thread if it's sleeping. */
    pthread_mutex_lock(&ctx->housekeeping_mutex);
    pthread_cond_signal(&ctx->housekeeping_cond);
    pthread_mutex_unlock(&ctx->housekeeping_mutex);

    pthread_join(ctx->housekeeping_thread, NULL);
    pthread_mutex_destroy(&ctx->housekeeping_mutex);
    pthread_cond_destroy(&ctx->housekeeping_cond);
    ctx->housekeeping_started = 0;
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
        struct dedup_node_cache *nc = dedup_cache_create((size_t)ctx->sb.block_size, ctx->cache_limit);
        if(nc) ctx->dedup_node_cache = nc;
    }

    /* Try to load the persisted key set from disk (fast path).
     * If keyset_lba is 0 the FS has no persisted keyset yet — treat as
     * stale and fall through to the full tree scan. */
    int loaded = 0;
    if(ctx->sb.keyset_lba != 0)
    {
        int lrc = obmafs3_dedup_keyset_load(ctx);
        if(lrc == OBMAFS3_OK)
        {
            loaded                   = 1;
            struct dedup_key_set *ks = (struct dedup_key_set *)ctx->dedup_key_set;
            clock_gettime(CLOCK_MONOTONIC, &t_end);
            fprintf(stderr, "[dedup-warmup] loaded persisted key set in %.1fms — %u keys\n",
                    timespec_diff_ms(&t_start, &t_end), ks->count);
        }
        else
        {
            fprintf(stderr, "[dedup-warmup] persisted keyset stale/invalid — rebuilding via tree scan\n");
        }
    }
    else
    {
        fprintf(stderr, "[dedup-warmup] no persisted keyset (keyset_lba=0) — building via tree scan\n");
    }

    if(!loaded)
    {
        /* Full tree scan (slow path). */
        struct dedup_key_set *ks = keyset_create();
        if(!ks) goto done;
        ctx->dedup_key_set = ks;

        /* Seed from any already-cached nodes (may be 0). */
        keyset_seed_from_cache(ks, (const struct dedup_node_cache *)ctx->dedup_node_cache);

        /* Read the dedup tree list and warm up every tree. */
        struct tree_list_header list_hdr;
        struct tree_list_entry *entries = NULL;
        uint64_t                count   = 0;
        int                     rc      = dedup_tree_list_read(ctx, &list_hdr, &entries, &count);
        if(rc == OBMAFS3_OK && count > 0)
        {
            uint8_t *buf = malloc((size_t)ctx->sb.block_size);
            if(buf)
            {
                for(uint64_t i = 0; i < count; i++)
                {
                    if(ctx->shutdown_requested) break;
                    struct btree_header hdr;
                    rc = obmafs3_btree_header_read(ctx, entries[i].tree_lba, &hdr);
                    if(rc == OBMAFS3_OK && hdr.root_node_lba != 0)
                    {
                        keyset_warmup(ctx, &hdr, buf, (struct dedup_node_cache *)ctx->dedup_node_cache);
                    }
                }
                free(buf);
            }
            free(entries);
        }

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        fprintf(stderr, "[dedup-warmup] background warmup completed in %.1fms — key set has %u keys\n",
                timespec_diff_ms(&t_start, &t_end), ks->count);
    }

done:
    /* Try to load persisted pending entries from a previous mount.
     * This creates the pending buffer and populates it from disk.
     * Also inserts loaded hashes into the keyset. */
    if(!ctx->dedup_pending && ctx->sb.pending_lba != 0)
    {
        int prc = obmafs3_dedup_pending_load(ctx, loaded);
        if(prc == OBMAFS3_OK)
        {
            struct dedup_pending_buf *lpb = (struct dedup_pending_buf *)ctx->dedup_pending;
            fprintf(stderr, "[dedup-warmup] loaded persisted pending buffer — %u entries\n", lpb ? lpb->count : 0);
        }
        else
        {
            fprintf(stderr, "[dedup-warmup] failed to load persisted pending (rc=%d)\n", prc);
        }
    }

    /* Create the pending insert buffer if not loaded from disk. */
    if(!ctx->dedup_pending)
    {
        struct dedup_pending_buf *pb = pending_create();
        if(pb) ctx->dedup_pending = pb;
    }

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
    while(!ctx->warmup_done) pthread_cond_wait(&ctx->warmup_cond, &ctx->warmup_mutex);
    pthread_mutex_unlock(&ctx->warmup_mutex);
}

/* ------------------------------------------------------------------ */
/*  Background compression start / stop                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Sector map cache flush / free                                      */
