// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : block.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 block I/O, compression, and file data reading/writing.
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

#include "debug.h"
#include "obmafs.h"

#include <LzmaLib.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

/* ------------------------------------------------------------------ */
/*  Parallel compression infrastructure                                */
/* ------------------------------------------------------------------ */

/* Forward declarations for functions defined later in this file */
static int compression_worthwhile(ZSTD_CCtx *cctx, const void *src, size_t src_size, void *dst, size_t dst_cap,
                                  int level);
int        obmafs3_compress(ZSTD_CCtx *cctx, const void *src, size_t src_size, void *dst, size_t *dst_size, int level);

/** Per-group compression job – filled by the caller, executed by a worker. */
struct compress_job
{
    /* Input — set by caller before launching workers */
    const uint8_t *group_data;  ///< pointer to assembled group data
    size_t         grp_bytes;   ///< input size in bytes
    uint64_t       grp_count;   ///< logical blocks in this group
    int            level;       ///< compression level (ZSTD or LZMA)
    uint64_t       block_size;  ///< filesystem block size
    uint8_t        algo;        ///< compression algorithm (kCompressionZstd or kCompressionLzma)
    uint32_t       lzma_dict;   ///< LZMA dictionary size (only used when algo == kCompressionLzma)

    /* Per-job resources — allocated by caller */
    uint8_t *comp_buf;       ///< output buffer (must be large enough)
    size_t   comp_buf_size;  ///< capacity of comp_buf

    /* Results — set by the worker */
    int      use_compressed;   ///< non-zero if compressed output should be used
    uint64_t phys_needed;      ///< number of physical blocks for compressed output
    size_t   compressed_size;  ///< compressed data size (excl. header)
};

/** Shared batch context for the worker threads. */
struct compress_batch
{
    struct compress_job *jobs;
    int                  total;
    atomic_int           next;  ///< next job index to claim
    atomic_int           done;  ///< completed job count
};

/**
 * Worker function – grabs jobs from the batch via atomic fetch-add,
 * uses its own ZSTD contexts, and compresses until no jobs remain.
 *
 * Two separate ZSTD contexts are used: @p probe_cctx for the fast
 * level-1 compressibility probe and @p cctx for the real compression.
 * Sharing a single context between the two causes a stale-window
 * assertion in ZSTD's btopt match finder (ZSTD debug builds).
 */
static int compress_worker_run(struct compress_batch *batch, ZSTD_CCtx *probe_cctx, ZSTD_CCtx *cctx)
{
    int idx, processed = 0;
    while((idx = atomic_fetch_add(&batch->next, 1)) < batch->total)
    {
        struct compress_job *job = &batch->jobs[idx];
        job->use_compressed      = 0;
        processed++; /* count every claimed job for batch completion tracking */

        /* Skip groups that weren't set up for compression (e.g. partial tail groups) */
        if(!job->comp_buf || job->grp_bytes == 0) continue;

        /* Fast probe: skip expensive levels if data is incompressible */
        int worth = compression_worthwhile(probe_cctx, job->group_data, job->grp_bytes,
                                           job->comp_buf + sizeof(struct block_header),
                                           job->comp_buf_size - sizeof(struct block_header), job->level);
        if(!worth) continue;

        size_t comp_size = job->comp_buf_size - sizeof(struct block_header);
        int    rc;
        if(job->algo == kCompressionLzma)
            rc = obmafs3_compress_lzma(job->group_data, job->grp_bytes,
                                       job->comp_buf + sizeof(struct block_header),
                                       &comp_size, job->level, job->lzma_dict);
        else
            rc = obmafs3_compress(cctx, job->group_data, job->grp_bytes,
                                  job->comp_buf + sizeof(struct block_header),
                                  &comp_size, job->level);
        if(rc != OBMAFS3_OK) continue;

        uint64_t total_on_disk = sizeof(struct block_header) + comp_size;
        uint64_t phys_needed   = (total_on_disk + job->block_size - 1) / job->block_size;
        if(phys_needed >= job->grp_count) continue; /* not worth it */

        /* Build the on-disk block header + padding */
        struct block_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        bhdr.magic            = OBMAFS3_BLOCK_MAGIC;
        bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
        bhdr.compression_type = job->algo;
        bhdr.original_size    = job->grp_bytes;
        bhdr.compressed_size  = comp_size;
        obmafs3_checksum_block(job->comp_buf + sizeof(bhdr), comp_size, bhdr.checksum);
        memcpy(job->comp_buf, &bhdr, sizeof(bhdr));

        size_t used             = sizeof(bhdr) + comp_size;
        size_t total_phys_bytes = (size_t)(phys_needed * job->block_size);
        if(used < total_phys_bytes) memset(job->comp_buf + used, 0, total_phys_bytes - used);

        job->use_compressed  = 1;
        job->phys_needed     = phys_needed;
        job->compressed_size = comp_size;
    }
    return processed;
}

/* ------------------------------------------------------------------ */
/*  Persistent compression thread pool                                 */
/* ------------------------------------------------------------------ */

struct compress_pool
{
    pthread_t             *threads;       ///< worker thread array
    int                    num_threads;   ///< number of worker threads
    pthread_mutex_t        mutex;         ///< protects batch, shutdown, generation, async_queue
    pthread_cond_t         work_avail;    ///< signalled when a new batch or async job is ready
    pthread_cond_t         batch_done;    ///< signalled when all jobs complete
    struct compress_batch  batch_data;    ///< embedded batch (lives as long as the pool)
    int                    batch_active;  ///< non-zero when a batch is in progress
    int                    shutdown;      ///< non-zero → workers should exit
    unsigned int           generation;    ///< incremented for each batch submission
    struct pool_async_job *async_queue;   ///< FIFO of pending async jobs
};

/** Pool worker thread main loop. */
static void *pool_worker_main(void *arg)
{
    struct compress_pool *pool   = arg;
    unsigned int          my_gen = 0;

    /* Persistent ZSTD contexts – created once per worker thread,
     * reused across all batches, freed on pool shutdown. */
    ZSTD_CCtx *probe_cctx = ZSTD_createCCtx();
    ZSTD_CCtx *cctx       = ZSTD_createCCtx();

    for(;;)
    {
        pthread_mutex_lock(&pool->mutex);
        while(pool->generation == my_gen && !pool->async_queue && !pool->shutdown)
            pthread_cond_wait(&pool->work_avail, &pool->mutex);

        if(pool->shutdown)
        {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* Check for an async job first (dedup block compression) */
        struct pool_async_job *aj = pool->async_queue;
        if(aj)
        {
            pool->async_queue = aj->next;
            aj->next          = NULL;
            pthread_mutex_unlock(&pool->mutex);

            aj->result = aj->fn(aj->arg, cctx);
            /* Signal completion under the job's mutex BEFORE setting
             * done.  If we set done first, the waiter can see it, return,
             * and the caller can free the job — leaving us accessing
             * freed mutex/cond memory.  Setting done under the lock
             * guarantees the waiter cannot observe done=1 and proceed
             * to free the job until we've released the mutex. */
            pthread_mutex_lock(&aj->mtx);
            atomic_store(&aj->done, 1);
            pthread_cond_signal(&aj->cond);
            pthread_mutex_unlock(&aj->mtx);
            continue; /* back to top — don't update my_gen, may have batch too */
        }

        my_gen                           = pool->generation;
        int                    has_batch = pool->batch_active;
        struct compress_batch *batch     = has_batch ? &pool->batch_data : NULL;
        pthread_mutex_unlock(&pool->mutex);

        /* The submitter may have already completed the batch and set
         * batch_active = 0 before this worker woke up.  Skip. */
        if(!batch) continue;

        int n = 0;
        if(cctx && probe_cctx) n = compress_worker_run(batch, probe_cctx, cctx);

        /* Only participate in completion signalling if we did real work */
        if(n > 0)
        {
            int total_done = atomic_fetch_add(&batch->done, n) + n;
            if(total_done >= batch->total)
            {
                pthread_mutex_lock(&pool->mutex);
                pthread_cond_signal(&pool->batch_done);
                pthread_mutex_unlock(&pool->mutex);
            }
        }
    }

    if(probe_cctx) ZSTD_freeCCtx(probe_cctx);
    if(cctx) ZSTD_freeCCtx(cctx);
    return NULL;
}

/**
 * Initialise the persistent compression thread pool.
 * Creates min(nproc, 32) worker threads.
 */
int obmafs3_compress_pool_init(struct obmafs3_ctx *ctx)
{
    int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    int n     = ncpus;
    if(n > 32) n = 32;
    if(n < 1) n = 1;

    struct compress_pool *pool = calloc(1, sizeof(*pool));
    if(!pool) return OBMAFS3_ERR_NOMEM;

    pool->threads = malloc((size_t)n * sizeof(pthread_t));
    if(!pool->threads)
    {
        free(pool);
        return OBMAFS3_ERR_NOMEM;
    }

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->work_avail, NULL);
    pthread_cond_init(&pool->batch_done, NULL);
    pool->num_threads  = 0;
    pool->batch_active = 0;
    pool->shutdown     = 0;

    for(int i = 0; i < n; i++)
    {
        if(pthread_create(&pool->threads[i], NULL, pool_worker_main, pool) == 0)
            pool->num_threads++;
        else
            break;
    }

    if(pool->num_threads == 0)
    {
        /* No threads created — fall back to inline compression */
        free(pool->threads);
        pthread_mutex_destroy(&pool->mutex);
        pthread_cond_destroy(&pool->work_avail);
        pthread_cond_destroy(&pool->batch_done);
        free(pool);
        ctx->compress_pool = NULL;
        return OBMAFS3_OK; /* not fatal */
    }

    ctx->compress_pool = pool;
    return OBMAFS3_OK;
}

/**
 * Re-initialise the compression pool after a fork.
 *
 * When FUSE daemonises (no -f flag), it forks.  The child inherits the
 * pool struct but not the worker threads.  This function discards the
 * stale pool (without joining non-existent threads) and creates a
 * fresh one with live worker threads.
 */
void obmafs3_compress_pool_reinit(struct obmafs3_ctx *ctx)
{
    struct compress_pool *old = ctx->compress_pool;
    if(old)
    {
        pid_t now = getpid();
        if(now == ctx->pre_fuse_pid)
        {
            /* No fork happened (-f flag) — the old threads are still
             * alive.  Shut them down properly before re-creating. */
            obmafs3_compress_pool_destroy(ctx);
        }
        else
        {
            /* The old threads don't exist in this process — just free the
             * data structures.  Do NOT pthread_join (UB on ghost tids).
             * Do NOT pthread_mutex_destroy / pthread_cond_destroy — after
             * fork the primitives may be in an inconsistent state (e.g.
             * recorded waiters that no longer exist), causing the destroy
             * call to block indefinitely.  Simply leak and free. */
            free(old->threads);
            free(old);
            ctx->compress_pool = NULL;
        }
    }
    obmafs3_compress_pool_init(ctx);
}

/**
 * Shut down the compression thread pool and free resources.
 */
void obmafs3_compress_pool_destroy(struct obmafs3_ctx *ctx)
{
    struct compress_pool *pool = ctx->compress_pool;
    if(!pool) return;

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_avail);
    pthread_mutex_unlock(&pool->mutex);

    for(int i = 0; i < pool->num_threads; i++) pthread_join(pool->threads[i], NULL);

    free(pool->threads);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->work_avail);
    pthread_cond_destroy(&pool->batch_done);
    free(pool);
    ctx->compress_pool = NULL;
}

/**
 * Submit compression jobs to the pool and wait for completion.
 * Falls back to inline compression if the pool is NULL.
 *
 * The batch is stored inside the pool (pool->batch_data) so that
 * late-waking worker threads never access freed stack memory.
 */
