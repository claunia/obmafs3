// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : io.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 context management, block I/O, creation, and checking.
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

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

/* Global debug flag (default off; set OBMAFS3_DEBUG=1 to enable). */
int obmafs3_debug = 0;

/* ------------------------------------------------------------------ */
/*  Thread-local scratch buffers                                       */
/* ------------------------------------------------------------------ */

/**
 * Destructor for per-thread scratch buffers.
 * Called automatically by pthreads when a thread exits.
 */
static void thread_bufs_destroy(void *ptr)
{
    struct obmafs3_thread_bufs *tb = (struct obmafs3_thread_bufs *)ptr;
    if(!tb) return;
    free(tb->hdr_buf);
    free(tb->node_buf);
    free(tb->io_buf);
    free(tb->io_buf2);
    free(tb->comp_buf);
    if(tb->zstd_cctx) ZSTD_freeCCtx(tb->zstd_cctx);
    if(tb->zstd_probe_cctx) ZSTD_freeCCtx(tb->zstd_probe_cctx);
    if(tb->zstd_dctx) ZSTD_freeDCtx(tb->zstd_dctx);
    free(tb);
}

/**
 * Get (or lazily allocate) the calling thread's scratch buffers.
 *
 * Each thread gets its own hdr_buf, node_buf, io_buf, io_buf2,
 * comp_buf, and ZSTD contexts so that concurrent FUSE callbacks do not
 * stomp on each other.  Buffers are freed automatically when the thread
 * exits (via the pthread_key destructor).
 */
struct obmafs3_thread_bufs *obmafs3_get_thread_bufs(struct obmafs3_ctx *ctx)
{
    struct obmafs3_thread_bufs *tb = (struct obmafs3_thread_bufs *)pthread_getspecific(ctx->tls_key);
    if(tb) return tb;

    tb = calloc(1, sizeof(*tb));
    if(!tb) return NULL;

    size_t bs          = (size_t)ctx->sb.block_size;
    size_t group_bytes = bs * OBMAFS3_COMPRESS_GROUP_BLOCKS;
    size_t comp_need   = sizeof(struct block_header) + ZSTD_compressBound(group_bytes);
    if(comp_need < group_bytes) comp_need = group_bytes;

    tb->hdr_buf         = malloc(bs);
    tb->node_buf        = malloc(bs);
    tb->io_buf          = malloc(group_bytes);
    tb->io_buf2         = malloc(group_bytes);
    tb->comp_buf        = malloc(comp_need);
    tb->comp_buf_size   = comp_need;
    tb->zstd_cctx       = ZSTD_createCCtx();
    tb->zstd_probe_cctx = ZSTD_createCCtx();
    tb->zstd_dctx       = ZSTD_createDCtx();

    if(!tb->hdr_buf || !tb->node_buf || !tb->io_buf || !tb->io_buf2 || !tb->comp_buf || !tb->zstd_cctx ||
       !tb->zstd_probe_cctx || !tb->zstd_dctx)
    {
        thread_bufs_destroy(tb);
        return NULL;
    }

    pthread_setspecific(ctx->tls_key, tb);
    return tb;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Generate an RFC 4122 version 4 GUID from /dev/urandom. */
static void generate_guid(uint8_t *guid)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd >= 0)
    {
        if(read(fd, guid, 16) != 16) memset(guid, 0, 16);
        close(fd);
    }
    else
    {
        memset(guid, 0, 16);
    }
    /* RFC 4122 version 4 */
    guid[6] = (guid[6] & 0x0F) | 0x40;
    guid[8] = (guid[8] & 0x3F) | 0x80;
}

/* ------------------------------------------------------------------ */
/*  Block I/O                                                          */
/* ------------------------------------------------------------------ */

/**
 * Read a block from the filesystem image.
 *
 * @param ctx   Filesystem context.
 * @param lba   Logical block address to read.
 * @param buf   Output buffer.
 * @param size  Number of bytes to read.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on failure.
 */
int obmafs3_block_read(struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t size)
{
    off_t   offset = (off_t)(lba * ctx->sb.block_size);
    ssize_t n      = pread(ctx->fd, buf, size, offset);
    if(n < 0 || (size_t)n != size)
        DBG_RETURN_ERRNO(OBMAFS3_ERR_IO, "pread lba=%" PRIu64 " offset=%" PRId64 " size=%zu got=%zd", lba,
                         (int64_t)offset, size, n);
    return OBMAFS3_OK;
}

/**
 * Write a block to the filesystem image.
 *
 * @param ctx   Filesystem context.
 * @param lba   Logical block address to write.
 * @param buf   Data buffer to write.
 * @param size  Number of bytes to write.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on failure.
 */
