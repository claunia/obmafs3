// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_write.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Background compression, dedup data blocks, sector map writing,
//     media image write path, and sector map cache flush/free.
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
/*  Background compression via the shared pool                         */
/* ------------------------------------------------------------------ */

/**
 * Argument block for a dedup compression async job.
 * Allocated by dedup_bg_submit(), freed by dedup_bg_wait().
 */
struct dedup_compress_arg
{
    struct obmafs3_ctx *ctx;
    uint8_t            *data;       /**< Buffer to compress and write (owned) */
    uint64_t            block_lba;  /**< Destination LBA */
    uint64_t            offset;     /**< Byte offset (end of payload) */
    uint64_t            capacity;   /**< Full dedup block size */
    uint64_t            free_lba;   /**< Trailing blocks to free (set by worker) */
    uint64_t            free_count; /**< Number of trailing blocks (set by worker) */
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
            int    crc = obmafs3_compress(cctx, data + sizeof(bhdr), (size_t)bhdr.original_size, comp_buf, &comp_size,
                                          ctx->zstd_level);
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
int dedup_bg_wait(struct obmafs3_ctx *ctx, void **pjob)
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
int dedup_bg_submit(struct compress_pool *pool, void **pjob, struct obmafs3_ctx *ctx, uint8_t *data, uint64_t block_lba,
                    uint64_t offset, uint64_t capacity)
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
        free(da->data); /* da took ownership of the data buffer */
        free(da);
        return OBMAFS3_ERR_NOMEM;
    }

    obmafs3_pool_submit_async(pool, job);
    *pjob = job;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Dedup data block management                                        */
/* ------------------------------------------------------------------ */

/**
 * Initialize the in-memory dedup block from the tree header.
 * If last_block_lba != 0, reads the partial block from disk.
 * Otherwise sets up for a fresh allocation.
 */