static void compress_pool_submit(struct compress_pool *pool, struct compress_job *jobs, int total)
{
    if(!pool)
    {
        /* No pool — run inline with temporary contexts */
        struct compress_batch batch;
        batch.jobs  = jobs;
        batch.total = total;
        atomic_init(&batch.next, 0);
        atomic_init(&batch.done, 0);
        ZSTD_CCtx *probe_cctx = ZSTD_createCCtx();
        ZSTD_CCtx *cctx       = ZSTD_createCCtx();
        if(cctx && probe_cctx) { compress_worker_run(&batch, probe_cctx, cctx); }
        if(probe_cctx) ZSTD_freeCCtx(probe_cctx);
        if(cctx) ZSTD_freeCCtx(cctx);
        return;
    }

    /* Fill the pool-owned batch so workers always reference valid memory */
    struct compress_batch *batch = &pool->batch_data;
    batch->jobs                  = jobs;
    batch->total                 = total;
    atomic_init(&batch->next, 0);
    atomic_init(&batch->done, 0);

    pthread_mutex_lock(&pool->mutex);
    pool->batch_active = 1;
    pool->generation++;
    pthread_cond_broadcast(&pool->work_avail);
    pthread_mutex_unlock(&pool->mutex);

    /* The submitter thread participates in compression too, so even if
     * all pool workers are dead the batch still completes.  This also
     * improves throughput by utilising the caller instead of blocking. */
    {
        ZSTD_CCtx *probe_cctx = ZSTD_createCCtx();
        ZSTD_CCtx *cctx       = ZSTD_createCCtx();
        if(cctx && probe_cctx)
        {
            int n = compress_worker_run(batch, probe_cctx, cctx);
            if(n > 0)
            {
                int total_done = atomic_fetch_add(&batch->done, n) + n;
                if(total_done >= batch->total)
                {
                    pthread_mutex_lock(&pool->mutex);
                    pthread_cond_signal(&pool->batch_done);
                    pthread_mutex_unlock(&pool->mutex);
                }
            }
        }
        if(probe_cctx) ZSTD_freeCCtx(probe_cctx);
        if(cctx) ZSTD_freeCCtx(cctx);
    }

    /* Wait for all jobs to complete (may already be done) */
    pthread_mutex_lock(&pool->mutex);
    while(atomic_load(&batch->done) < batch->total) pthread_cond_wait(&pool->batch_done, &pool->mutex);

    pool->batch_active = 0;
    pthread_mutex_unlock(&pool->mutex);
}

/* ------------------------------------------------------------------ */
/*  Async job management for the compression pool                      */
/* ------------------------------------------------------------------ */

/**
 * Create a new async job structure.
 *
 * @param fn   Work function to execute (receives the opaque arg and a ZSTD context).
 * @param arg  Opaque context passed to @p fn.  Ownership is NOT taken;
 *             the caller must keep @p arg alive until the job completes.
 * @return     A new pool_async_job, or NULL on allocation failure.
 */
struct pool_async_job *obmafs3_pool_async_job_create(int (*fn)(void *arg, void *cctx), void *arg)
{
    struct pool_async_job *job = calloc(1, sizeof(*job));
    if(!job) return NULL;
    job->fn  = fn;
    job->arg = arg;
    atomic_init(&job->done, 0);
    pthread_mutex_init(&job->mtx, NULL);
    pthread_cond_init(&job->cond, NULL);
    return job;
}

/**
 * Free a completed async job.
 * The caller must have waited for the job first (obmafs3_pool_wait_async).
 */
void obmafs3_pool_async_job_free(struct pool_async_job *job)
{
    if(!job) return;
    pthread_cond_destroy(&job->cond);
    pthread_mutex_destroy(&job->mtx);
    free(job);
}

/**
 * Submit an async job to the pool.
 *
 * The job is added to the pool's async queue and will be picked up
 * by the next available worker thread.  If the pool is NULL the job
 * is executed inline on the calling thread using a temporary ZSTD
 * context (fallback for single-threaded builds).
 */
void obmafs3_pool_submit_async(struct compress_pool *pool, struct pool_async_job *job)
{
    if(!pool)
    {
        /* Inline fallback */
        ZSTD_CCtx *cctx = ZSTD_createCCtx();
        job->result     = job->fn(job->arg, cctx);
        if(cctx) ZSTD_freeCCtx(cctx);
        atomic_store(&job->done, 1);
        return;
    }

    pthread_mutex_lock(&pool->mutex);
    /* Append to queue tail to preserve FIFO order */
    job->next = NULL;
    if(!pool->async_queue) { pool->async_queue = job; }
    else
    {
        struct pool_async_job *tail = pool->async_queue;
        while(tail->next) tail = tail->next;
        tail->next = job;
    }
    pthread_cond_broadcast(&pool->work_avail);
    pthread_mutex_unlock(&pool->mutex);
}

/**
 * Wait for an async job to complete and return its result.
 */
int obmafs3_pool_wait_async(struct pool_async_job *job)
{
    if(!job) return OBMAFS3_OK;
    pthread_mutex_lock(&job->mtx);
    while(!atomic_load(&job->done)) pthread_cond_wait(&job->cond, &job->mtx);
    pthread_mutex_unlock(&job->mtx);
    return job->result;
}

/**
 * Compress data using ZSTD.
 *
 * @param cctx      Reusable ZSTD compression context.
 * @param src       Source data buffer.
 * @param src_size  Number of bytes in @p src.
 * @param dst       Destination buffer for compressed data.
 * @param dst_size  On input, capacity of @p dst; on output, compressed size.
 * @param level     ZSTD compression level (1–22).
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on error.
 */
int obmafs3_compress(ZSTD_CCtx *cctx, const void *src, size_t src_size, void *dst, size_t *dst_size, int level)
{
    size_t result = ZSTD_compressCCtx(cctx, dst, *dst_size, src, src_size, level);
    if(ZSTD_isError(result)) DBG_RETURN(OBMAFS3_ERR_IO, "I/O error");
    *dst_size = result;
    return OBMAFS3_OK;
}

/**
 * Decompress ZSTD-compressed data.
 *
 * @param dctx      Reusable ZSTD decompression context.
 * @param src       Compressed data buffer.
 * @param src_size  Number of compressed bytes.
 * @param dst       Output buffer for decompressed data.
 * @param dst_size  Capacity of @p dst (must be >= original size).
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on error.
 */
int obmafs3_decompress(ZSTD_DCtx *dctx, const void *src, size_t src_size, void *dst, size_t dst_size)
{
    size_t result = ZSTD_decompressDCtx(dctx, dst, dst_size, src, src_size);
    if(ZSTD_isError(result)) DBG_RETURN(OBMAFS3_ERR_IO, "I/O error");
    return OBMAFS3_OK;
}

/**
 * Fast compressibility probe.
 *
 * When a high ZSTD level (> 3) is configured, attempting compression on
 * incompressible data wastes significant CPU time.  This function does
 * a quick ZSTD level-1 probe on the data; if level 1 cannot achieve at
 * least 10 % size reduction, the data is almost certainly incompressible
 * at any level so the caller should skip the expensive attempt.
 *
 * @param cctx      Reusable ZSTD compression context.
 * @param src       Data to test.
 * @param src_size  Size of data in bytes.
 * @param dst       Scratch buffer (at least ZSTD_compressBound(src_size)).
 * @param dst_cap   Cap of @p dst.
 * @param level     Configured ZSTD level.
 * @return Non-zero if compression is worth attempting at the full level.
 */
static int compression_worthwhile(ZSTD_CCtx *cctx, const void *src, size_t src_size, void *dst, size_t dst_cap,
                                  int level)
{
    if(level <= 3) return 1; /* levels 1-3 are already fast enough */
    size_t result = ZSTD_compressCCtx(cctx, dst, dst_cap, src, src_size, 1);
    if(ZSTD_isError(result)) return 0;
    /* If level-1 can't reduce size by at least 10%, skip the expensive attempt */
    return result < (src_size * 9 / 10);
}

/* ---- LZMA compression/decompression ---- */

/**
 * Compress data using LZMA.
 *
 * The 5-byte LZMA properties header is prepended to the compressed output.
 * `*dst_size` on input must be the capacity of @p dst; on output it reflects
 * the total written (props + compressed payload).
 */
int obmafs3_compress_lzma(const void *src, size_t src_size, void *dst, size_t *dst_size, int level, uint32_t dict_size)
{
    if(*dst_size < LZMA_PROPS_SIZE) return OBMAFS3_ERR_IO;

    uint8_t *out     = (uint8_t *)dst;
    size_t   props_sz = LZMA_PROPS_SIZE;
    size_t   dest_len = *dst_size - LZMA_PROPS_SIZE;

    int rc = LzmaCompress(out + LZMA_PROPS_SIZE, &dest_len, (const unsigned char *)src, src_size,
                          out, &props_sz, level, dict_size, -1, -1, -1, -1, 1);

    if(rc != SZ_OK) return OBMAFS3_ERR_IO;

    *dst_size = LZMA_PROPS_SIZE + dest_len;
    return OBMAFS3_OK;
}

/**
 * Decompress LZMA-compressed data.
 *
 * Expects 5-byte LZMA properties header at the start of @p src.
 */
int obmafs3_decompress_lzma(const void *src, size_t src_size, void *dst, size_t dst_size)
{
    if(src_size < LZMA_PROPS_SIZE) return OBMAFS3_ERR_IO;

    const uint8_t *in       = (const uint8_t *)src;
    size_t          dest_len = dst_size;
    SizeT           src_len  = (SizeT)(src_size - LZMA_PROPS_SIZE);

    int rc = LzmaUncompress((unsigned char *)dst, &dest_len, in + LZMA_PROPS_SIZE, &src_len, in, LZMA_PROPS_SIZE);

    if(rc != SZ_OK) return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

/**
 * Dispatch compression to the configured algorithm.
 * Returns OBMAFS3_OK on success; *dst_size set to compressed output size.
 */
int obmafs3_compress_dispatch(struct obmafs3_ctx *ctx, const void *src, size_t src_size, void *dst, size_t *dst_size)
{
    if(ctx->compression_algo == kCompressionLzma)
        return obmafs3_compress_lzma(src, src_size, dst, dst_size, ctx->lzma_level, ctx->lzma_dict_size);
    return obmafs3_compress(obmafs3_get_thread_bufs(ctx)->zstd_cctx, src, src_size, dst, dst_size, ctx->zstd_level);
}

/**
 * Dispatch decompression based on the block header's compression_type.
 */
int obmafs3_decompress_dispatch(struct obmafs3_ctx *ctx, uint8_t compression_type, const void *src, size_t src_size,
                                void *dst, size_t dst_size)
{
    if(compression_type == kCompressionLzma)
        return obmafs3_decompress_lzma(src, src_size, dst, dst_size);
    return obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, src, src_size, dst, dst_size);
}

/* ------------------------------------------------------------------ */
/*  Overflow extent B+Tree helpers                                     */
/* ------------------------------------------------------------------ */

/** Compute and store the checksum for a btree node block. */
static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr      = (struct btree_node_header *)buf;
    size_t                    data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

/** Maximum overflow_extent records in a leaf node. */
static uint16_t overflow_leaf_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct overflow_extent));
}

/** Maximum overflow_index_entry entries in an index node. */
static uint16_t overflow_index_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header)) / sizeof(struct overflow_index_entry));
}

/**
 * Binary search for (inode_id, logical_offset) in an overflow leaf node.
 * Returns index (>= 0) if found, else -(insertion_point) - 1.
 */