int obmafs3_block_write(struct obmafs3_ctx *ctx, uint64_t lba, const void *buf, size_t size)
{
    off_t   offset = (off_t)(lba * ctx->sb.block_size);
    ssize_t n      = pwrite(ctx->fd, buf, size, offset);
    if(n < 0 || (size_t)n != size)
        DBG_RETURN_ERRNO(OBMAFS3_ERR_IO, "pwrite lba=%" PRIu64 " offset=%" PRId64 " size=%zu got=%zd", lba,
                         (int64_t)offset, size, n);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Context open / close                                               */
/* ------------------------------------------------------------------ */

/**
 * Open an existing OBMAFS3 filesystem (default flags).
 *
 * Equivalent to calling @c obmafs3_open_flags with @c flags = 0.
 *
 * @param path  Path to the filesystem image or device.
 * @param ctx   Output filesystem context pointer.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_open(const char *path, struct obmafs3_ctx **ctx) { return obmafs3_open_flags(path, 0, ctx); }

/**
 * Open an existing OBMAFS3 filesystem with flags.
 *
 * Opens the file, reads and validates the superblock, loads all B+Tree
 * headers and (unless @c OBMAFS3_OPEN_SKIP_BITMAP is set) the
 * allocation bitmap.  When @c OBMAFS3_OPEN_LENIENT is set, checksum
 * errors in tree headers are tolerated.
 *
 * @param path   Path to the filesystem image or device.
 * @param flags  Combination of @c OBMAFS3_OPEN_LENIENT and/or
 *               @c OBMAFS3_OPEN_SKIP_BITMAP.
 * @param ctx    Output filesystem context pointer.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_open_flags(const char *path, int flags, struct obmafs3_ctx **ctx)
{
    obmafs3_debug_init();

    int fd = open(path, O_RDWR);
    if(fd < 0) DBG_RETURN_ERRNO(OBMAFS3_ERR_IO, "open(\"%s\", O_RDWR) failed", path);

    struct obmafs3_ctx *c = calloc(1, sizeof(*c));
    if(!c)
    {
        close(fd);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "calloc ctx");
    }

    c->fd               = fd;
    c->compression      = 1;                   /* compression on by default */
    c->compression_algo = kCompressionZstd;     /* ZSTD by default */
    c->zstd_level       = 15;                   /* ZSTD level 15 by default */
    c->lzma_level       = 5;                    /* LZMA level 5 by default */
    c->lzma_dict_size   = 65536;                /* 64 KiB dictionary (matches compression group size) */
    c->warmup_done      = 1;                    /* default: no warmup pending */
    pthread_mutex_init(&c->warmup_mutex, NULL);
    pthread_cond_init(&c->warmup_cond, NULL);

    int rc = obmafs3_sb_read(fd, &c->sb);
    if(rc != OBMAFS3_OK || obmafs3_sb_validate(&c->sb) != OBMAFS3_OK)
    {
        /* Primary superblock unreadable or invalid — try the backup.
         * We need the file size to locate the backup at the last block. */
        off_t file_size = lseek(fd, 0, SEEK_END);
        int   recovered = 0;
        if(file_size > 0)
        {
            /* Try reading the backup with default block size 4096 first,
             * then fall back to 512 .. 65536 in case a non-default
             * block size was used. */
            static const uint64_t try_bs[] = {4096, 512, 1024, 2048, 8192, 16384, 32768, 65536};
            for(int i = 0; i < (int)(sizeof(try_bs) / sizeof(try_bs[0])); i++)
            {
                uint64_t bs = try_bs[i];
                if((uint64_t)file_size < 2 * bs) continue; /* need at least 2 blocks */
                struct obmafs3_sb backup;
                if(obmafs3_sb_read_backup(fd, bs, (uint64_t)file_size, &backup) == OBMAFS3_OK &&
                   obmafs3_sb_validate(&backup) == OBMAFS3_OK && backup.block_size == bs &&
                   backup.total_bytes == (uint64_t)file_size)
                {
                    c->sb     = backup;
                    recovered = 1;
                    break;
                }
            }
        }
        if(!recovered)
        {
            close(fd);
            free(c);
            return (rc != OBMAFS3_OK) ? rc : OBMAFS3_ERR_BADMAGIC;
        }
    }

    /* Reject filesystems created by a newer revision of the format */
    if(!(flags & OBMAFS3_OPEN_LENIENT) && c->sb.revision > OBMAFS3_REVISION)
    {
        fprintf(stderr, "Error: filesystem revision %u is newer than supported revision %u — refusing to mount\n",
                (unsigned)c->sb.revision, (unsigned)OBMAFS3_REVISION);
        close(fd);
        free(c);
        return OBMAFS3_ERR_REVISION;
    }

    /* Check feature compatibility flags */
    if(!(flags & OBMAFS3_OPEN_LENIENT))
    {
        uint64_t unknown_incompat = c->sb.incompatible_flags & ~OBMAFS3_INCOMPAT_FLAGS_KNOWN;
        if(unknown_incompat)
        {
            fprintf(stderr,
                    "Error: filesystem has incompatible feature flags 0x%016" PRIx64
                    " that this implementation does not support — refusing to mount\n",
                    unknown_incompat);
            close(fd);
            free(c);
            return OBMAFS3_ERR_INCOMPAT;
        }

        uint64_t unknown_rocompat = c->sb.rocompat_flags & ~OBMAFS3_ROCOMPAT_FLAGS_KNOWN;
        if(unknown_rocompat)
        {
            fprintf(stderr,
                    "Warning: filesystem has read-only compatible feature flags 0x%016" PRIx64
                    " that this implementation does not support — mounting read-only\n",
                    unknown_rocompat);
            c->read_only = 1;
        }
    }

    /* Initialise thread-local storage key for per-thread scratch buffers
     * and the write serialisation mutex.  Scratch buffers (hdr_buf,
     * node_buf, io_buf, io_buf2, comp_buf, ZSTD contexts) are allocated
     * lazily in obmafs3_get_thread_bufs() the first time a thread needs
     * them, and freed automatically when the thread exits. */
    if(pthread_key_create(&c->tls_key, thread_bufs_destroy) != 0)
    {
        close(fd);
        free(c);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "pthread_key_create");
    }
    pthread_rwlock_init(&c->tree_lock, NULL);
    pthread_mutex_init(&c->bitmap_lock, NULL);
    pthread_mutex_init(&c->sme_backfill_lock, NULL);

    /* Refcount leaf cache buffer (separate from per-thread node_buf so
     * lookups don't clobber the traversal buffer; protected by
     * tree_lock since refcount ops are write-side only). */
    c->rc_leaf_buf   = malloc((size_t)c->sb.block_size);
    c->rc_leaf_valid = 0;

    if(!c->rc_leaf_buf)
    {
        free(c->rc_leaf_buf);
        pthread_key_delete(c->tls_key);
        pthread_mutex_destroy(&c->sme_backfill_lock);
        pthread_mutex_destroy(&c->bitmap_lock);
        pthread_rwlock_destroy(&c->tree_lock);
        close(fd);
        free(c);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    if(flags & OBMAFS3_OPEN_LENIENT)
    {
        int cs_ok;
        rc = obmafs3_btree_header_read_lenient(c, c->sb.catalog_lba, &c->catalog_hdr, &cs_ok);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
        {
            close(fd);
            free(c);
            return rc;
        }

        rc = obmafs3_btree_header_read_lenient(c, c->sb.inode_lba, &c->inode_hdr, &cs_ok);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
        {
            close(fd);
            free(c);
            return rc;
        }

        if(c->sb.overflow_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.overflow_lba, &c->overflow_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.media_tag_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.media_tag_lba, &c->media_tag_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.cd_prefix_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.cd_prefix_lba, &c->cd_prefix_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.cd_suffix_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.cd_suffix_lba, &c->cd_suffix_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.cd_subchannel_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.cd_subchannel_lba, &c->cd_subchannel_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.metadata_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.metadata_lba, &c->metadata_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.metadata_idx_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.metadata_idx_lba, &c->metadata_idx_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.refcount_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.refcount_lba, &c->refcount_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.sector_tag_data_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.sector_tag_data_lba, &c->sector_tag_data_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.sector_tag_ref_lba != 0)
        {
            rc = obmafs3_btree_header_read_lenient(c, c->sb.sector_tag_ref_lba, &c->sector_tag_ref_hdr, &cs_ok);
            if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            {
                close(fd);
                free(c);
                return rc;
            }
        }
    }
    else
    {
        rc = obmafs3_btree_header_read(c, c->sb.catalog_lba, &c->catalog_hdr);
        if(rc != OBMAFS3_OK)
        {
            close(fd);
            free(c);
            return rc;
        }

        rc = obmafs3_btree_header_read(c, c->sb.inode_lba, &c->inode_hdr);
        if(rc != OBMAFS3_OK)
        {
            close(fd);
            free(c);
            return rc;
        }

        if(c->sb.overflow_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.overflow_lba, &c->overflow_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.media_tag_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.media_tag_lba, &c->media_tag_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.cd_prefix_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.cd_prefix_lba, &c->cd_prefix_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.cd_suffix_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.cd_suffix_lba, &c->cd_suffix_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.cd_subchannel_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.cd_subchannel_lba, &c->cd_subchannel_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.metadata_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.metadata_lba, &c->metadata_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.metadata_idx_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.metadata_idx_lba, &c->metadata_idx_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.refcount_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.refcount_lba, &c->refcount_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.sector_tag_data_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.sector_tag_data_lba, &c->sector_tag_data_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.sector_tag_ref_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.sector_tag_ref_lba, &c->sector_tag_ref_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }

        if(c->sb.junk_map_lba != 0)
        {
            rc = obmafs3_btree_header_read(c, c->sb.junk_map_lba, &c->junk_map_hdr);
            if(rc != OBMAFS3_OK)
            {
                close(fd);
                free(c);
                return rc;
            }
        }
    }

    /* Load allocation bitmap (skip when asked, e.g. for fsck) */
    if(!(flags & OBMAFS3_OPEN_SKIP_BITMAP) && c->sb.bitmap_lba != 0 && c->sb.bitmap_blocks != 0)
    {
        rc = obmafs3_bitmap_read(c);
        if(rc != OBMAFS3_OK)
        {
            close(fd);
            free(c);
            return rc;
        }
    }

    /* Initialise the persistent compression thread pool. */
    obmafs3_compress_pool_init(c);

    /* Initialise the dedup B+Tree node cache so that the read path
     * can cache index nodes in memory and avoid repeated pread()
     * syscalls for every tree traversal.  Without this the read path
     * issues 2-3 raw pread() calls per sector lookup (one per tree
     * level) and is orders of magnitude slower. */
    obmafs3_dedup_node_cache_init(c);

    *ctx = c;
    return OBMAFS3_OK;
}