int dedup_block_init(struct obmafs3_ctx *ctx, const struct btree_header *hdr, struct dedup_block_ctx *db)
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
            rc = obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, db->data + sizeof(bhdr),
                                    (size_t)bhdr.compressed_size, temp, (size_t)bhdr.original_size);
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
int dedup_block_flush(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db)
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
            int    crc       = obmafs3_compress(obmafs3_get_thread_bufs(ctx)->zstd_cctx, db->data + sizeof(bhdr),
                                                (size_t)bhdr.original_size, comp_buf, &comp_size, ctx->zstd_level);
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
int dedup_block_new(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db)
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
int dedup_block_store(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db, const void *sector_data, size_t sector_len,
                      uint64_t *out_lba, uint64_t *out_offset, struct compress_pool *pool, void **pending_job)
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
void dedup_block_free(struct dedup_block_ctx *db)
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
int write_sector_map_batch(struct obmafs3_ctx *ctx, struct inode_record *inode, const struct sector_map_entry *entries,
                           uint64_t count)
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
     * header, skip the expensive disk read entirely.
     *
     * Lock for first-call setup that touches shared state (tree list,
     * block init).  Uses recursive mutex — safe when the caller already
     * holds the lock (e.g. ioctl path). */
    int cold_setup = (!db_cache || !db_cache->hdr_cached || !db_cache->initialized);
    if(cold_setup) pthread_rwlock_wrlock(&ctx->tree_lock);

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
        if(rc != OBMAFS3_OK)
        {
            pthread_rwlock_unlock(&ctx->tree_lock);
            return rc;
        }
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
            if(rc != OBMAFS3_OK)
            {
                pthread_rwlock_unlock(&ctx->tree_lock);
                return rc;
            }
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
        if(rc != OBMAFS3_OK)
        {
            pthread_rwlock_unlock(&ctx->tree_lock);
            return rc;
        }
        db = &db_local;
    }

    if(cold_setup) pthread_rwlock_unlock(&ctx->tree_lock);

    /*
     * Pre-allocate a buffer for sector_map_entries.
     * When cache is NULL but db_cache is provided (CD image path),
     * the caller manages its own sector map — skip entirely.
     * Maximum number of sectors in this write = size / sector_size + 1.
     */
    int                      skip_sme  = (cache == NULL && db_cache != NULL);
    struct sector_map_entry *sme_buf   = NULL;
    uint64_t                 sme_count = 0;
    if(!skip_sme)
    {
        uint64_t max_sectors = size / sector_size + 1;
        sme_buf              = malloc((size_t)(max_sectors * sizeof(struct sector_map_entry)));
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
     * create the node cache and key set inline.  Double-check under
     * the lock to avoid creating two caches concurrently. */
    if(!ctx->dedup_node_cache)
    {
        pthread_rwlock_wrlock(&ctx->tree_lock);
        if(!ctx->dedup_node_cache)
        {
            struct dedup_node_cache *nc = dedup_cache_create((size_t)ctx->sb.block_size);
            if(nc) ctx->dedup_node_cache = nc;
        }
        pthread_rwlock_unlock(&ctx->tree_lock);
    }

    const uint8_t *data = (const uint8_t *)buf;
    size_t         bsz  = (size_t)ctx->sb.block_size;

    /* Use the thread-local node buffer for dedup tree traversal. */
    uint8_t *tree_buf = obmafs3_get_thread_bufs(ctx)->node_buf;

    struct timespec t_lock_start, t_lock_end;
    struct timespec t_prefetch_start, t_prefetch_end;
    struct timespec t_phase1_start, t_phase1_end;
    struct timespec t_phase2_start;
    struct timespec t_bgwait_end, t_flush_end, t_ncflush_end, t_hdr_end, t_sme_end;
    uint64_t        dedup_hits = 0, dedup_misses = 0;
    uint64_t        prefetch_advised = 0;

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
        size_t   bp = 0;
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

    /*
     * Fast-path: check how many hashes are already in the key set
     * or the pending insert buffer.
     * If all are known, we can skip the entire prefetch phase —
     * the Phase 1 loop will resolve them via keyset_contains()
     * without any tree traversal or disk I/O.
     */
    uint64_t keyset_fast_hits = 0;
    {
        const struct dedup_key_set     *ks       = (const struct dedup_key_set *)ctx->dedup_key_set;
        const struct dedup_pending_buf *pb       = (const struct dedup_pending_buf *)ctx->dedup_pending;
        const struct dedup_pending_buf *drain_pb = (const struct dedup_pending_buf *)ctx->dedup_pending_draining;
        for(uint64_t i = 0; i < num_sectors; i++)
        {
            if((ks && keyset_contains(ks, sw[i].hash)) || pending_lookup(pb, sw[i].hash) ||
               pending_lookup(drain_pb, sw[i].hash))
                keyset_fast_hits++;
        }
    }

    /*
     * === CRITICAL SECTION ===
     * From here through Phase 2 we touch shared mutable state (node
     * cache, key set inserts, B+Tree, bitmap, dedup block store).
     * Hashing, sorting and key-set reads above were lock-free.
     */
    clock_gettime(CLOCK_MONOTONIC, &t_lock_start);
    pthread_rwlock_wrlock(&ctx->tree_lock);
    clock_gettime(CLOCK_MONOTONIC, &t_lock_end);

    /* Refresh the dedup header from disk under the lock.
     * The housekeeping thread may have modified the tree (splits,
     * node allocations) since we cached the header, so free_node_lba
     * and root_node_lba could be stale. */
    {
        struct btree_header fresh_hdr;
        int                 rrc = obmafs3_btree_header_read(ctx, dedup_hdr_lba, &fresh_hdr);
        if(rrc == OBMAFS3_OK)
        {
            dedup_hdr.root_node_lba = fresh_hdr.root_node_lba;
            dedup_hdr.free_node_lba = fresh_hdr.free_node_lba;
            dedup_hdr.free_nodes    = fresh_hdr.free_nodes;
            dedup_hdr.total_nodes   = fresh_hdr.total_nodes;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_prefetch_start);

    /*
     * Leaf prefetch: use the pre-computed hashes to find target leaf
     * LBAs and batch-prefetch them with posix_fadvise.
     * Skipped entirely when:
     *  - all hashes are already known (keyset + pending hits), OR
     *  - both keyset and pending buffer are available (Phase 1 will
     *    use the deferred-insert path for misses — no tree I/O).
     */
    const int have_deferred_path = (ctx->dedup_pending != NULL && ctx->dedup_key_set != NULL);
    if(keyset_fast_hits < num_sectors && !have_deferred_path)
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
                    posix_fadvise(ctx->fd, (off_t)(pf_lbas[i] * bsz), (off_t)bsz, POSIX_FADV_WILLNEED);
                    prefetch_advised++;
                }

                prev_lba = 0;
                for(uint64_t i = 0; i < num_sectors; i++)
                {
                    if(pf_lbas[i] == 0 || pf_lbas[i] == prev_lba) continue;
                    prev_lba                      = pf_lbas[i];
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

    /* Remember root LBA before Phase 1 — if a root split changes it
     * we must flush tree nodes before writing the header. */
    uint64_t original_root = dedup_hdr.root_node_lba;

    clock_gettime(CLOCK_MONOTONIC, &t_phase1_start);

    /*
     * Phase 1: Process sectors in hash-sorted order — dedup lookup,
     *          store new sectors, insert into tree (or buffer).
     *          Hash ordering groups accesses to the same leaf node,
     *          so consecutive inserts typically share a single cached
     *          leaf read.
     *
     *          When the pending insert buffer is available and the
     *          keyset says "miss", the B+Tree insert is deferred to
     *          the buffer — zero tree I/O on the hot write path.
     *
     *          sector_map_entries are filled at their original index
     *          to preserve positional ordering for reads.
     */
    sme_count                                  = num_sectors; /* all slots will be filled */
    uint64_t                  pending_deferred = 0;
    struct dedup_pending_buf *pb               = (struct dedup_pending_buf *)ctx->dedup_pending;
    for(uint64_t si = 0; si < num_sectors; si++)
    {
        uint32_t       idx   = sorted_idx[si];
        uint64_t       hash  = sw[idx].hash;
        const uint8_t *sdata = sw[idx].data;
        size_t         slen  = sw[idx].len;
        int64_t        snum  = sw[idx].sector_num;

        /* Fast path: if the key set or pending buffer confirms this
         * hash exists, skip tree traversal — zero disk I/O. */
        struct dedup_key_set           *ks        = (struct dedup_key_set *)ctx->dedup_key_set;
        const struct dedup_pending_buf *drain_pb2 = (const struct dedup_pending_buf *)ctx->dedup_pending_draining;
        if(keyset_contains(ks, hash) || pending_lookup(pb, hash) || pending_lookup(drain_pb2, hash)) { dedup_hits++; }
        else if(pb && ks)
        {
            /* If sector_size changed (different dedup tree), flush first. */
            if(pb->sector_size != 0 && pb->sector_size != sector_size && pb->count > 0)
            {
                struct btree_header flush_hdr;
                uint64_t            flush_hdr_lba;
                int                 frc = obmafs3_dedup_get_tree(ctx, pb->sector_size, &flush_hdr, &flush_hdr_lba);
                if(frc == OBMAFS3_OK) frc = pending_flush(pb, ctx, &flush_hdr, flush_hdr_lba);
                /* Only reset sector_size when flush succeeded;
                 * otherwise keep old entries for retry / persistence. */
                if(frc == OBMAFS3_OK || pb->count == 0) pb->sector_size = 0;
            }

            /* Keyset says "miss" and pending buffer is available.
             * Store the data and defer the B+Tree insert. */
            dedup_misses++;
            uint64_t              stored_lba, stored_offset;
            struct compress_pool *pool = db_cache ? ctx->compress_pool : NULL;
            void                **pjob = db_cache ? &db_cache->pending_job : NULL;
            rc = dedup_block_store(ctx, db, sdata, slen, &stored_lba, &stored_offset, pool, pjob);
            if(rc != OBMAFS3_OK)
            {
                free(sw);
                free(sorted_idx);
                goto out;
            }

            struct dedup_entry new_entry;
            new_entry.hash         = hash;
            new_entry.block_lba    = stored_lba;
            new_entry.block_offset = stored_offset;

            /* Buffer the insert for later batch flush. */
            if(pb->sector_size == 0) pb->sector_size = sector_size;
            pending_insert(pb, &new_entry);
            keyset_insert(ks, hash);
            pending_deferred++;
        }
        else
        {
            /* No pending buffer (or no keyset) — fall back to immediate
             * tree insert (original path). */
            struct dedup_entry       existing;
            struct dedup_upsert_ctx  uctx;
            struct dedup_node_cache *nc = (struct dedup_node_cache *)ctx->dedup_node_cache;
            rc                          = dedup_upsert_find(ctx, &dedup_hdr, hash, &existing, &uctx, tree_buf, nc);

            if(rc == OBMAFS3_OK) { dedup_hits++; }
            else if(rc == OBMAFS3_ERR_NOTFOUND)
            {
                dedup_misses++;
                uint64_t              stored_lba, stored_offset;
                struct compress_pool *pool = db_cache ? ctx->compress_pool : NULL;
                void                **pjob = db_cache ? &db_cache->pending_job : NULL;
                rc = dedup_block_store(ctx, db, sdata, slen, &stored_lba, &stored_offset, pool, pjob);
                if(rc != OBMAFS3_OK)
                {
                    free(sw);
                    free(sorted_idx);
                    goto out;
                }

                struct dedup_entry new_entry;
                new_entry.hash         = hash;
                new_entry.block_lba    = stored_lba;
                new_entry.block_offset = stored_offset;

                rc = dedup_upsert_insert(ctx, &dedup_hdr, &new_entry, &uctx, tree_buf, nc);
                if(rc != OBMAFS3_OK)
                {
                    free(sw);
                    free(sorted_idx);
                    goto out;
                }

                /* Add the newly inserted key to the set for future lookups */
                if(ks) keyset_insert(ks, hash);
            }
            else
            {
                free(sw);
                free(sorted_idx);
                goto out;
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

    /* Flush cached tree nodes — deferred to reduce I/O.
     * Forced when the root changed (header needs a valid root on disk),
     * when the dirty count exceeds a threshold, or periodically after
     * DEDUP_NC_FLUSH_INTERVAL writes to bound unflushed state. */
    if(ctx->dedup_node_cache)
    {
        struct dedup_node_cache *nc_flush = (struct dedup_node_cache *)ctx->dedup_node_cache;
        if(nc_flush->dirty_count > 0)
        {
            int must_flush = (dedup_hdr.root_node_lba != original_root) ||
                             (nc_flush->writes_since_flush >= DEDUP_NC_FLUSH_INTERVAL) ||
                             (nc_flush->dirty_count >= DEDUP_NC_DIRTY_THRESHOLD);
            if(must_flush)
            {
                int nc_rc = dedup_cache_flush(nc_flush, ctx);
                if(rc == OBMAFS3_OK) rc = nc_rc;
            }
            else
            {
                nc_flush->writes_since_flush++;
            }
        }
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

    pthread_rwlock_unlock(&ctx->tree_lock);
    /* === END CRITICAL SECTION === */

    free(sme_buf);

    /* Print timing instrumentation */
    {
        struct dedup_node_cache *nc       = (struct dedup_node_cache *)ctx->dedup_node_cache;
        struct dedup_key_set    *ks       = (struct dedup_key_set *)ctx->dedup_key_set;
        uint32_t                 nc_count = nc ? nc->count : 0;
        uint32_t                 nc_cap   = nc ? nc->capacity : 0;
        uint32_t                 ks_count = ks ? ks->count : 0;
        uint32_t                 ks_cap   = ks ? ks->capacity : 0;
        fprintf(stderr,
                "[dedup-timing] write %zu bytes @ %" PRIu64 ": "
                "lock_wait=%.1fms  "
                "prefetch=%.1fms(%" PRIu64 " leaves)  "
                "phase1=%.1fms  bg_wait=%.1fms  blk_flush=%.1fms  "
                "nc_flush=%.1fms  hdr=%.1fms  sme=%.1fms  TOTAL=%.1fms  "
                "hits=%" PRIu64 " misses=%" PRIu64 " pending=%" PRIu64 " nc_count=%u/%u ks=%u/%u ks_fast=%" PRIu64 "\n",
                size, offset, timespec_diff_ms(&t_lock_start, &t_lock_end),
                timespec_diff_ms(&t_prefetch_start, &t_prefetch_end), prefetch_advised,
                timespec_diff_ms(&t_phase1_start, &t_phase1_end), timespec_diff_ms(&t_phase2_start, &t_bgwait_end),
                timespec_diff_ms(&t_bgwait_end, &t_flush_end), timespec_diff_ms(&t_flush_end, &t_ncflush_end),
                timespec_diff_ms(&t_ncflush_end, &t_hdr_end), timespec_diff_ms(&t_hdr_end, &t_sme_end),
                timespec_diff_ms(&t_lock_start, &t_sme_end), dedup_hits, dedup_misses, pending_deferred, nc_count,
                nc_cap, ks_count, ks_cap, keyset_fast_hits);
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
    /* Error path — wait for bg, then flush dedup state.
     * tree_lock is held (goto out is reachable only from Phase 1). */
    if(db_cache && db_cache->pending_job)
    {
        int bg_rc = dedup_bg_wait(ctx, &db_cache->pending_job);
        if(bg_rc != OBMAFS3_OK && rc == OBMAFS3_OK) rc = bg_rc;
    }
    if(db->data && db->dirty) dedup_block_flush(ctx, db);

    /* Flush cached tree nodes even on error to keep disk consistent.
     * Only write the tree header when the cache flush succeeds —
     * if dirty nodes could not be persisted, writing a header
     * that references them would leave the on-disk tree broken. */
    if(ctx->dedup_node_cache)
    {
        int flush_rc = dedup_cache_flush((struct dedup_node_cache *)ctx->dedup_node_cache, ctx);
        if(flush_rc == OBMAFS3_OK)
        {
            dedup_hdr.last_block_lba    = db->block_lba;
            dedup_hdr.last_block_offset = db->offset;
            obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);
        }
        else
        {
            fprintf(stderr, "ERROR: cache flush failed on error path — "
                            "skipping header write to preserve on-disk consistency\n");
        }
    }

    /* Keep the cached header in sync */
    if(db_cache) db_cache->dedup_hdr = dedup_hdr;

    pthread_rwlock_unlock(&ctx->tree_lock);

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