static int overflow_leaf_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, uint64_t logical_offset)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;

    while(lo <= hi)
    {
        int                    mid = lo + (hi - lo) / 2;
        struct overflow_extent oe;
        memcpy(&oe, data + (size_t)mid * sizeof(oe), sizeof(oe));

        if(oe.inode_id < inode_id) { lo = mid + 1; }
        else if(oe.inode_id > inode_id) { hi = mid - 1; }
        else if(oe.logical_offset < logical_offset) { lo = mid + 1; }
        else if(oe.logical_offset > logical_offset) { hi = mid - 1; }
        else
        {
            return mid;
        }
    }

    return -(lo + 1);
}

/**
 * Binary search in an overflow index node for the child covering
 * (inode_id, logical_offset).  Uses the full composite key so that
 * files with many extents are correctly routed across multiple leaves.
 * Returns the slot index of the child pointer to follow.
 */
static uint16_t overflow_index_find(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id, uint64_t logical_offset)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int                         mid = lo + (hi - lo) / 2;
        struct overflow_index_entry ie;
        memcpy(&ie, data + (size_t)mid * sizeof(ie), sizeof(ie));

        if(ie.inode_id < inode_id || (ie.inode_id == inode_id && ie.logical_offset <= logical_offset))
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

/**
 * Binary search in an overflow index node for the LEFTMOST child
 * whose subtree may contain entries for @p inode_id.
 *
 * Because a leaf split can place entries for the same inode_id in
 * both the old (left) and new (right) sibling while pushing the
 * inode_id up as a separator key, we must start scanning from the
 * child that PRECEDES the first exact-match slot.  This is achieved
 * by using strict less-than: we return the rightmost slot whose key
 * is < inode_id (not <=).  The right-link scan will then visit any
 * subsequent leaves that also hold entries for the inode.
 */
static uint16_t overflow_index_find_left(const uint8_t *buf, uint16_t node_keys, uint64_t inode_id)
{
    const uint8_t *data = buf + sizeof(struct btree_node_header);
    int            lo = 0, hi = (int)node_keys - 1;
    uint16_t       result = 0;

    while(lo <= hi)
    {
        int                         mid = lo + (hi - lo) / 2;
        struct overflow_index_entry ie;
        memcpy(&ie, data + (size_t)mid * sizeof(ie), sizeof(ie));
        if(ie.inode_id < inode_id)
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

#define OVERFLOW_BTREE_MAX_DEPTH 8

struct overflow_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

/**
 * Insert an extent into the overflow B+Tree.
 * Entries are sorted by (inode_id, start_block).
 */
static int overflow_insert(struct obmafs3_ctx *ctx, const struct overflow_extent *entry)
{
    struct btree_header *hdr     = &ctx->overflow_hdr;
    uint64_t             hdr_lba = ctx->sb.overflow_lba;
    size_t               bsz     = (size_t)ctx->sb.block_size;
    int                  rc;

    /* ---- Empty tree: create a single leaf as root ---- */
    if(hdr->root_node_lba == 0)
    {
        uint64_t root_lba;
        rc = obmafs3_btree_alloc_node(ctx, hdr, hdr_lba, &root_lba);
        if(rc != OBMAFS3_OK) return rc;

        uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
        memset(buf, 0, bsz);

        struct btree_node_header nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
        nhdr.record_type = kBtreeDataTypeExtent;
        nhdr.level       = 0;
        nhdr.node_keys   = 1;
        nhdr.keys_length = (uint16_t)sizeof(struct overflow_extent);
        memcpy(buf, &nhdr, sizeof(nhdr));
        memcpy(buf + sizeof(nhdr), entry, sizeof(*entry));
        compute_node_checksum(buf);

        rc = obmafs3_block_write(ctx, root_lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        hdr->root_node_lba = root_lba;
        hdr->total_nodes   = 1;
        return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
    }

    /* ---- Traverse from root to leaf, recording path ---- */
    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct overflow_btree_path path[OVERFLOW_BTREE_MAX_DEPTH];
    int                        depth = 0;
    uint64_t                   lba   = hdr->root_node_lba;

    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) return rc;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        if(nhdr.level == 0) break; /* reached leaf */

        if(depth >= OVERFLOW_BTREE_MAX_DEPTH) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

        uint16_t slot    = overflow_index_find(buf, nhdr.node_keys, entry->inode_id, entry->logical_offset);
        path[depth].lba  = lba;
        path[depth].slot = slot;
        depth++;

        struct overflow_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* ---- buf holds the leaf node at lba ---- */
    struct btree_node_header leaf_hdr;
    memcpy(&leaf_hdr, buf, sizeof(leaf_hdr));

    int      idx      = overflow_leaf_find(buf, leaf_hdr.node_keys, entry->inode_id, entry->logical_offset);
    uint16_t max_leaf = overflow_leaf_max_keys(ctx);
    size_t   rec_sz   = sizeof(struct overflow_extent);

    /* Exact match — update the existing record in place (no duplicate) */
    if(idx >= 0)
    {
        uint8_t *data = buf + sizeof(struct btree_node_header);
        memcpy(data + (size_t)idx * rec_sz, entry, rec_sz);
        compute_node_checksum(buf);
        return obmafs3_block_write(ctx, lba, buf, bsz);
    }

    int insert_pos = -(idx + 1);

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

        rc = obmafs3_block_write(ctx, lba, buf, bsz);
        return rc;
    }

    /* ---- Leaf is full: split ---- */
    uint16_t                total = max_leaf + 1;
    struct overflow_extent *all   = calloc(total, rec_sz);
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
    rc = obmafs3_btree_alloc_node(ctx, hdr, hdr_lba, &new_leaf_lba);
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
    nh.record_type = kBtreeDataTypeExtent;
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

    uint64_t push_key       = all[left_count].inode_id;
    uint64_t push_offset    = all[left_count].logical_offset;
    uint64_t push_child     = new_leaf_lba;
    uint64_t left_first_key = all[0].inode_id;
    uint64_t left_first_off = all[0].logical_offset;
    uint64_t left_lba       = lba;

    free(all);
    hdr->total_nodes++;

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

        uint16_t max_idx    = overflow_index_max_keys(ctx);
        uint16_t idx_insert = parent_slot + 1;
        size_t   ie_sz      = sizeof(struct overflow_index_entry);

        if(phdr.node_keys < max_idx)
        {
            /* Room in parent — insert */
            uint8_t *id = buf + sizeof(struct btree_node_header);

            /* Update the key at parent_slot to the left child's
             * actual minimum.  Without this, the parent key can
             * be stale (higher than the true minimum) after the
             * leftmost child accumulated entries with keys below
             * the original index key. */
            struct overflow_index_entry oe_upd;
            memcpy(&oe_upd, id + (size_t)parent_slot * ie_sz, sizeof(oe_upd));
            oe_upd.inode_id       = left_first_key;
            oe_upd.logical_offset = left_first_off;
            memcpy(id + (size_t)parent_slot * ie_sz, &oe_upd, sizeof(oe_upd));

            if(idx_insert < phdr.node_keys)
                memmove(id + ((size_t)idx_insert + 1) * ie_sz, id + (size_t)idx_insert * ie_sz,
                        ((size_t)phdr.node_keys - (size_t)idx_insert) * ie_sz);

            struct overflow_index_entry ne;
            ne.inode_id       = push_key;
            ne.logical_offset = push_offset;
            ne.child_lba      = push_child;
            memcpy(id + (size_t)idx_insert * ie_sz, &ne, sizeof(ne));

            phdr.node_keys++;
            phdr.keys_length = (uint16_t)(phdr.node_keys * ie_sz);
            memcpy(buf, &phdr, sizeof(phdr));
            compute_node_checksum(buf);

            rc = obmafs3_block_write(ctx, parent_lba, buf, bsz);
            if(rc != OBMAFS3_OK) return rc;
            return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        }

        /* Parent is full — split the index node */
        uint16_t                     idx_total = max_idx + 1;
        struct overflow_index_entry *aie       = calloc(idx_total, ie_sz);
        if(!aie) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        uint8_t *id = buf + sizeof(struct btree_node_header);

        /* Update the key at parent_slot to the left child's
         * actual minimum before building the merged array. */
        struct overflow_index_entry oe_upd;
        memcpy(&oe_upd, id + (size_t)parent_slot * ie_sz, sizeof(oe_upd));
        oe_upd.inode_id       = left_first_key;
        oe_upd.logical_offset = left_first_off;
        memcpy(id + (size_t)parent_slot * ie_sz, &oe_upd, sizeof(oe_upd));

        memcpy(aie, id, (size_t)idx_insert * ie_sz);
        aie[idx_insert].inode_id       = push_key;
        aie[idx_insert].logical_offset = push_offset;
        aie[idx_insert].child_lba      = push_child;
        memcpy(&aie[idx_insert + 1], id + (size_t)idx_insert * ie_sz, ((size_t)max_idx - (size_t)idx_insert) * ie_sz);

        uint16_t il = idx_total / 2;
        uint16_t ir = idx_total - il;

        /* Allocate new index node before writing so we can set sibling links */
        uint64_t idx_old_right = phdr.right_link;
        uint64_t new_idx_lba;
        rc = obmafs3_btree_alloc_node(ctx, hdr, hdr_lba, &new_idx_lba);
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
        nih.record_type = kBtreeDataTypeExtent;
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

        push_key       = aie[il].inode_id;
        push_offset    = aie[il].logical_offset;
        push_child     = new_idx_lba;
        left_first_key = aie[0].inode_id;
        left_first_off = aie[0].logical_offset;
        left_lba       = parent_lba;

        free(aie);
        hdr->total_nodes++;

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
    rc = obmafs3_btree_alloc_node(ctx, hdr, hdr_lba, &new_root_lba);
    if(rc != OBMAFS3_OK) return rc;

    /* Read old root to get its level */
    rc = obmafs3_block_read(ctx, left_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;
    struct btree_node_header old_hdr;
    memcpy(&old_hdr, buf, sizeof(old_hdr));

    memset(buf, 0, bsz);
    struct btree_node_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    rh.record_type = kBtreeDataTypeExtent;
    rh.level       = old_hdr.level + 1;
    rh.node_keys   = 2;
    rh.keys_length = (uint16_t)(2 * sizeof(struct overflow_index_entry));
    memcpy(buf, &rh, sizeof(rh));

    struct overflow_index_entry roots[2];
    roots[0].inode_id       = left_first_key;
    roots[0].logical_offset = left_first_off;
    roots[0].child_lba      = left_lba;
    roots[1].inode_id       = push_key;
    roots[1].logical_offset = push_offset;
    roots[1].child_lba      = push_child;
    memcpy(buf + sizeof(rh), roots, sizeof(roots));
    compute_node_checksum(buf);

    rc = obmafs3_block_write(ctx, new_root_lba, buf, bsz);
    if(rc != OBMAFS3_OK) return rc;

    hdr->root_node_lba = new_root_lba;
    hdr->total_nodes++;
    return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
}

/* ------------------------------------------------------------------ */
/*  Extent descriptor — unified inline + overflow representation       */
/* ------------------------------------------------------------------ */

struct extent_descriptor
{
    uint64_t logical_start; /* first logical block covered */
    uint64_t logical_count; /* number of logical blocks    */
    uint64_t phys_start;    /* first physical LBA          */
    uint64_t phys_count;    /* number of physical blocks   */
};

/* ------------------------------------------------------------------ */
/*  Overflow extent helpers (variable-length extents)                  */
/* ------------------------------------------------------------------ */

/**
 * Count total logical blocks covered by inline extents.
 */
static uint64_t count_inline_logical(const struct inode_record *inode)
{
    uint64_t total = 0;
    for(int i = 0; i < 8; i++) total += inode->extents[i].logical_blocks;
    return total;
}

/**
 * Search the overflow B+Tree for the extent covering @p logical_block.
 *
 * Each overflow_extent now stores an absolute logical_offset, so we
 * simply look for the entry whose range contains the target block.
 *
 * @return 1 if found (out filled), 0 if not found.
 */
static int overflow_find_extent(struct obmafs3_ctx *ctx, uint64_t inode_id, uint64_t logical_block,
                                struct extent_descriptor *out)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return 0;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    uint64_t lba = hdr->root_node_lba;

    /* Traverse index levels to reach the leaf */
    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return 0;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return 0;
        if(nhdr.level == 0) break;

        uint16_t                    slot = overflow_index_find_left(buf, nhdr.node_keys, inode_id);
        struct overflow_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf nodes for the inode's extents */
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return 0;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            past    = 0;
        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));

            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }

            if(logical_block >= oe.logical_offset && logical_block < oe.logical_offset + oe.logical_count)
            {
                out->logical_start = oe.logical_offset;
                out->logical_count = oe.logical_count;
                out->phys_start    = oe.start_block;
                out->phys_count    = oe.block_count;
                return 1;
            }
        }
        if(past) break;
        lba = nhdr.right_link;
    }

    return 0;
}