/**
 * Close the filesystem and free all associated resources.
 *
 * Releases the in-memory bitmap, closes the file descriptor, and frees
 * the context structure.
 *
 * @param ctx  Filesystem context (may be NULL).
 */
void obmafs3_close(struct obmafs3_ctx *ctx)
{
    if(!ctx) return;

    /* Signal the warmup thread to abort early if still running. */
    ctx->shutdown_requested = 1;

    /* Shut down the compression thread pool before anything else. */
    fprintf(stderr, "[obmafs3] close: step 1 — compress pool destroy\n");
    fflush(stderr);
    obmafs3_compress_pool_destroy(ctx);

    /* Stop the background housekeeping thread (pending → B+Tree drain). */
    fprintf(stderr, "[obmafs3] close: step 2 — housekeeping stop\n");
    fflush(stderr);
    obmafs3_housekeeping_stop(ctx);

    /* Wait for and join the background warmup thread (if any). */
    fprintf(stderr, "[obmafs3] close: step 3 — warmup thread join\n");
    fflush(stderr);
    obmafs3_dedup_warmup_wait(ctx);
    if(ctx->warmup_started) pthread_join(ctx->warmup_thread, NULL);
    pthread_mutex_destroy(&ctx->warmup_mutex);
    pthread_cond_destroy(&ctx->warmup_cond);

    fprintf(stderr, "[obmafs3] close: step 4 — pending buffer save\n");
    fflush(stderr);

    /* Persist any remaining pending entries to disk (fast sequential
     * write of ~1-2 MiB).  Entries will be loaded and drained into
     * the B+Tree by the housekeeping thread on the next mount. */
    obmafs3_dedup_pending_flush_and_free(ctx);

    fprintf(stderr, "[obmafs3] close: step 5 — keyset save (warmup_done=%d)\n", ctx->warmup_done);
    fflush(stderr);

    /* Persist the dedup key set to disk before freeing it.
     * Must happen before bitmap/sb write since it allocates blocks
     * and updates sb.keyset_lba / sb.keyset_blocks.
     * Only save if warmup completed — a partial keyset is worse than
     * none because the next mount would skip the tree scan. */
    if(ctx->bitmap && ctx->fd >= 0 && ctx->dedup_key_set && ctx->warmup_done && !ctx->warmup_running)
    {
        int ks_rc = obmafs3_dedup_keyset_save(ctx);
        if(ks_rc != OBMAFS3_OK) fprintf(stderr, "[dedup-keyset] save failed (rc=%d)\n", ks_rc);
    }
    else if(ctx->dedup_key_set && (!ctx->warmup_done || ctx->warmup_running))
    {
        fprintf(stderr, "[dedup-keyset] warmup was incomplete — skipping keyset persist\n");
    }

    /* Free the global dedup B+Tree node cache and key set. */
    obmafs3_dedup_node_cache_free(ctx);
    obmafs3_dedup_key_set_free(ctx);

    /* Persist the allocation bitmap and superblock on close (unmount).
     * During normal operation these are deferred from the hot alloc/free
     * paths to avoid per-operation I/O overhead. */
    if(ctx->bitmap && ctx->fd >= 0)
    {
        obmafs3_bitmap_write(ctx);
        obmafs3_sb_write(ctx->fd, &ctx->sb);
    }

    /* Free the calling thread's TLS buffers (the destructor won't fire
     * for the thread that calls close, since the key is about to be
     * deleted).  Other threads' buffers are freed by the pthreads
     * destructor when those threads exit. */
    {
        struct obmafs3_thread_bufs *tb = (struct obmafs3_thread_bufs *)pthread_getspecific(ctx->tls_key);
        if(tb)
        {
            pthread_setspecific(ctx->tls_key, NULL);
            thread_bufs_destroy(tb);
        }
    }
    pthread_key_delete(ctx->tls_key);
    pthread_mutex_destroy(&ctx->sme_backfill_lock);
    pthread_mutex_destroy(&ctx->bitmap_lock);
    pthread_rwlock_destroy(&ctx->tree_lock);

    free(ctx->rc_leaf_buf);
    if(ctx->bitmap) free(ctx->bitmap);
    if(ctx->fd >= 0) close(ctx->fd);
    free(ctx);
}

/* ------------------------------------------------------------------ */
/*  Filesystem creation (mkobmafs)                                     */
/* ------------------------------------------------------------------ */

/**
 * Write a zero-padded block during filesystem creation.
 *
 * Copies up to @p data_size bytes from @p data into a zeroed block
 * buffer and writes it to the given LBA.
 *
 * @param fd         Open file descriptor.
 * @param block_size Block size in bytes.
 * @param lba        Logical block address to write.
 * @param data       Source data.
 * @param data_size  Number of bytes to copy from @p data.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int write_block(int fd, uint64_t block_size, uint64_t lba, const void *data, size_t data_size)
{
    uint8_t *block = calloc(1, (size_t)block_size);
    if(!block) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    if(data_size > (size_t)block_size) data_size = (size_t)block_size;
    memcpy(block, data, data_size);

    off_t   offset = (off_t)(lba * block_size);
    ssize_t n      = pwrite(fd, block, (size_t)block_size, offset);
    free(block);

    if(n < 0 || (size_t)n != (size_t)block_size) DBG_RETURN(OBMAFS3_ERR_IO, "I/O error");
    return OBMAFS3_OK;
}

/**
 * Create a new OBMAFS3 filesystem.
 *
 * Initialises the superblock, B+Tree headers, root inode, allocation
 * bitmap, and all on-disk structures in the file at @p path.  If the
 * path does not point to a block device the file is created/truncated.
 *
 * @param path             Path to the image file or block device.
 * @param total_size       Total filesystem size in bytes.
 * @param block_size       Block size in bytes.
 * @param dedup_block_size Deduplication block size in bytes.
 * @param label            Volume label string.
 * @param guid             Filesystem GUID (16 bytes), or NULL for random.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_create(const char *path, uint64_t total_size, uint64_t block_size, uint64_t dedup_block_size,
                   const char *label, const uint8_t *guid)
{
    struct stat st;
    int         is_blkdev = 0;
    int         fd;

    if(stat(path, &st) == 0 && S_ISBLK(st.st_mode))
    {
        is_blkdev = 1;
        fd        = open(path, O_RDWR);
    }
    else
    {
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    }
    if(fd < 0) DBG_RETURN(OBMAFS3_ERR_IO, "I/O error");

    if(!is_blkdev && ftruncate(fd, (off_t)total_size) < 0)
    {
        close(fd);
        DBG_RETURN(OBMAFS3_ERR_IO, "I/O error");
    }

    int rc;

    /* --- Block 0: Superblock --- */
    struct obmafs3_sb sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = OBMAFS3_SB_MAGIC;
    if(guid)
        memcpy(sb.guid, guid, 16);
    else
        generate_guid(sb.guid);
    sb.block_size        = block_size;
    sb.dedup_block_size  = dedup_block_size;
    sb.total_bytes       = total_size;
    sb.catalog_lba       = 1;  /* block 1 */
    sb.inode_lba         = 3;  /* block 3 */
    sb.overflow_lba      = 5;  /* block 5 */
    sb.dedup_lba         = 6;  /* block 6 */
    sb.metadata_lba      = 11; /* block 11 */
    sb.media_tag_lba     = 7;  /* block 7 */
    sb.cd_prefix_lba     = 8;  /* block 8 */
    sb.cd_suffix_lba     = 9;  /* block 9 */
    sb.cd_subchannel_lba = 10; /* block 10 */
    sb.metadata_idx_lba  = 12; /* block 12 */
    sb.refcount_lba      = 13; /* block 13 */
    sb.checksum_type     = kChecksumTypeXXH64;
    sb.creation_time     = (uint64_t)time(NULL);
    sb.btree_clump_size  = OBMAFS3_DEFAULT_CLUMP_SIZE;
    sb.dedup_clump_size  = OBMAFS3_DEDUP_CLUMP_SIZE;
    sb.revision          = OBMAFS3_REVISION;

    /* Calculate allocation bitmap size */
    uint64_t total_blocks         = total_size / block_size;
    uint64_t bitmap_bytes         = (total_blocks + 7) / 8;
    size_t   hdr_size             = sizeof(struct bitmap_header);
    /* First block holds header + data, subsequent blocks are pure data */
    uint64_t first_block_capacity = block_size - hdr_size;
    uint64_t bitmap_blks;
    if(bitmap_bytes <= first_block_capacity)
        bitmap_blks = 1;
    else
        bitmap_blks = 1 + (bitmap_bytes - first_block_capacity + block_size - 1) / block_size;

    sb.bitmap_lba    = 16; /* bitmap starts after sector tag headers */
    sb.bitmap_blocks = bitmap_blks;
    sb.next_inode_id = 3; /* root inode is 2, next is 3 */
    strncpy((char *)sb.volume_label, label, sizeof(sb.volume_label) - 1);

    /* Sector tag trees: blocks 14 and 15 (right after refcount header).
     * Set the rocompat flag so older code mounts read-only. */
    sb.sector_tag_data_lba = 14;
    sb.sector_tag_ref_lba  = 15;
    sb.rocompat_flags     |= OBMAFS3_ROCOMPAT_SECTOR_TAGS;

    /* Compute V1 checksum (covers bytes 0..OBMAFS3_SB_V1_SIZE-1) */
    memset(sb.checksum, 0, sizeof(sb.checksum));
    obmafs3_checksum_block(&sb, OBMAFS3_SB_V1_SIZE, sb.checksum);

    /* Compute extension checksum (covers bytes 526..4095) */
    memset(sb.checksum2, 0, sizeof(sb.checksum2));
    obmafs3_checksum_block((const uint8_t *)&sb + OBMAFS3_SB_V1_SIZE,
                           sizeof(sb) - OBMAFS3_SB_V1_SIZE, sb.checksum2);

    rc = write_block(fd, block_size, 0, &sb, sizeof(sb));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 1: Catalog tree header --- */
    struct btree_header cat_hdr;
    memset(&cat_hdr, 0, sizeof(cat_hdr));
    cat_hdr.magic         = OBMAFS3_BTREE_HDR_MAGIC;
    cat_hdr.data_type     = kBtreeDataTypeFilename;
    cat_hdr.root_node_lba = 2;
    cat_hdr.node_size     = (uint16_t)block_size;
    cat_hdr.total_nodes   = 1;
    cat_hdr.tree_type     = kBtreeTypeCatalog;
    /* checksum is already zeroed; hash entire struct, store result */
    obmafs3_checksum_block(&cat_hdr, sizeof(cat_hdr), cat_hdr.checksum);

    rc = write_block(fd, block_size, 1, &cat_hdr, sizeof(cat_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 2: Catalog root node (root directory entry, B+Tree leaf) --- */
    uint8_t *cat_buf = calloc(1, block_size);
    if(!cat_buf)
    {
        close(fd);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    struct btree_node_header cat_node_hdr;
    memset(&cat_node_hdr, 0, sizeof(cat_node_hdr));
    cat_node_hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    cat_node_hdr.record_type = kBtreeDataTypeFilename;
    cat_node_hdr.level       = 0;
    cat_node_hdr.node_keys   = 1;
    cat_node_hdr.keys_length = (uint16_t)sizeof(struct catalog_record);
    memcpy(cat_buf, &cat_node_hdr, sizeof(cat_node_hdr));

    struct catalog_record root_rec;
    memset(&root_rec, 0, sizeof(root_rec));
    root_rec.inode_id       = OBMAFS3_ROOT_INODE_ID;
    root_rec.parent_id      = OBMAFS3_ROOT_INODE_ID;
    root_rec.directory_flag = 1;
    strncpy(root_rec.name, "/", sizeof(root_rec.name) - 1);
    memcpy(cat_buf + sizeof(cat_node_hdr), &root_rec, sizeof(root_rec));

    /* Compute checksum the same way btree.c does:
       zero the checksum field, hash header + keys_length bytes */
    {
        struct btree_node_header *nhdr      = (struct btree_node_header *)cat_buf;
        size_t                    data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
        memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
        obmafs3_checksum_block(cat_buf, data_size, nhdr->checksum);
    }

    rc = write_block(fd, block_size, 2, cat_buf, block_size);
    free(cat_buf);
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 3: Inode tree header --- */
    struct btree_header ino_hdr;
    memset(&ino_hdr, 0, sizeof(ino_hdr));
    ino_hdr.magic         = OBMAFS3_BTREE_HDR_MAGIC;
    ino_hdr.data_type     = kBtreeDataTypeInode;
    ino_hdr.root_node_lba = 4;
    ino_hdr.node_size     = (uint16_t)block_size;
    ino_hdr.total_nodes   = 1;
    ino_hdr.tree_type     = kBtreeTypeInode;
    obmafs3_checksum_block(&ino_hdr, sizeof(ino_hdr), ino_hdr.checksum);

    rc = write_block(fd, block_size, 3, &ino_hdr, sizeof(ino_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 4: Root directory inode (B+Tree leaf with one record) --- */
    uint8_t *ino_buf = calloc(1, block_size);
    if(!ino_buf)
    {
        close(fd);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    struct btree_node_header ino_node_hdr;
    memset(&ino_node_hdr, 0, sizeof(ino_node_hdr));
    ino_node_hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    ino_node_hdr.record_type = kBtreeDataTypeInode;
    ino_node_hdr.level       = 0;
    ino_node_hdr.node_keys   = 1;
    ino_node_hdr.keys_length = (uint16_t)sizeof(struct inode_record);
    memcpy(ino_buf, &ino_node_hdr, sizeof(ino_node_hdr));

    struct inode_record root_ino;
    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.inode_id          = OBMAFS3_ROOT_INODE_ID;
    root_ino.uid               = 0;
    root_ino.gid               = 0;
    root_ino.mode              = 0755;
    root_ino.creation_time     = sb.creation_time;
    root_ino.modification_time = sb.creation_time;
    root_ino.access_time       = sb.creation_time;
    root_ino.file_size         = 0;
    root_ino.file_type         = kFileTypeDirectory;
    root_ino.ref_count         = 1;
    memcpy(ino_buf + sizeof(ino_node_hdr), &root_ino, sizeof(root_ino));

    /* Compute checksum the same way btree.c does:
       zero the checksum field, hash header + keys_length bytes */
    {
        struct btree_node_header *nhdr      = (struct btree_node_header *)ino_buf;
        size_t                    data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
        memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
        obmafs3_checksum_block(ino_buf, data_size, nhdr->checksum);
    }

    rc = write_block(fd, block_size, 4, ino_buf, block_size);
    free(ino_buf);
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 5: Overflow tree header (empty) --- */
    struct btree_header ovf_hdr;
    memset(&ovf_hdr, 0, sizeof(ovf_hdr));
    ovf_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    ovf_hdr.data_type = kBtreeDataTypeExtent;
    ovf_hdr.node_size = (uint16_t)block_size;
    ovf_hdr.tree_type = kBtreeTypeOverflow;
    obmafs3_checksum_block(&ovf_hdr, sizeof(ovf_hdr), ovf_hdr.checksum);

    rc = write_block(fd, block_size, 5, &ovf_hdr, sizeof(ovf_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 6: Dedup tree list header (empty) --- */
    struct tree_list_header dedup_list;
    memset(&dedup_list, 0, sizeof(dedup_list));
    dedup_list.magic      = OBMAFS3_TREELIST_MAGIC;
    dedup_list.tree_count = 0;
    obmafs3_checksum_block(&dedup_list, sizeof(dedup_list), dedup_list.checksum);

    rc = write_block(fd, block_size, 6, &dedup_list, sizeof(dedup_list));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 7: Media tag tree header (empty) --- */
    struct btree_header tag_hdr;
    memset(&tag_hdr, 0, sizeof(tag_hdr));
    tag_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    tag_hdr.data_type = kBtreeDataTypeMediaTagEntry;
    tag_hdr.node_size = (uint16_t)block_size;
    tag_hdr.tree_type = kBtreeTypeMediaTag;
    obmafs3_checksum_block(&tag_hdr, sizeof(tag_hdr), tag_hdr.checksum);

    rc = write_block(fd, block_size, 7, &tag_hdr, sizeof(tag_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 8: CD prefix tree header (empty) --- */
    struct btree_header cdpfx_hdr;
    memset(&cdpfx_hdr, 0, sizeof(cdpfx_hdr));
    cdpfx_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    cdpfx_hdr.data_type = kBtreeDataTypeCdPrefixEntry;
    cdpfx_hdr.node_size = (uint16_t)block_size;
    cdpfx_hdr.tree_type = kBtreeTypeCdPrefix;
    obmafs3_checksum_block(&cdpfx_hdr, sizeof(cdpfx_hdr), cdpfx_hdr.checksum);

    rc = write_block(fd, block_size, 8, &cdpfx_hdr, sizeof(cdpfx_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 9: CD suffix tree header (empty) --- */
    struct btree_header cdsfx_hdr;
    memset(&cdsfx_hdr, 0, sizeof(cdsfx_hdr));
    cdsfx_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    cdsfx_hdr.data_type = kBtreeDataTypeCdSuffixEntry;
    cdsfx_hdr.node_size = (uint16_t)block_size;
    cdsfx_hdr.tree_type = kBtreeTypeCdSuffix;
    obmafs3_checksum_block(&cdsfx_hdr, sizeof(cdsfx_hdr), cdsfx_hdr.checksum);

    rc = write_block(fd, block_size, 9, &cdsfx_hdr, sizeof(cdsfx_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 10: CD subchannel tree header (empty) --- */
    struct btree_header cdsub_hdr;
    memset(&cdsub_hdr, 0, sizeof(cdsub_hdr));
    cdsub_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    cdsub_hdr.data_type = kBtreeDataTypeCdSubchannelEntry;
    cdsub_hdr.node_size = (uint16_t)block_size;
    cdsub_hdr.tree_type = kBtreeTypeCdSubchannel;
    obmafs3_checksum_block(&cdsub_hdr, sizeof(cdsub_hdr), cdsub_hdr.checksum);

    rc = write_block(fd, block_size, 10, &cdsub_hdr, sizeof(cdsub_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 11: Metadata tree header (empty, multi-block nodes) --- */
    struct btree_header meta_hdr;
    memset(&meta_hdr, 0, sizeof(meta_hdr));
    meta_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    meta_hdr.data_type = kBtreeDataTypeMetadataEntry;
    meta_hdr.node_size = (uint16_t)(METADATA_NODE_BLOCKS * block_size);
    meta_hdr.tree_type = kBtreeTypeMetadata;
    obmafs3_checksum_block(&meta_hdr, sizeof(meta_hdr), meta_hdr.checksum);

    rc = write_block(fd, block_size, 11, &meta_hdr, sizeof(meta_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 12: Metadata index tree header (empty, multi-block nodes) --- */
    struct btree_header midx_hdr;
    memset(&midx_hdr, 0, sizeof(midx_hdr));
    midx_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    midx_hdr.data_type = kBtreeDataTypeMetadataIndexEntry;
    midx_hdr.node_size = (uint16_t)(METADATA_NODE_BLOCKS * block_size);
    midx_hdr.tree_type = kBtreeTypeMetadataIndex;
    obmafs3_checksum_block(&midx_hdr, sizeof(midx_hdr), midx_hdr.checksum);

    rc = write_block(fd, block_size, 12, &midx_hdr, sizeof(midx_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Block 13: Refcount tree header (empty) --- */
    struct btree_header ref_hdr;
    memset(&ref_hdr, 0, sizeof(ref_hdr));
    ref_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    ref_hdr.data_type = kBtreeDataTypeRefcountEntry;
    ref_hdr.node_size = (uint16_t)block_size;
    ref_hdr.tree_type = kBtreeTypeRefcount;
    obmafs3_checksum_block(&ref_hdr, sizeof(ref_hdr), ref_hdr.checksum);

    rc = write_block(fd, block_size, 13, &ref_hdr, sizeof(ref_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Sector Tag Data tree header (empty) --- */
    struct btree_header std_hdr;
    memset(&std_hdr, 0, sizeof(std_hdr));
    std_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    std_hdr.data_type = kBtreeDataTypeSectorTagDataEntry;
    std_hdr.node_size = (uint16_t)block_size;
    std_hdr.tree_type = kBtreeTypeSectorTagData;
    obmafs3_checksum_block(&std_hdr, sizeof(std_hdr), std_hdr.checksum);

    rc = write_block(fd, block_size, 14, &std_hdr, sizeof(std_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Sector Tag Ref tree header (empty) --- */
    struct btree_header str_hdr;
    memset(&str_hdr, 0, sizeof(str_hdr));
    str_hdr.magic     = OBMAFS3_BTREE_HDR_MAGIC;
    str_hdr.data_type = kBtreeDataTypeSectorTagRefEntry;
    str_hdr.node_size = (uint16_t)block_size;
    str_hdr.tree_type = kBtreeTypeSectorTagRef;
    obmafs3_checksum_block(&str_hdr, sizeof(str_hdr), str_hdr.checksum);

    rc = write_block(fd, block_size, 15, &str_hdr, sizeof(str_hdr));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* --- Blocks after sector tag headers: Allocation bitmap --- */
    {
        /* Build the flat bitmap data */
        uint8_t *bitmap = calloc(1, (size_t)bitmap_bytes);
        if(!bitmap)
        {
            close(fd);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }

        /* Mark blocks 0 through (16 + bitmap_blks - 1) as allocated.
         * This includes: superblock(1) + tree headers(15) + bitmap. */
        uint64_t reserved = 16 + bitmap_blks;
        for(uint64_t b = 0; b < reserved; b++) bitmap[b / 8] |= (1u << (b % 8));

        /* Mark the last block (backup superblock) as allocated */
        uint64_t backup_lba = total_blocks - 1;
        if(backup_lba >= reserved) bitmap[backup_lba / 8] |= (1u << (backup_lba % 8));

        /* Build the bitmap header with checksum over bitmap data */
        struct bitmap_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        bhdr.magic         = OBMAFS3_BITMAP_MAGIC;
        bhdr.total_blocks  = total_blocks;
        bhdr.next_free_lba = reserved; /* first block after reserved area */
        obmafs3_checksum_block(bitmap, (size_t)bitmap_bytes, bhdr.checksum);

        /* Write bitmap blocks: first block = header + data */
        uint8_t *blk = calloc(1, (size_t)block_size);
        if(!blk)
        {
            free(bitmap);
            close(fd);
            DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
        }

        uint64_t data_offset    = 0;
        uint64_t data_remaining = bitmap_bytes;

        for(uint64_t i = 0; i < bitmap_blks; i++)
        {
            memset(blk, 0, (size_t)block_size);

            if(i == 0)
            {
                memcpy(blk, &bhdr, hdr_size);
                size_t avail = (size_t)(block_size - hdr_size);
                size_t copy  = (data_remaining < avail) ? (size_t)data_remaining : avail;
                memcpy(blk + hdr_size, bitmap + data_offset, copy);
                data_offset += copy;
                data_remaining -= copy;
            }
            else
            {
                size_t copy = (data_remaining < block_size) ? (size_t)data_remaining : (size_t)block_size;
                memcpy(blk, bitmap + data_offset, copy);
                data_offset += copy;
                data_remaining -= copy;
            }

            rc = write_block(fd, block_size, sb.bitmap_lba + i, blk, (size_t)block_size);
            if(rc != OBMAFS3_OK)
            {
                free(blk);
                free(bitmap);
                close(fd);
                return rc;
            }
        }
        free(blk);
        free(bitmap);
    }

    /* --- Last block: Backup superblock --- */
    rc = write_block(fd, block_size, total_blocks - 1, &sb, sizeof(sb));
    if(rc != OBMAFS3_OK)
    {
        close(fd);
        return rc;
    }

    /* Flush and close */
    if(fsync(fd) < 0)
    {
        close(fd);
        DBG_RETURN(OBMAFS3_ERR_IO, "I/O error");
    }

    close(fd);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Filesystem checking (obmafsck)                                     */
/* ------------------------------------------------------------------ */

/**
 * Perform a basic filesystem check.
 *
 * Opens the filesystem, reads and prints the superblock, bitmap
 * statistics, and B+Tree header information.  Does not repair any
 * inconsistencies.
 *
 * @param path  Path to the filesystem image or device.
 * @return @c OBMAFS3_OK if the check passes, or an error code.
 */
int obmafs3_check(const char *path)
{
    struct obmafs3_ctx *ctx;
    int                 rc = obmafs3_open(path, &ctx);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "Error: failed to open filesystem: %d\n", rc);
        return rc;
    }

    printf("Superblock:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->sb.magic,
           ctx->sb.magic == OBMAFS3_SB_MAGIC ? "OK" : "BAD");
    printf("  Block size:       %" PRIu64 "\n", ctx->sb.block_size);
    printf("  Dedup block size: %" PRIu64 "\n", ctx->sb.dedup_block_size);
    printf("  Total bytes:      %" PRIu64 "\n", ctx->sb.total_bytes);
    printf("  Volume label:     %s\n", ctx->sb.volume_label);

    /* Bitmap statistics */
    if(ctx->sb.bitmap_lba != 0 && ctx->bitmap)
    {
        /* Read the header for display */
        uint8_t             *bhdr_buf = malloc((size_t)ctx->sb.block_size);
        struct bitmap_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        if(bhdr_buf)
        {
            if(obmafs3_block_read(ctx, ctx->sb.bitmap_lba, bhdr_buf, (size_t)ctx->sb.block_size) == OBMAFS3_OK)
                memcpy(&bhdr, bhdr_buf, sizeof(bhdr));
            free(bhdr_buf);
        }

        uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;
        uint64_t allocated    = 0;
        for(uint64_t b = 0; b < total_blocks; b++)
        {
            if(obmafs3_bitmap_is_set(ctx, b)) allocated++;
        }

        printf("\nAllocation bitmap:\n");
        printf("  Magic:            0x%016" PRIx64 " (%s)\n", bhdr.magic,
               bhdr.magic == OBMAFS3_BITMAP_MAGIC ? "OK" : "BAD");
        printf("  Checksum:         %s\n", ctx->bitmap ? "OK" : "BAD"); /* bitmap_read would have failed */
        printf("  Bitmap LBA:       %" PRIu64 "\n", ctx->sb.bitmap_lba);
        printf("  Bitmap blocks:    %" PRIu64 "\n", ctx->sb.bitmap_blocks);
        printf("  Total blocks:     %" PRIu64 "\n", total_blocks);
        printf("  Allocated blocks: %" PRIu64 "\n", allocated);
        printf("  Free blocks:      %" PRIu64 "\n", total_blocks - allocated);
    }

    printf("\nCatalog tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->catalog_hdr.magic,
           ctx->catalog_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
    printf("  Checksum:         OK\n"); /* btree_header_read already validated */
    printf("  Root node LBA:    %" PRIu64 "\n", ctx->catalog_hdr.root_node_lba);
    printf("  Total nodes:      %u\n", ctx->catalog_hdr.total_nodes);

    printf("\nInode tree:\n");
    printf("  Magic:            0x%016" PRIx64 " (%s)\n", ctx->inode_hdr.magic,
           ctx->inode_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
    printf("  Checksum:         OK\n"); /* btree_header_read already validated */
    printf("  Root node LBA:    %" PRIu64 "\n", ctx->inode_hdr.root_node_lba);
    printf("  Total nodes:      %u\n", ctx->inode_hdr.total_nodes);

    obmafs3_close(ctx);
    printf("\nFilesystem check passed.\n");
    return OBMAFS3_OK;
}