/**
 * Count the total number of LOGICAL blocks stored in overflow extents
 * for an inode.
 */
static uint64_t overflow_count_logical(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return 0;

    uint8_t *buf = obmafs3_get_thread_bufs(ctx)->node_buf;
    uint64_t lba = hdr->root_node_lba;

    while(1)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return 0;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) return 0;
        if(nhdr.level == 0) break;

        uint16_t                    slot = overflow_index_find_left(buf, nhdr.node_keys, inode_id);
        struct overflow_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    uint64_t total = 0;
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            past    = 0;
        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));

            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }
            total += oe.logical_count;
        }
        if(past) break;
        lba = nhdr.right_link;
    }

    return total;
}

/**
 * Find the extent covering @p logical_block in the inode's full extent
 * map (inline extents first, then overflow B+Tree).
 *
 * @return 1 if found (out filled), 0 if not found.
 */
static int find_extent(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t logical_block,
                       struct extent_descriptor *out)
{
    uint64_t base = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count == 0 && inode->extents[i].logical_blocks == 0) continue;
        uint64_t lc = inode->extents[i].logical_blocks;
        if(logical_block >= base && logical_block < base + lc)
        {
            out->logical_start = base;
            out->logical_count = lc;
            out->phys_start    = inode->extents[i].start_block;
            out->phys_count    = inode->extents[i].block_count;
            return 1;
        }
        base += lc;
    }

    return overflow_find_extent(ctx, inode->inode_id, logical_block, out);
}

/* ------------------------------------------------------------------ */
/*  File data reading                                                  */
/* ------------------------------------------------------------------ */

/**
 * Read logical blocks from a single extent into @p out_buf.
 *
 * For uncompressed extents (logical_count == phys_count) the blocks
 * are read directly.  For compressed extents the physical blocks are
 * read, the block_header is parsed, and the data is decompressed.
 *
 * When reading the full extent (offset_in_ext == 0 && logical_count ==
 * extent logical_count), decompression goes directly into @p out_buf.
 * For partial reads, io_buf2 is used as a temporary decompression
 * target.
 *
 * @param ctx            Filesystem context.
 * @param ext            Extent descriptor.
 * @param first_logical  First logical block to read (absolute).
 * @param logical_count  Number of logical blocks to read.
 * @param out_buf        Output buffer (must be >= logical_count * block_size).
 * @return OBMAFS3_OK on success.
 */
static int read_extent_blocks(struct obmafs3_ctx *ctx, const struct extent_descriptor *ext, uint64_t first_logical,
                              uint64_t logical_count, uint8_t *out_buf)
{
    uint64_t block_size    = ctx->sb.block_size;
    uint64_t offset_in_ext = first_logical - ext->logical_start;

    if(ext->logical_count == ext->phys_count)
    {
        /* Uncompressed: read blocks directly into out_buf */
        for(uint64_t i = 0; i < logical_count; i++)
        {
            int rc = obmafs3_block_read(ctx, ext->phys_start + offset_in_ext + i, out_buf + i * (size_t)block_size,
                                        (size_t)block_size);
            if(rc != OBMAFS3_OK) return rc;
        }
    }
    else
    {
        /* Compressed extent: read all physical blocks into io_buf */
        uint8_t *phys_buf = obmafs3_get_thread_bufs(ctx)->io_buf;
        for(uint64_t b = 0; b < ext->phys_count; b++)
        {
            int rc =
                obmafs3_block_read(ctx, ext->phys_start + b, phys_buf + b * (size_t)block_size, (size_t)block_size);
            if(rc != OBMAFS3_OK) return rc;
        }

        /* Parse block_header at the start */
        struct block_header bhdr;
        memcpy(&bhdr, phys_buf, sizeof(bhdr));
        if(bhdr.magic != OBMAFS3_BLOCK_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

        /* Verify checksum */
        size_t check_size =
            (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? (size_t)bhdr.compressed_size : (size_t)bhdr.original_size;
        uint8_t computed[32];
        obmafs3_checksum_block(phys_buf + sizeof(bhdr), check_size, computed);
        if(memcmp(computed, bhdr.checksum, 32) != 0) DBG_RETURN(OBMAFS3_ERR_CHECKSUM, "checksum mismatch");

        if(offset_in_ext == 0 && logical_count == ext->logical_count)
        {
            /* Full extent read → decompress directly into out_buf */
            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                int rc = obmafs3_decompress_dispatch(ctx, bhdr.compression_type, phys_buf + sizeof(bhdr),
                                            (size_t)bhdr.compressed_size, out_buf, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK) return rc;
            }
            else
            {
                memcpy(out_buf, phys_buf + sizeof(bhdr), (size_t)bhdr.original_size);
            }
        }
        else
        {
            /* Partial read → decompress into io_buf2, copy the portion */
            uint8_t *decomp = obmafs3_get_thread_bufs(ctx)->io_buf2;
            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                int rc = obmafs3_decompress_dispatch(ctx, bhdr.compression_type, phys_buf + sizeof(bhdr),
                                            (size_t)bhdr.compressed_size, decomp, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK) return rc;
            }
            else
            {
                memcpy(decomp, phys_buf + sizeof(bhdr), (size_t)bhdr.original_size);
            }

            size_t start_off  = (size_t)(offset_in_ext * block_size);
            size_t copy_bytes = (size_t)(logical_count * block_size);
            if(start_off + copy_bytes <= (size_t)bhdr.original_size)
            {
                memcpy(out_buf, decomp + start_off, copy_bytes);
            }
            else
            {
                size_t avail = (start_off < (size_t)bhdr.original_size) ? (size_t)bhdr.original_size - start_off : 0;
                if(avail > 0) memcpy(out_buf, decomp + start_off, avail);
                if(avail < copy_bytes) memset(out_buf + avail, 0, copy_bytes - avail);
            }
        }
    }

    return OBMAFS3_OK;
}

/**
 * Read file data from the filesystem.
 *
 * Resolves logical block offsets through the inode's inline extents and
 * overflow extent tree.  Uncompressed extents are read directly;
 * compressed extents are decompressed transparently with caching of the
 * last decompressed group to avoid redundant decompression for
 * sequential reads.
 *
 * @param ctx     Filesystem context.
 * @param inode   Inode record describing the file.
 * @param offset  Byte offset within the file to start reading.
 * @param buf     Output buffer.
 * @param size    Number of bytes to read.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_read_file_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                           size_t size)
{
    uint64_t block_size = ctx->sb.block_size;
    size_t   bytes_read = 0;

    if(offset >= inode->file_size) return OBMAFS3_OK;
    if(offset + size > inode->file_size) size = (size_t)(inode->file_size - offset);

    /* Cache last decompressed compressed extent to avoid re-decompressing
     * for sequential reads within the same group. */
    struct extent_descriptor cached_ext;
    memset(&cached_ext, 0, sizeof(cached_ext));
    int      cached_valid = 0;
    uint8_t *decomp_buf   = obmafs3_get_thread_bufs(ctx)->io_buf2;

    while(bytes_read < size)
    {
        uint64_t read_pos     = offset + bytes_read;
        uint64_t logical_blk  = read_pos / block_size;
        size_t   off_in_block = (size_t)(read_pos % block_size);

        struct extent_descriptor ext;
        if(!find_extent(ctx, inode, logical_blk, &ext)) DBG_RETURN(OBMAFS3_ERR_IO, "I/O error");

        if(ext.logical_count == ext.phys_count)
        {
            /* Uncompressed extent — read as many blocks as we need in one pass */
            uint64_t blk_in_ext = logical_blk - ext.logical_start;
            uint64_t blks_left  = ext.logical_count - blk_in_ext;
            uint8_t *block_buf  = obmafs3_get_thread_bufs(ctx)->io_buf;

            /* How many bytes remain in this extent from the current read position */
            size_t ext_avail  = (size_t)(blks_left * block_size) - off_in_block;
            size_t want       = size - bytes_read;
            size_t to_consume = (want < ext_avail) ? want : ext_avail;

            /* Read block by block and copy the relevant portion.
             * For the first block, skip off_in_block bytes.  For internal
             * blocks, copy whole blocks directly into the output buffer
             * to avoid an extra memcpy.  For the last block, copy only
             * the needed tail. */
            size_t consumed = 0;
            while(consumed < to_consume)
            {
                uint64_t cur_blk   = logical_blk + (off_in_block + consumed) / block_size;
                /* Re-derive the per-block offset only matters for the very
                 * first iteration when off_in_block != 0. */
                size_t   blk_off   = (consumed == 0) ? off_in_block : 0;
                size_t   blk_avail = (size_t)block_size - blk_off;
                size_t   chunk     = to_consume - consumed;
                if(chunk > blk_avail) chunk = blk_avail;

                uint64_t phys_lba = ext.phys_start + (cur_blk - ext.logical_start);

                if(blk_off == 0 && chunk == block_size)
                {
                    /* Full-block read directly into output buffer */
                    int rc =
                        obmafs3_block_read(ctx, phys_lba, (uint8_t *)buf + bytes_read + consumed, (size_t)block_size);
                    if(rc != OBMAFS3_OK) return rc;
                }
                else
                {
                    /* Partial block — bounce through io_buf */
                    int rc = obmafs3_block_read(ctx, phys_lba, block_buf, (size_t)block_size);
                    if(rc != OBMAFS3_OK) return rc;
                    memcpy((uint8_t *)buf + bytes_read + consumed, block_buf + blk_off, chunk);
                }
                consumed += chunk;
            }
            bytes_read += to_consume;
        }
        else
        {
            /* Compressed extent — decompress once, then serve all needed bytes */
            if(!cached_valid || cached_ext.phys_start != ext.phys_start)
            {
                /* Read all physical blocks of the extent */
                uint8_t *phys_buf = obmafs3_get_thread_bufs(ctx)->io_buf;
                for(uint64_t b = 0; b < ext.phys_count; b++)
                {
                    int rc = obmafs3_block_read(ctx, ext.phys_start + b, phys_buf + b * (size_t)block_size,
                                                (size_t)block_size);
                    if(rc != OBMAFS3_OK) return rc;
                }

                struct block_header bhdr;
                memcpy(&bhdr, phys_buf, sizeof(bhdr));
                if(bhdr.magic != OBMAFS3_BLOCK_MAGIC) DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic");

                size_t  check_size = (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? (size_t)bhdr.compressed_size
                                                                                  : (size_t)bhdr.original_size;
                uint8_t computed[32];
                obmafs3_checksum_block(phys_buf + sizeof(bhdr), check_size, computed);
                if(memcmp(computed, bhdr.checksum, 32) != 0) DBG_RETURN(OBMAFS3_ERR_CHECKSUM, "checksum mismatch");

                if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                {
                    int rc = obmafs3_decompress_dispatch(ctx, bhdr.compression_type, phys_buf + sizeof(bhdr),
                                                (size_t)bhdr.compressed_size, decomp_buf, (size_t)bhdr.original_size);
                    if(rc != OBMAFS3_OK) return rc;
                }
                else
                {
                    memcpy(decomp_buf, phys_buf + sizeof(bhdr), (size_t)bhdr.original_size);
                }
                cached_ext   = ext;
                cached_valid = 1;
            }

            /* Copy as much data from this extent as needed in a single memcpy */
            uint64_t blk_in_ext    = logical_blk - ext.logical_start;
            size_t   decomp_offset = (size_t)(blk_in_ext * block_size + off_in_block);
            size_t   decomp_total  = (size_t)(ext.logical_count * block_size);
            size_t   ext_avail     = (decomp_offset < decomp_total) ? decomp_total - decomp_offset : 0;
            size_t   want          = size - bytes_read;
            size_t   to_copy       = (want < ext_avail) ? want : ext_avail;
            memcpy((uint8_t *)buf + bytes_read, decomp_buf + decomp_offset, to_copy);
            bytes_read += to_copy;
        }
    }

    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Overflow helpers for extent management                             */
/* ------------------------------------------------------------------ */

/**
 * Collect all overflow extents for @a inode_id as extent_descriptors.
 * The caller must free @c *out_exts when done.
 */
static int overflow_collect_extents(struct obmafs3_ctx *ctx, uint64_t inode_id, struct extent_descriptor **out_exts,
                                    uint64_t *out_count)
{
    *out_exts  = NULL;
    *out_count = 0;

    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    struct extent_descriptor *exts  = NULL;
    uint64_t                  count = 0, cap = 0;

    /* Navigate to first relevant leaf */
    uint64_t lba = hdr->root_node_lba;
    for(;;)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.level == 0) break;
        uint16_t                    slot = overflow_index_find_left(buf, nhdr.node_keys, inode_id);
        struct overflow_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaves */
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            past    = 0;

        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));
            if(oe.inode_id < inode_id) continue;
            if(oe.inode_id > inode_id)
            {
                past = 1;
                break;
            }

            if(count >= cap)
            {
                cap                           = (cap == 0) ? 64 : cap * 2;
                struct extent_descriptor *tmp = realloc(exts, (size_t)cap * sizeof(*tmp));
                if(!tmp)
                {
                    free(exts);
                    free(buf);
                    DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                }
                exts = tmp;
            }
            exts[count].logical_start = oe.logical_offset;
            exts[count].logical_count = oe.logical_count;
            exts[count].phys_start    = oe.start_block;
            exts[count].phys_count    = oe.block_count;
            count++;
        }
        if(past) break;
        lba = nhdr.right_link;
    }

    free(buf);
    *out_exts  = exts;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Remove every overflow extent entry for @a inode_id from the
 * overflow B+Tree.  Entries for other inodes are left intact.
 *
 * The tree is @b not rebalanced; leaves may become underfull or
 * empty.  This is acceptable for a subsequent rebuild via
 * overflow_insert().
 *
 * @param ctx       Filesystem context.
 * @param inode_id  Inode whose overflow entries are removed.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int overflow_clear_inode(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    struct btree_header *hdr = &ctx->overflow_hdr;
    if(hdr->root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Navigate to the first leaf that may contain this inode */
    uint64_t lba = hdr->root_node_lba;
    for(;;)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.level == 0) break;
        uint16_t                    slot = overflow_index_find_left(buf, nhdr.node_keys, inode_id);
        struct overflow_index_entry ie;
        memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Walk leaves, compacting out entries that match inode_id */
    const size_t rec_sz = sizeof(struct overflow_extent);
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        uint8_t *entries  = buf + sizeof(struct btree_node_header);
        int      modified = 0, past = 0;
        uint16_t wp = 0;

        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * rec_sz, rec_sz);
            if(oe.inode_id == inode_id)
            {
                modified = 1;
                continue; /* skip / remove */
            }
            if(oe.inode_id > inode_id) past = 1;
            if(wp != i) memmove(entries + (size_t)wp * rec_sz, entries + (size_t)i * rec_sz, rec_sz);
            wp++;
        }

        if(modified)
        {
            if(wp < nhdr.node_keys) memset(entries + (size_t)wp * rec_sz, 0, ((size_t)nhdr.node_keys - wp) * rec_sz);
            nhdr.node_keys   = wp;
            nhdr.keys_length = (uint16_t)(wp * rec_sz);
            memcpy(buf, &nhdr, sizeof(nhdr));
            compute_node_checksum(buf);
            rc = obmafs3_block_write(ctx, lba, buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK)
            {
                free(buf);
                return rc;
            }
        }

        if(past) break;
        lba = nhdr.right_link;
    }

    free(buf);
    return OBMAFS3_OK;
}

/**
 * Collect ALL extents (inline + overflow) for an inode into a flat
 * array sorted by logical_start.  The caller must free @c *out.
 */
static int collect_all_extents(struct obmafs3_ctx *ctx, const struct inode_record *inode,
                               struct extent_descriptor **out, uint64_t *out_count)
{
    /* Count inline extents */
    uint64_t inline_count = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count > 0 || inode->extents[i].logical_blocks > 0) inline_count++;
    }

    /* Collect overflow extents */
    struct extent_descriptor *ovf_exts  = NULL;
    uint64_t                  ovf_count = 0;
    int                       rc        = overflow_collect_extents(ctx, inode->inode_id, &ovf_exts, &ovf_count);
    if(rc != OBMAFS3_OK) return rc;

    uint64_t total = inline_count + ovf_count;
    if(total == 0)
    {
        free(ovf_exts);
        *out       = NULL;
        *out_count = 0;
        return OBMAFS3_OK;
    }

    struct extent_descriptor *list = calloc((size_t)total, sizeof(*list));
    if(!list)
    {
        free(ovf_exts);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    /* Add inline extents */
    uint64_t idx = 0, base = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count == 0 && inode->extents[i].logical_blocks == 0) continue;
        list[idx].logical_start = base;
        list[idx].logical_count = inode->extents[i].logical_blocks;
        list[idx].phys_start    = inode->extents[i].start_block;
        list[idx].phys_count    = inode->extents[i].block_count;
        base += inode->extents[i].logical_blocks;
        idx++;
    }

    /* Add overflow extents */
    if(ovf_count > 0)
    {
        memcpy(list + idx, ovf_exts, (size_t)ovf_count * sizeof(*list));
        idx += ovf_count;
    }
    free(ovf_exts);

    *out       = list;
    *out_count = idx;
    return OBMAFS3_OK;
}

/**
 * Free a single physical block, respecting refcounts.
 */
static void free_single_phys_block(struct obmafs3_ctx *ctx, uint64_t lba)
{
    uint32_t ref = 1;
    obmafs3_refcount_get(ctx, lba, &ref);
    if(ref > 1)
        obmafs3_refcount_dec(ctx, lba, NULL);
    else
        obmafs3_free_block(ctx, lba);
}

/**
 * Free all physical blocks of an extent, respecting refcounts.
 */
static void free_extent_phys(struct obmafs3_ctx *ctx, const struct extent_descriptor *ext)
{
    for(uint64_t b = 0; b < ext->phys_count; b++) free_single_phys_block(ctx, ext->phys_start + b);
}

/**
 * Write an extent list back to the inode's inline slots + overflow tree.
 *
 * Clears existing overflow entries, merges adjacent uncompressed
 * extents, fills inline slots, and spills the rest to overflow.
 * The input list must be sorted by logical_start.
 */
static int write_extent_list(struct obmafs3_ctx *ctx, struct inode_record *inode, const struct extent_descriptor *list,
                             uint64_t count)
{
    memset(inode->extents, 0, sizeof(inode->extents));

    int rc = overflow_clear_inode(ctx, inode->inode_id);
    if(rc != OBMAFS3_OK) return rc;

    if(count == 0) return OBMAFS3_OK;

    /* Coalesce adjacent uncompressed extents */
    struct extent_descriptor *merged = calloc((size_t)count, sizeof(*merged));
    if(!merged) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    uint64_t mc = 0;
    merged[0]   = list[0];
    for(uint64_t i = 1; i < count; i++)
    {
        struct extent_descriptor *prev = &merged[mc];
        /* Merge only if both are uncompressed and physically contiguous */
        if(prev->logical_count == prev->phys_count && list[i].logical_count == list[i].phys_count &&
           prev->phys_start + prev->phys_count == list[i].phys_start &&
           prev->logical_start + prev->logical_count == list[i].logical_start)
        {
            prev->logical_count += list[i].logical_count;
            prev->phys_count += list[i].phys_count;
        }
        else
        {
            mc++;
            merged[mc] = list[i];
        }
    }
    mc++;

    /* First 8 extent runs go to inline slots */
    uint64_t inline_limit = (mc < 8) ? mc : 8;
    for(uint64_t i = 0; i < inline_limit; i++)
    {
        inode->extents[i].start_block    = merged[i].phys_start;
        inode->extents[i].block_count    = merged[i].phys_count;
        inode->extents[i].logical_blocks = merged[i].logical_count;
    }

    /* Remaining extents go to overflow tree */
    for(uint64_t i = 8; i < mc; i++)
    {
        struct overflow_extent oe;
        oe.inode_id       = inode->inode_id;
        oe.logical_offset = merged[i].logical_start;
        oe.start_block    = merged[i].phys_start;
        oe.block_count    = merged[i].phys_count;
        oe.logical_count  = merged[i].logical_count;
        rc                = overflow_insert(ctx, &oe);
        if(rc != OBMAFS3_OK)
        {
            free(merged);
            return rc;
        }
    }

    free(merged);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  File data writing                                                  */
/* ------------------------------------------------------------------ */

/**
 * Fast path for appending data past the end of a file.
 *
 * When the write begins at or after the current file_size AND the
 * first affected compression group does not overlap any existing
 * group, we can skip collecting all existing extents and the
 * expensive clear+rebuild of the overflow B+Tree.  Instead, each new
 * compression group is written directly and its extent is inserted
 * into an inline slot or directly into the overflow tree.
 *
 * This turns sequential file writes from O(n²) to O(n).
 */
static int write_file_data_append(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t offset, const void *buf,
                                  size_t size)
{
    uint64_t block_size  = ctx->sb.block_size;
    uint64_t group_size  = OBMAFS3_COMPRESS_GROUP_BLOCKS;
    uint64_t group_bytes = block_size * group_size;
    int      rc;

    /* Update file size */
    uint64_t new_end = offset + size;
    if(new_end > inode->file_size) inode->file_size = new_end;

    uint64_t total_logical = (inode->file_size + block_size - 1) / block_size;

    /* Determine affected groups */
    uint64_t first_block = offset / block_size;
    uint64_t last_block  = (offset + size > 0) ? (offset + size - 1) / block_size : first_block;
    uint64_t first_group = first_block / group_size;
    uint64_t last_group  = last_block / group_size;
    int      num_groups  = (int)(last_group - first_group + 1);

    /* Count used inline slots */
    int inline_used = 0;
    for(int i = 0; i < 8; i++)
    {
        if(inode->extents[i].block_count > 0 || inode->extents[i].logical_blocks > 0) inline_used++;
    }

    /* -------------------------------------------------------------- */
    /*  Phase 1 — assemble all groups' input data                      */
    /* -------------------------------------------------------------- */

    /* Per-group metadata needed across phases */
    uint64_t *grp_counts = calloc((size_t)num_groups, sizeof(uint64_t));
    uint64_t *grp_starts = calloc((size_t)num_groups, sizeof(uint64_t)); /* logical start block */
    size_t   *grp_sizes  = calloc((size_t)num_groups, sizeof(size_t));
    uint8_t **grp_bufs   = calloc((size_t)num_groups, sizeof(uint8_t *));

    if(!grp_counts || !grp_starts || !grp_sizes || !grp_bufs)
    {
        free(grp_counts);
        free(grp_starts);
        free(grp_sizes);
        free(grp_bufs);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    for(int gi = 0; gi < num_groups; gi++)
    {
        uint64_t g         = first_group + (uint64_t)gi;
        uint64_t grp_start = g * group_size;
        uint64_t grp_end   = (g + 1) * group_size;
        if(grp_end > total_logical) grp_end = total_logical;
        uint64_t grp_count   = grp_end - grp_start;
        size_t   grp_bytes_i = (size_t)(grp_count * block_size);

        grp_counts[gi] = grp_count;
        grp_starts[gi] = grp_start;
        grp_sizes[gi]  = grp_bytes_i;

        grp_bufs[gi] = malloc(grp_bytes_i);
        if(!grp_bufs[gi])
        {
            for(int j = 0; j < gi; j++) free(grp_bufs[j]);
            free(grp_bufs);
            free(grp_counts);
            free(grp_starts);
            free(grp_sizes);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }

        /* Zero-fill and overlay the write data */
        memset(grp_bufs[gi], 0, grp_bytes_i);
        uint64_t grp_byte_start = grp_start * block_size;
        uint64_t grp_byte_end   = grp_byte_start + grp_bytes_i;
        uint64_t write_start    = (offset > grp_byte_start) ? offset : grp_byte_start;
        uint64_t write_end      = (offset + size < grp_byte_end) ? offset + size : grp_byte_end;
        if(write_end > write_start)
        {
            size_t dest_off = (size_t)(write_start - grp_byte_start);
            size_t src_off  = (size_t)(write_start - offset);
            size_t nbytes   = (size_t)(write_end - write_start);
            memcpy(grp_bufs[gi] + dest_off, (const uint8_t *)buf + src_off, nbytes);
        }
    }

    /* -------------------------------------------------------------- */
    /*  Phase 2 — compress groups in parallel                          */
    /* -------------------------------------------------------------- */

    /* Compression output buffer size (one per group) */
    size_t comp_buf_cap = ZSTD_compressBound((size_t)group_bytes) + sizeof(struct block_header);

    struct compress_job *jobs = NULL;

    /* Only attempt parallel compression when it's enabled and there
     * is at least one full group. */
    int any_compressible = 0;
    if(ctx->compression)
    {
        jobs = calloc((size_t)num_groups, sizeof(struct compress_job));
        if(jobs)
        {
            int all_ok = 1;
            for(int gi = 0; gi < num_groups; gi++)
            {
                if(grp_counts[gi] != group_size || grp_sizes[gi] == 0) continue;

                jobs[gi].comp_buf = malloc(comp_buf_cap);
                if(!jobs[gi].comp_buf)
                {
                    all_ok = 0;
                    break;
                }
                jobs[gi].group_data    = grp_bufs[gi];
                jobs[gi].grp_bytes     = grp_sizes[gi];
                jobs[gi].grp_count     = grp_counts[gi];
                jobs[gi].level         = (ctx->compression_algo == kCompressionLzma) ? ctx->lzma_level : ctx->zstd_level;
                jobs[gi].block_size    = block_size;
                jobs[gi].comp_buf_size = comp_buf_cap;
                jobs[gi].algo          = (uint8_t)ctx->compression_algo;
                jobs[gi].lzma_dict     = ctx->lzma_dict_size;
                any_compressible       = 1;
            }

            if(!all_ok)
            {
                for(int gi = 0; gi < num_groups; gi++) free(jobs[gi].comp_buf);
                free(jobs);
                jobs             = NULL;
                any_compressible = 0;
            }
        }
    }

    if(any_compressible && jobs) { compress_pool_submit(ctx->compress_pool, jobs, num_groups); }

    /* -------------------------------------------------------------- */
    /*  Phase 3 — allocate blocks, write data, record extents          */
    /* -------------------------------------------------------------- */

    rc = OBMAFS3_OK;

    for(int gi = 0; gi < num_groups; gi++)
    {
        uint64_t grp_count  = grp_counts[gi];
        uint64_t phys_start = 0;
        uint64_t phys_count = 0;

        /* Check if compression succeeded for this group */
        if(jobs && jobs[gi].use_compressed)
        {
            uint64_t phys_needed = jobs[gi].phys_needed;
            rc                   = obmafs3_alloc_blocks(ctx, phys_needed, &phys_start);
            if(rc != OBMAFS3_OK) goto cleanup;

            for(uint64_t b = 0; b < phys_needed; b++)
            {
                rc = obmafs3_block_write(ctx, phys_start + b, jobs[gi].comp_buf + b * (size_t)block_size,
                                         (size_t)block_size);
                if(rc != OBMAFS3_OK) goto cleanup;
            }
            phys_count = phys_needed;

            /* Set LZMA incompat flag on first LZMA block */
            if(jobs[gi].algo == kCompressionLzma &&
               !(ctx->sb.incompatible_flags & OBMAFS3_INCOMPAT_LZMA))
            {
                ctx->sb.incompatible_flags |= OBMAFS3_INCOMPAT_LZMA;
                obmafs3_sb_write(ctx->fd, &ctx->sb);
            }
        }
        else
        {
            /* Store uncompressed */
            rc = obmafs3_alloc_blocks(ctx, grp_count, &phys_start);
            if(rc != OBMAFS3_OK) goto cleanup;

            for(uint64_t b = 0; b < grp_count; b++)
            {
                rc =
                    obmafs3_block_write(ctx, phys_start + b, grp_bufs[gi] + b * (size_t)block_size, (size_t)block_size);
                if(rc != OBMAFS3_OK) goto cleanup;
            }
            phys_count = grp_count;
        }

        /* Add extent directly: inline slot if room, else overflow */
        if(inline_used < 8)
        {
            inode->extents[inline_used].start_block    = phys_start;
            inode->extents[inline_used].block_count    = phys_count;
            inode->extents[inline_used].logical_blocks = grp_count;
            inline_used++;
        }
        else
        {
            struct overflow_extent oe;
            oe.inode_id       = inode->inode_id;
            oe.logical_offset = grp_starts[gi];
            oe.start_block    = phys_start;
            oe.block_count    = phys_count;
            oe.logical_count  = grp_count;
            rc                = overflow_insert(ctx, &oe);
            if(rc != OBMAFS3_OK) goto cleanup;
        }
    }

cleanup:
    if(jobs)
    {
        for(int gi = 0; gi < num_groups; gi++) free(jobs[gi].comp_buf);
        free(jobs);
    }
    for(int gi = 0; gi < num_groups; gi++) free(grp_bufs[gi]);
    free(grp_bufs);
    free(grp_counts);
    free(grp_starts);
    free(grp_sizes);
    return rc;
}

/**
 * Write file data to the filesystem.
 *
 * Data is organized into compression groups of
 * OBMAFS3_COMPRESS_GROUP_BLOCKS logical blocks (default 16 × 4 KiB =
 * 64 KiB).  Full compression groups are ZSTD-compressed as a unit
 * into variable-length physical extents.  Partial groups (at the tail
 * of a file) are stored uncompressed.  Individual blocks never carry
 * a block_header; only compressed extents do.
 *
 * @param ctx     Filesystem context.
 * @param inode   Inode record to update (modified in place).
 * @param offset  Byte offset within the file to start writing.
 * @param buf     Data buffer to write.
 * @param size    Number of bytes to write.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_write_file_data(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t offset, const void *buf,
                            size_t size)
{
    uint64_t block_size  = ctx->sb.block_size;
    uint64_t group_size  = OBMAFS3_COMPRESS_GROUP_BLOCKS;
    uint64_t group_bytes = block_size * group_size;
    int      rc;

    /* ---- Fast path: pure append (no overlap with existing data) ---- */
    {
        uint64_t old_file_size    = inode->file_size;
        uint64_t old_total_blocks = (old_file_size + block_size - 1) / block_size;
        uint64_t old_groups       = (old_total_blocks + group_size - 1) / group_size;
        uint64_t first_block_     = offset / block_size;
        uint64_t first_group_     = first_block_ / group_size;

        /* Use the fast path when the write starts at or past the file end
         * AND the first affected group does not overlap any existing group. */
        if(offset >= old_file_size && (old_total_blocks == 0 || first_group_ >= old_groups))
        {
            return write_file_data_append(ctx, inode, offset, buf, size);
        }
    }

    /* Update file size */
    uint64_t new_end = offset + size;
    if(new_end > inode->file_size) inode->file_size = new_end;

    uint64_t total_logical = (inode->file_size + block_size - 1) / block_size;

    /* Determine affected logical block range */
    uint64_t first_block = offset / block_size;
    uint64_t last_block  = (offset + size > 0) ? (offset + size - 1) / block_size : first_block;

    /* Determine affected compression groups */
    uint64_t first_group = first_block / group_size;
    uint64_t last_group  = last_block / group_size;

    /* Collect all existing extents */
    struct extent_descriptor *ext_list  = NULL;
    uint64_t                  ext_count = 0;
    rc                                  = collect_all_extents(ctx, inode, &ext_list, &ext_count);
    if(rc != OBMAFS3_OK) return rc;

    /* Pre-size the extent list for the new extents we'll add */
    uint64_t ext_cap = ext_count + (last_group - first_group + 1) + 16;
    {
        struct extent_descriptor *tmp = realloc(ext_list, (size_t)ext_cap * sizeof(*tmp));
        if(!tmp)
        {
            free(ext_list);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }
        ext_list = tmp;
    }

    /* Allocate a separate group assembly buffer (up to 64 KiB).
     * This must be separate from io_buf / io_buf2 which are used
     * internally by read_extent_blocks(). */
    uint8_t *group_data = malloc((size_t)group_bytes);
    if(!group_data)
    {
        free(ext_list);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    for(uint64_t g = first_group; g <= last_group; g++)
    {
        uint64_t grp_start = g * group_size;
        uint64_t grp_end   = (g + 1) * group_size;
        if(grp_end > total_logical) grp_end = total_logical;
        uint64_t grp_count = grp_end - grp_start;
        size_t   grp_bytes = (size_t)(grp_count * block_size);

        /* Zero-fill the group assembly buffer */
        memset(group_data, 0, grp_bytes);

        /* Read existing data from extents that overlap this group */
        for(uint64_t ei = 0; ei < ext_count; ei++)
        {
            struct extent_descriptor *e     = &ext_list[ei];
            uint64_t                  e_end = e->logical_start + e->logical_count;

            /* Check for overlap with [grp_start, grp_end) */
            if(e_end <= grp_start || e->logical_start >= grp_end) continue;

            /* Compute overlap range */
            uint64_t overlap_start = (e->logical_start > grp_start) ? e->logical_start : grp_start;
            uint64_t overlap_end   = (e_end < grp_end) ? e_end : grp_end;
            uint64_t overlap_count = overlap_end - overlap_start;

            /* Read the overlapping blocks into group_data at the right offset */
            uint8_t *dest = group_data + (size_t)((overlap_start - grp_start) * block_size);
            rc            = read_extent_blocks(ctx, e, overlap_start, overlap_count, dest);
            if(rc != OBMAFS3_OK)
            {
                free(group_data);
                free(ext_list);
                return rc;
            }
        }

        /* Overlay new write data */
        uint64_t grp_byte_start = grp_start * block_size;
        uint64_t grp_byte_end   = grp_start * block_size + grp_bytes;
        uint64_t write_start    = (offset > grp_byte_start) ? offset : grp_byte_start;
        uint64_t write_end      = (offset + size < grp_byte_end) ? offset + size : grp_byte_end;
        if(write_end > write_start)
        {
            size_t dest_off = (size_t)(write_start - grp_byte_start);
            size_t src_off  = (size_t)(write_start - offset);
            size_t nbytes   = (size_t)(write_end - write_start);
            memcpy(group_data + dest_off, (const uint8_t *)buf + src_off, nbytes);
        }

        /* Remove overlapping extents from the list, freeing their physical blocks.
         * Uncompressed extents that straddle the group boundary are trimmed.
         * Compressed extents that straddle (shouldn't happen by design) are freed entirely. */
        for(uint64_t ei = 0; ei < ext_count;)
        {
            struct extent_descriptor *e     = &ext_list[ei];
            uint64_t                  e_end = e->logical_start + e->logical_count;

            if(e_end <= grp_start || e->logical_start >= grp_end)
            {
                ei++;
                continue;
            }

            /* Extent entirely within the group */
            if(e->logical_start >= grp_start && e_end <= grp_end)
            {
                free_extent_phys(ctx, e);
                memmove(e, e + 1, (size_t)(ext_count - ei - 1) * sizeof(*e));
                ext_count--;
                continue;
            }

            /* Uncompressed extent straddling only the left boundary */
            if(e->logical_start < grp_start && e_end <= grp_end && e->logical_count == e->phys_count)
            {
                uint64_t keep = grp_start - e->logical_start;
                for(uint64_t b = keep; b < e->phys_count; b++) free_single_phys_block(ctx, e->phys_start + b);
                e->logical_count = keep;
                e->phys_count    = keep;
                ei++;
                continue;
            }

            /* Uncompressed extent straddling only the right boundary */
            if(e->logical_start >= grp_start && e_end > grp_end && e->logical_count == e->phys_count)
            {
                uint64_t skip = grp_end - e->logical_start;
                for(uint64_t b = 0; b < skip; b++) free_single_phys_block(ctx, e->phys_start + b);
                e->logical_start += skip;
                e->logical_count -= skip;
                e->phys_start += skip;
                e->phys_count -= skip;
                ei++;
                continue;
            }

            /* Uncompressed extent fully containing the group */
            if(e->logical_start < grp_start && e_end > grp_end && e->logical_count == e->phys_count)
            {
                uint64_t left_count    = grp_start - e->logical_start;
                uint64_t right_start_l = grp_end;
                uint64_t right_count   = e_end - grp_end;
                uint64_t right_phys    = e->phys_start + (grp_end - e->logical_start);

                for(uint64_t b = left_count; b < left_count + grp_count; b++)
                    free_single_phys_block(ctx, e->phys_start + b);

                e->logical_count = left_count;
                e->phys_count    = left_count;

                /* Insert the right residual part */
                if(ext_count >= ext_cap)
                {
                    ext_cap                        = ext_cap * 2 + 16;
                    struct extent_descriptor *tmp2 = realloc(ext_list, (size_t)ext_cap * sizeof(*tmp2));
                    if(!tmp2)
                    {
                        free(group_data);
                        free(ext_list);
                        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                    }
                    ext_list = tmp2;
                    e        = &ext_list[ei]; /* pointer may have moved */
                }
                memmove(&ext_list[ei + 2], &ext_list[ei + 1], (size_t)(ext_count - ei - 1) * sizeof(*ext_list));
                ext_list[ei + 1].logical_start = right_start_l;
                ext_list[ei + 1].logical_count = right_count;
                ext_list[ei + 1].phys_start    = right_phys;
                ext_list[ei + 1].phys_count    = right_count;
                ext_count++;
                ei += 2; /* skip the left and right residuals */
                continue;
            }

            /* Compressed extent overlapping (shouldn't cross group boundaries
             * by design, but handle gracefully by freeing entirely) */
            free_extent_phys(ctx, e);
            memmove(e, e + 1, (size_t)(ext_count - ei - 1) * sizeof(*e));
            ext_count--;
        }

        /* ---- Compress and write the group ---- */
        uint64_t phys_start     = 0;
        uint64_t phys_count     = 0;
        int      use_compressed = 0;

        /* Only attempt compression for full groups */
        if(ctx->compression && grp_count == group_size && grp_bytes > 0)
        {
            uint8_t *comp_out  = obmafs3_get_thread_bufs(ctx)->comp_buf;
            size_t   comp_cap  = obmafs3_get_thread_bufs(ctx)->comp_buf_size - sizeof(struct block_header);
            size_t   comp_size = comp_cap;

            /* Fast probe: skip expensive compression if data is incompressible.
             * Use a SEPARATE ZSTD context for the probe — sharing the same
             * cctx between a level-1 probe and a high-level compression
             * triggers a stale-window assertion in ZSTD's btopt match finder. */
            int probe_level = (ctx->compression_algo == kCompressionLzma) ? ctx->lzma_level : ctx->zstd_level;
            int worth = compression_worthwhile(obmafs3_get_thread_bufs(ctx)->zstd_probe_cctx, group_data, grp_bytes,
                                               comp_out + sizeof(struct block_header), comp_cap, probe_level);
            rc        = worth ? obmafs3_compress_dispatch(ctx, group_data, grp_bytes,
                                                         comp_out + sizeof(struct block_header), &comp_size)
                              : OBMAFS3_ERR_IO;
            if(rc == OBMAFS3_OK)
            {
                uint64_t total_on_disk = sizeof(struct block_header) + comp_size;
                uint64_t phys_needed   = (total_on_disk + block_size - 1) / block_size;
                if(phys_needed < grp_count)
                {
                    /* Compression saves space — use it */
                    use_compressed = 1;
                    rc             = obmafs3_alloc_blocks(ctx, phys_needed, &phys_start);
                    if(rc != OBMAFS3_OK)
                    {
                        free(group_data);
                        free(ext_list);
                        return rc;
                    }

                    /* Build block_header */
                    struct block_header bhdr;
                    memset(&bhdr, 0, sizeof(bhdr));
                    bhdr.magic            = OBMAFS3_BLOCK_MAGIC;
                    bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                    bhdr.compression_type = (uint8_t)ctx->compression_algo;
                    bhdr.original_size    = grp_bytes;
                    bhdr.compressed_size  = comp_size;
                    obmafs3_checksum_block(comp_out + sizeof(bhdr), comp_size, bhdr.checksum);
                    memcpy(comp_out, &bhdr, sizeof(bhdr));

                    /* Set the LZMA incompat flag if needed */
                    if(ctx->compression_algo == kCompressionLzma &&
                       !(ctx->sb.incompatible_flags & OBMAFS3_INCOMPAT_LZMA))
                    {
                        ctx->sb.incompatible_flags |= OBMAFS3_INCOMPAT_LZMA;
                        obmafs3_sb_write(ctx->fd, &ctx->sb);
                    }

                    /* Zero-pad the last physical block */
                    size_t used             = sizeof(bhdr) + comp_size;
                    size_t total_phys_bytes = (size_t)(phys_needed * block_size);
                    if(used < total_phys_bytes) memset(comp_out + used, 0, total_phys_bytes - used);

                    /* Write all physical blocks */
                    for(uint64_t b = 0; b < phys_needed; b++)
                    {
                        rc = obmafs3_block_write(ctx, phys_start + b, comp_out + b * (size_t)block_size,
                                                 (size_t)block_size);
                        if(rc != OBMAFS3_OK)
                        {
                            free(group_data);
                            free(ext_list);
                            return rc;
                        }
                    }
                    phys_count = phys_needed;
                }
            }
        }

        if(!use_compressed)
        {
            /* Write uncompressed: one physical block per logical block,
             * no block_header per block. */
            rc = obmafs3_alloc_blocks(ctx, grp_count, &phys_start);
            if(rc != OBMAFS3_OK)
            {
                free(group_data);
                free(ext_list);
                return rc;
            }

            for(uint64_t b = 0; b < grp_count; b++)
            {
                rc = obmafs3_block_write(ctx, phys_start + b, group_data + b * (size_t)block_size, (size_t)block_size);
                if(rc != OBMAFS3_OK)
                {
                    free(group_data);
                    free(ext_list);
                    return rc;
                }
            }
            phys_count = grp_count;
        }

        /* Add the new extent to the list (sorted insert) */
        if(ext_count >= ext_cap)
        {
            ext_cap                        = ext_cap * 2 + 16;
            struct extent_descriptor *tmp2 = realloc(ext_list, (size_t)ext_cap * sizeof(*tmp2));
            if(!tmp2)
            {
                free(group_data);
                free(ext_list);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }
            ext_list = tmp2;
        }

        uint64_t ins = 0;
        while(ins < ext_count && ext_list[ins].logical_start < grp_start) ins++;
        if(ins < ext_count) memmove(&ext_list[ins + 1], &ext_list[ins], (size_t)(ext_count - ins) * sizeof(*ext_list));
        ext_list[ins].logical_start = grp_start;
        ext_list[ins].logical_count = grp_count;
        ext_list[ins].phys_start    = phys_start;
        ext_list[ins].phys_count    = phys_count;
        ext_count++;
    }

    free(group_data);

    /* Write back the extent list to the inode + overflow tree */
    rc = write_extent_list(ctx, inode, ext_list, ext_count);
    free(ext_list);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Refcount-aware block freeing                                       */
/* ------------------------------------------------------------------ */

/**
 * Free all data blocks owned by an inode, respecting refcounts.
 *
 * For each physical block in the inode's inline extents and overflow
 * B+Tree entries, the block refcount is checked.  Shared blocks
 * (refcount > 1) have their refcount decremented; unshared blocks
 * are freed to the bitmap.  All inline extent slots and overflow
 * entries are cleared afterwards.
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode whose data blocks are freed (modified in place).
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_free_file_blocks(struct obmafs3_ctx *ctx, struct inode_record *inode)
{
    /* Free inline extent blocks */
    for(int i = 0; i < 8; i++)
    {
        for(uint64_t j = 0; j < inode->extents[i].block_count; j++)
            free_single_phys_block(ctx, inode->extents[i].start_block + j);
        inode->extents[i].start_block    = 0;
        inode->extents[i].block_count    = 0;
        inode->extents[i].logical_blocks = 0;
    }

    /* Free overflow extent blocks */
    struct extent_descriptor *ovf_exts  = NULL;
    uint64_t                  ovf_count = 0;
    int                       rc        = overflow_collect_extents(ctx, inode->inode_id, &ovf_exts, &ovf_count);
    if(rc != OBMAFS3_OK) return rc;

    for(uint64_t i = 0; i < ovf_count; i++) free_extent_phys(ctx, &ovf_exts[i]);
    free(ovf_exts);

    if(ovf_count > 0) overflow_clear_inode(ctx, inode->inode_id);

    return OBMAFS3_OK;
}

/**
 * Truncate an inode's data blocks to @a new_block_count logical blocks.
 *
 * Blocks beyond @a new_block_count are freed (refcount-aware).  The
 * remaining extent map is rebuilt.  Compressed extents at the boundary
 * are decompressed and rewritten as uncompressed.
 *
 * @param ctx             Filesystem context.
 * @param inode           Inode record to update (modified in place).
 * @param new_block_count Number of logical data blocks to keep.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_truncate_file_blocks(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t new_block_count)
{
    struct extent_descriptor *list  = NULL;
    uint64_t                  count = 0;
    int                       rc    = collect_all_extents(ctx, inode, &list, &count);
    if(rc != OBMAFS3_OK) return rc;

    /* Sum total logical blocks */
    uint64_t total_logical = 0;
    for(uint64_t i = 0; i < count; i++) total_logical += list[i].logical_count;

    if(new_block_count >= total_logical)
    {
        free(list);
        return OBMAFS3_OK; /* nothing to free */
    }

    /* Walk extents, trim at the truncation point */
    uint64_t keep_count  = 0;
    uint64_t logical_pos = 0;

    for(uint64_t i = 0; i < count; i++)
    {
        uint64_t ext_end = logical_pos + list[i].logical_count;

        if(ext_end <= new_block_count)
        {
            /* Keep entirely */
            logical_pos = ext_end;
            keep_count  = i + 1;
            continue;
        }

        if(logical_pos >= new_block_count)
        {
            /* Free entirely */
            free_extent_phys(ctx, &list[i]);
            continue;
        }

        /* This extent straddles the truncation point */
        uint64_t keep_logical = new_block_count - logical_pos;

        if(list[i].logical_count == list[i].phys_count)
        {
            /* Uncompressed: simply trim the right side */
            for(uint64_t b = keep_logical; b < list[i].phys_count; b++)
                free_single_phys_block(ctx, list[i].phys_start + b);
            list[i].logical_count = keep_logical;
            list[i].phys_count    = keep_logical;
            keep_count            = i + 1;
        }
        else
        {
            /* Compressed extent at truncation boundary:
             * decompress, keep the first keep_logical blocks as uncompressed */
            uint8_t *temp = malloc((size_t)(keep_logical * ctx->sb.block_size));
            if(!temp)
            {
                free(list);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }

            rc = read_extent_blocks(ctx, &list[i], logical_pos, keep_logical, temp);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(list);
                return rc;
            }

            /* Free old compressed extent's physical blocks */
            free_extent_phys(ctx, &list[i]);

            /* Allocate new uncompressed blocks */
            uint64_t new_start;
            rc = obmafs3_alloc_blocks(ctx, keep_logical, &new_start);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(list);
                return rc;
            }

            for(uint64_t b = 0; b < keep_logical; b++)
            {
                rc = obmafs3_block_write(ctx, new_start + b, temp + b * (size_t)ctx->sb.block_size,
                                         (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK)
                {
                    free(temp);
                    free(list);
                    return rc;
                }
            }
            free(temp);

            list[i].phys_start    = new_start;
            list[i].phys_count    = keep_logical;
            list[i].logical_count = keep_logical;
            keep_count            = i + 1;
        }
        logical_pos = new_block_count;
    }

    rc = write_extent_list(ctx, inode, list, keep_count);
    free(list);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Clone / reflink file range                                         */
/* ------------------------------------------------------------------ */

/**
 * Clone a range of data blocks from one inode to another by sharing
 * physical blocks and incrementing their refcounts.
 *
 * Both offsets and length must be aligned to the filesystem block size.
 * Source blocks may reside in inline extents or the overflow B+Tree.
 * The destination's existing blocks in the target range are freed
 * (refcount-aware), and the resulting extent map is rebuilt.
 *
 * Compressed extents that are fully within the clone range are shared
 * as-is (all their physical blocks get a refcount bump).  Compressed
 * extents that only partially overlap the clone range are decompressed
 * and the relevant portion is written as a private uncompressed copy.
 *
 * @param ctx         Filesystem context.
 * @param src_inode   Source inode (read-only).
 * @param src_offset  Byte offset into the source file (block-aligned).
 * @param dst_inode   Destination inode (modified in place).
 * @param dst_offset  Byte offset into the destination file (block-aligned).
 * @param length      Number of bytes to clone (block-aligned).
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_clone_file_range(struct obmafs3_ctx *ctx, const struct inode_record *src_inode, uint64_t src_offset,
                             struct inode_record *dst_inode, uint64_t dst_offset, uint64_t length)
{
    uint64_t block_size = ctx->sb.block_size;
    int      rc;

    /* Validate alignment to block_size (no per-block header any more) */
    if(src_offset % block_size || dst_offset % block_size || length % block_size || length == 0)
        DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

    if(src_inode->inode_id == dst_inode->inode_id) DBG_RETURN(OBMAFS3_ERR_INVAL, "invalid parameter");

    uint64_t num_logical   = length / block_size;
    uint64_t src_log_start = src_offset / block_size;
    uint64_t dst_log_start = dst_offset / block_size;

    /* ---- Collect source extents ---- */
    struct extent_descriptor *src_exts      = NULL;
    uint64_t                  src_ext_count = 0;
    rc                                      = collect_all_extents(ctx, src_inode, &src_exts, &src_ext_count);
    if(rc != OBMAFS3_OK) return rc;

    /* ---- Collect destination extents ---- */
    struct extent_descriptor *dst_exts      = NULL;
    uint64_t                  dst_ext_count = 0;
    rc                                      = collect_all_extents(ctx, dst_inode, &dst_exts, &dst_ext_count);
    if(rc != OBMAFS3_OK)
    {
        free(src_exts);
        return rc;
    }

    /* ---- Build cloned extents from source ---- */
    uint64_t                  clone_cap   = 64;
    struct extent_descriptor *clone_exts  = calloc((size_t)clone_cap, sizeof(*clone_exts));
    uint64_t                  clone_count = 0;
    if(!clone_exts)
    {
        free(src_exts);
        free(dst_exts);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    uint64_t src_range_end = src_log_start + num_logical;

    for(uint64_t i = 0; i < src_ext_count; i++)
    {
        struct extent_descriptor *se     = &src_exts[i];
        uint64_t                  se_end = se->logical_start + se->logical_count;

        if(se_end <= src_log_start || se->logical_start >= src_range_end) continue;

        uint64_t overlap_start = (se->logical_start > src_log_start) ? se->logical_start : src_log_start;
        uint64_t overlap_end   = (se_end < src_range_end) ? se_end : src_range_end;
        uint64_t overlap_count = overlap_end - overlap_start;

        /* Remap to destination logical space */
        uint64_t dst_log = dst_log_start + (overlap_start - src_log_start);

        if(clone_count >= clone_cap)
        {
            clone_cap                     = clone_cap * 2;
            struct extent_descriptor *tmp = realloc(clone_exts, (size_t)clone_cap * sizeof(*tmp));
            if(!tmp)
            {
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }
            clone_exts = tmp;
        }

        if(se->logical_count == se->phys_count)
        {
            /* Uncompressed: share the overlapping physical blocks */
            uint64_t phys_off                     = overlap_start - se->logical_start;
            clone_exts[clone_count].logical_start = dst_log;
            clone_exts[clone_count].logical_count = overlap_count;
            clone_exts[clone_count].phys_start    = se->phys_start + phys_off;
            clone_exts[clone_count].phys_count    = overlap_count;

            for(uint64_t b = 0; b < overlap_count; b++)
            {
                rc = obmafs3_refcount_inc(ctx, se->phys_start + phys_off + b);
                if(rc != OBMAFS3_OK)
                {
                    free(clone_exts);
                    free(src_exts);
                    free(dst_exts);
                    return rc;
                }
            }
            clone_count++;
        }
        else if(overlap_start == se->logical_start && overlap_count == se->logical_count)
        {
            /* Compressed extent fully within range: share it whole */
            clone_exts[clone_count].logical_start = dst_log;
            clone_exts[clone_count].logical_count = se->logical_count;
            clone_exts[clone_count].phys_start    = se->phys_start;
            clone_exts[clone_count].phys_count    = se->phys_count;

            for(uint64_t b = 0; b < se->phys_count; b++)
            {
                rc = obmafs3_refcount_inc(ctx, se->phys_start + b);
                if(rc != OBMAFS3_OK)
                {
                    free(clone_exts);
                    free(src_exts);
                    free(dst_exts);
                    return rc;
                }
            }
            clone_count++;
        }
        else
        {
            /* Partial compressed extent: decompress and write
             * private uncompressed copy of the overlapping portion */
            uint8_t *temp = malloc((size_t)(overlap_count * block_size));
            if(!temp)
            {
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
            }

            rc = read_extent_blocks(ctx, se, overlap_start, overlap_count, temp);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                return rc;
            }

            uint64_t new_start;
            rc = obmafs3_alloc_blocks(ctx, overlap_count, &new_start);
            if(rc != OBMAFS3_OK)
            {
                free(temp);
                free(clone_exts);
                free(src_exts);
                free(dst_exts);
                return rc;
            }

            for(uint64_t b = 0; b < overlap_count; b++)
            {
                rc = obmafs3_block_write(ctx, new_start + b, temp + b * (size_t)block_size, (size_t)block_size);
                if(rc != OBMAFS3_OK)
                {
                    free(temp);
                    free(clone_exts);
                    free(src_exts);
                    free(dst_exts);
                    return rc;
                }
            }
            free(temp);

            clone_exts[clone_count].logical_start = dst_log;
            clone_exts[clone_count].logical_count = overlap_count;
            clone_exts[clone_count].phys_start    = new_start;
            clone_exts[clone_count].phys_count    = overlap_count;
            clone_count++;
        }
    }
    free(src_exts);

    /* ---- Free destination blocks in the clone range ---- */
    uint64_t dst_range_end = dst_log_start + num_logical;
    for(uint64_t i = 0; i < dst_ext_count; i++)
    {
        struct extent_descriptor *de     = &dst_exts[i];
        uint64_t                  de_end = de->logical_start + de->logical_count;

        if(de_end <= dst_log_start || de->logical_start >= dst_range_end) continue;

        if(de->logical_count == de->phys_count)
        {
            /* Uncompressed: free only the overlapping physical blocks */
            uint64_t overlap_start = (de->logical_start > dst_log_start) ? de->logical_start : dst_log_start;
            uint64_t overlap_end   = (de_end < dst_range_end) ? de_end : dst_range_end;
            uint64_t phys_off      = overlap_start - de->logical_start;

            for(uint64_t b = 0; b < overlap_end - overlap_start; b++)
                free_single_phys_block(ctx, de->phys_start + phys_off + b);
        }
        else
        {
            /* Compressed dest extent overlapping the clone range: free entirely */
            free_extent_phys(ctx, de);
        }
    }

    /* ---- Build new destination extent list ---- */
    uint64_t                  new_cap  = dst_ext_count + clone_count + 16;
    struct extent_descriptor *new_list = calloc((size_t)new_cap, sizeof(*new_list));
    if(!new_list)
    {
        free(clone_exts);
        free(dst_exts);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }
    uint64_t new_count = 0;

    /* Add surviving destination extents before the clone range */
    for(uint64_t i = 0; i < dst_ext_count; i++)
    {
        struct extent_descriptor *de     = &dst_exts[i];
        uint64_t                  de_end = de->logical_start + de->logical_count;

        if(de_end <= dst_log_start)
        {
            new_list[new_count++] = *de;
            continue;
        }

        /* Keep the left part of an uncompressed extent that straddles the start */
        if(de->logical_start < dst_log_start && de->logical_count == de->phys_count)
        {
            uint64_t keep                     = dst_log_start - de->logical_start;
            new_list[new_count]               = *de;
            new_list[new_count].logical_count = keep;
            new_list[new_count].phys_count    = keep;
            new_count++;
        }
    }

    /* Add cloned extents */
    for(uint64_t i = 0; i < clone_count; i++) new_list[new_count++] = clone_exts[i];

    /* Add surviving destination extents after the clone range */
    for(uint64_t i = 0; i < dst_ext_count; i++)
    {
        struct extent_descriptor *de     = &dst_exts[i];
        uint64_t                  de_end = de->logical_start + de->logical_count;

        if(de->logical_start >= dst_range_end)
        {
            new_list[new_count++] = *de;
            continue;
        }

        /* Keep the right part of an uncompressed extent that straddles the end */
        if(de_end > dst_range_end && de->logical_count == de->phys_count)
        {
            uint64_t skip                     = dst_range_end - de->logical_start;
            new_list[new_count].logical_start = dst_range_end;
            new_list[new_count].logical_count = de->logical_count - skip;
            new_list[new_count].phys_start    = de->phys_start + skip;
            new_list[new_count].phys_count    = de->phys_count - skip;
            new_count++;
        }
    }

    free(clone_exts);
    free(dst_exts);

    rc = write_extent_list(ctx, dst_inode, new_list, new_count);
    free(new_list);
    if(rc != OBMAFS3_OK) return rc;

    uint64_t new_end = dst_offset + length;
    if(new_end > dst_inode->file_size) dst_inode->file_size = new_end;

    return OBMAFS3_OK;
}
