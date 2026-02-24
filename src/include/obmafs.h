// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : obmafs.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Common definitions and structures for OBMAFS3.
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

#ifndef OBMAFS3_OBMAFS_H
#define OBMAFS3_OBMAFS_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "btree.h"
#include "defs.h"
#include "enums.h"
#include "superblock.h"
#include "tags.h"

/* Forward-declare ZSTD context types so we can store them as pointers
 * without pulling <zstd.h> into the public header. */
struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

/* Forward-declare the compression thread-pool (defined in block.c). */
struct compress_pool;

/**
 * Per-thread scratch buffers and ZSTD contexts.
 *
 * Each FUSE worker thread gets its own set of scratch buffers so that
 * concurrent B+Tree traversals, reads, and compressions do not stomp
 * on each other's data.  Allocated lazily on first use via
 * obmafs3_get_thread_bufs() and freed automatically when the thread
 * exits (via pthread_key destructor).
 */
struct obmafs3_thread_bufs
{
    uint8_t            *hdr_buf;          ///< B+Tree header I/O (block_size bytes)
    uint8_t            *node_buf;         ///< B+Tree node traversal (block_size bytes)
    uint8_t            *io_buf;           ///< Data block I/O (group_bytes)
    uint8_t            *io_buf2;          ///< Decompression work buffer (group_bytes)
    uint8_t            *comp_buf;         ///< Compression output buffer
    size_t              comp_buf_size;    ///< Size of comp_buf in bytes
    struct ZSTD_CCtx_s *zstd_cctx;        ///< ZSTD compression context
    struct ZSTD_CCtx_s *zstd_probe_cctx;  ///< ZSTD probe context (fast level-1 compressibility test)
    struct ZSTD_DCtx_s *zstd_dctx;        ///< ZSTD decompression context
};

/* Error codes */
#define OBMAFS3_OK           0
#define OBMAFS3_ERR_IO       -1
#define OBMAFS3_ERR_NOMEM    -2
#define OBMAFS3_ERR_BADMAGIC -3
#define OBMAFS3_ERR_CHECKSUM -4
#define OBMAFS3_ERR_NOTFOUND -5
#define OBMAFS3_ERR_INVAL    -6
#define OBMAFS3_ERR_EXISTS   -7
#define OBMAFS3_ERR_NOSPC    -8

/** Filesystem context */
struct obmafs3_ctx
{
    int                   fd;                      ///< File descriptor for the backing file
    struct obmafs3_sb     sb;                      ///< Cached superblock
    struct btree_header   catalog_hdr;             ///< Cached catalog tree header
    struct btree_header   inode_hdr;               ///< Cached inode tree header
    struct btree_header   overflow_hdr;            ///< Cached overflow tree header
    struct btree_header   media_tag_hdr;           ///< Cached media tag tree header
    struct btree_header   cd_prefix_hdr;           ///< Cached CD prefix tree header
    struct btree_header   cd_suffix_hdr;           ///< Cached CD suffix tree header
    struct btree_header   cd_subchannel_hdr;       ///< Cached CD subchannel tree header
    struct btree_header   metadata_hdr;            ///< Cached metadata tree header
    struct btree_header   metadata_idx_hdr;        ///< Cached metadata index tree header
    struct btree_header   refcount_hdr;            ///< Cached refcount tree header
    uint8_t              *bitmap;                  ///< In-memory allocation bitmap
    uint64_t              bitmap_size;             ///< Size of allocation bitmap in bytes
    uint64_t              next_free_lba;           ///< Allocation hint (persisted in bitmap header)
    int                   compression;             ///< Non-zero to compress data blocks on write
    int                   zstd_level;              ///< ZSTD compression level (1-15)
    pthread_key_t         tls_key;                 ///< Thread-local scratch buffers (obmafs3_thread_bufs)
    pthread_rwlock_t      tree_lock;               ///< Serialises writers; readers take shared lock
    uint8_t              *rc_leaf_buf;             ///< Cached refcount B+Tree leaf node
    uint64_t              rc_leaf_lba;             ///< LBA of the cached refcount leaf
    uint64_t              rc_leaf_min;             ///< Smallest key in the cached leaf
    uint64_t              rc_leaf_max;             ///< Largest key in the cached leaf
    uint16_t              rc_leaf_count;           ///< Number of keys in the cached leaf
    int                   rc_leaf_valid;           ///< Non-zero when the leaf cache is populated
    struct compress_pool *compress_pool;           ///< Persistent compression thread pool
    pid_t                 pre_fuse_pid;            ///< PID before fuse_main (detect fork in init)
    void                 *dedup_node_cache;        ///< Global dedup B+Tree node cache (shared across files)
    void                 *dedup_lookup_cache;      ///< Global hash→dedup_entry cache (shared across files)
    void                 *dedup_key_set;           ///< Global dedup hash key set (fast existence check)
    void                 *dedup_pending;           ///< Pending insert buffer (deferred B+Tree inserts)
    void                 *dedup_pending_draining;  ///< Pending buffer being drained by housekeeping thread
    pthread_t             housekeeping_thread;     ///< Background housekeeping thread (drains pending → B+Tree)
    pthread_mutex_t       housekeeping_mutex;      ///< Protects housekeeping condvar
    pthread_cond_t        housekeeping_cond;       ///< Signalled to wake or stop housekeeping
    int                   housekeeping_started;    ///< 1 if housekeeping thread was created
    pthread_t             warmup_thread;           ///< Background keyset warmup thread
    pthread_mutex_t       warmup_mutex;            ///< Protects warmup_done flag
    pthread_cond_t        warmup_cond;             ///< Signalled when warmup completes
    int                   warmup_running;          ///< 1 while background warmup is active
    int                   warmup_done;             ///< 1 after warmup has completed
    int                   warmup_started;          ///< 1 if warmup thread was created (needs join)
    volatile int          shutdown_requested;      ///< 1 when close() wants warmup to abort early
};

/* Open flags */
#define OBMAFS3_OPEN_SKIP_BITMAP 0x01  ///< Do not load/validate bitmap
#define OBMAFS3_OPEN_LENIENT     0x02  ///< Tolerate checksum errors (for fsck)

/* --- Context management --- */
int  obmafs3_open(const char *path, struct obmafs3_ctx **ctx);
int  obmafs3_open_flags(const char *path, int flags, struct obmafs3_ctx **ctx);
void obmafs3_close(struct obmafs3_ctx *ctx);

/* --- Thread-local scratch buffers --- */
struct obmafs3_thread_bufs *obmafs3_get_thread_bufs(struct obmafs3_ctx *ctx);

/* --- Compression thread pool --- */
int  obmafs3_compress_pool_init(struct obmafs3_ctx *ctx);
void obmafs3_compress_pool_reinit(struct obmafs3_ctx *ctx);
void obmafs3_compress_pool_destroy(struct obmafs3_ctx *ctx);

/* --- Async pool jobs (used by dedup background compression) --- */

/**
 * A single asynchronous work item that can be submitted to the pool.
 * Used by the dedup path to compress full dedup blocks in the
 * background while the main thread continues accumulating data.
 *
 * The function pointer uses @c void* for the compression context
 * to avoid a ZSTD dependency in the header; callers cast as needed.
 */
struct pool_async_job
{
    int (*fn)(void *arg, void *cctx);  ///< work function (cctx is ZSTD_CCtx*)
    void                  *arg;        ///< opaque context passed to fn
    int                    result;     ///< return value from fn
    _Atomic int            done;       ///< set to 1 when complete
    pthread_mutex_t        mtx;        ///< protects cond wait
    pthread_cond_t         cond;       ///< signalled on completion
    struct pool_async_job *next;       ///< queue link
};

struct pool_async_job *obmafs3_pool_async_job_create(int (*fn)(void *arg, void *cctx), void *arg);
void                   obmafs3_pool_async_job_free(struct pool_async_job *job);
void                   obmafs3_pool_submit_async(struct compress_pool *pool, struct pool_async_job *job);
int                    obmafs3_pool_wait_async(struct pool_async_job *job);

/* --- Superblock operations --- */
int obmafs3_sb_read(int fd, struct obmafs3_sb *sb);
int obmafs3_sb_read_lenient(int fd, struct obmafs3_sb *sb, int *checksum_ok);
int obmafs3_sb_write(int fd, const struct obmafs3_sb *sb);
int obmafs3_sb_validate(const struct obmafs3_sb *sb);
int obmafs3_sb_read_backup(int fd, uint64_t block_size, uint64_t total_bytes, struct obmafs3_sb *sb);
int obmafs3_sb_read_backup_lenient(int fd, uint64_t block_size, uint64_t total_bytes, struct obmafs3_sb *sb,
                                   int *checksum_ok);

/* --- Block I/O --- */
int obmafs3_block_read(struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t size);
int obmafs3_block_write(struct obmafs3_ctx *ctx, uint64_t lba, const void *buf, size_t size);

/* --- B+Tree operations --- */
int obmafs3_btree_header_read(struct obmafs3_ctx *ctx, uint64_t lba, struct btree_header *hdr);
int obmafs3_btree_header_read_lenient(struct obmafs3_ctx *ctx, uint64_t lba, struct btree_header *hdr,
                                      int *checksum_ok);
int obmafs3_btree_header_write(struct obmafs3_ctx *ctx, uint64_t lba, struct btree_header *hdr);

/* --- Catalog operations --- */
int obmafs3_catalog_lookup(struct obmafs3_ctx *ctx, uint64_t parent_id, const char *name, struct catalog_record *entry);
int obmafs3_catalog_list(struct obmafs3_ctx *ctx, uint64_t parent_id, struct catalog_record **entries, uint32_t *count);
void obmafs3_catalog_list_free(struct catalog_record *entries);

/* --- Inode operations --- */
int obmafs3_inode_get(struct obmafs3_ctx *ctx, uint64_t inode_id, struct inode_record *inode);
int obmafs3_inode_put(struct obmafs3_ctx *ctx, const struct inode_record *inode);
int obmafs3_inode_delete(struct obmafs3_ctx *ctx, uint64_t inode_id);

/* --- Allocation bitmap --- */
int  obmafs3_bitmap_read(struct obmafs3_ctx *ctx);
int  obmafs3_bitmap_write(struct obmafs3_ctx *ctx);
void obmafs3_bitmap_set(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t count);
void obmafs3_bitmap_clear(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t count);
int  obmafs3_bitmap_is_set(struct obmafs3_ctx *ctx, uint64_t lba);
int  obmafs3_bitmap_find_free(struct obmafs3_ctx *ctx, uint64_t count, uint64_t *start_lba);

/* --- Block allocation --- */
int      obmafs3_alloc_block(struct obmafs3_ctx *ctx, uint64_t *lba);
int      obmafs3_alloc_blocks(struct obmafs3_ctx *ctx, uint64_t count, uint64_t *start_lba);
int      obmafs3_free_block(struct obmafs3_ctx *ctx, uint64_t lba);
int      obmafs3_free_blocks(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t count);
uint64_t obmafs3_alloc_inode_id(struct obmafs3_ctx *ctx);

/* --- B+Tree node allocation (clump-aware) --- */
int obmafs3_btree_alloc_node(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, uint64_t *node_lba);
int obmafs3_btree_free_node(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba, uint64_t node_lba);

/* --- Catalog mutations --- */
int obmafs3_catalog_insert(struct obmafs3_ctx *ctx, const struct catalog_record *entry);
int obmafs3_catalog_delete(struct obmafs3_ctx *ctx, uint64_t parent_id, const char *name);

/* --- File data operations --- */
int obmafs3_read_file_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                           size_t size);
int obmafs3_write_file_data(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t offset, const void *buf,
                            size_t size);

/* --- Clone / reflink operations --- */
int obmafs3_clone_file_range(struct obmafs3_ctx *ctx, const struct inode_record *src_inode, uint64_t src_offset,
                             struct inode_record *dst_inode, uint64_t dst_offset, uint64_t length);
int obmafs3_free_file_blocks(struct obmafs3_ctx *ctx, struct inode_record *inode);
int obmafs3_truncate_file_blocks(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t new_block_count);

/* --- Deduplication operations --- */
int obmafs3_dedup_get_tree(struct obmafs3_ctx *ctx, uint16_t sector_size, struct btree_header *hdr, uint64_t *hdr_lba);
int obmafs3_dedup_lookup(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash,
                         struct dedup_entry *entry);

/**
 * In-memory cache of sector_map_entries.  When non-NULL is passed to
 * obmafs3_write_media_image_data, entries are accumulated here instead
 * of being written to disk on every FUSE write call.  Call
 * obmafs3_flush_sector_map_cache to write them out (e.g. on close).
 */
struct sector_map_cache
{
    struct sector_map_entry *entries;
    uint64_t                 count;
    uint64_t                 capacity;
};

/**
 * Persistent cache of the dedup data block accumulator.  When non-NULL
 * is passed to obmafs3_write_media_image_data, the 4 MiB in-memory
 * dedup block is kept alive across calls — avoiding a costly disk
 * read + decompression on every FUSE write.  The caller must call
 * obmafs3_flush_dedup_block_cache before close/release, then
 * obmafs3_free_dedup_block_cache to free the memory.
 */
struct dedup_block_cache
{
    uint8_t            *data;               ///< In-memory dedup data block buffer
    uint64_t            block_lba;          ///< LBA of this dedup block
    uint64_t            offset;             ///< Next write offset within the block
    uint64_t            capacity;           ///< Total capacity (dedup_block_size)
    uint64_t            std_blocks;         ///< Number of standard blocks per dedup block
    int                 dirty;              ///< Whether the buffer has been modified
    int                 initialized;        ///< Non-zero once first init has run
    void               *pending_job;        ///< Pending pool_async_job (NULL when idle)
    struct btree_header dedup_hdr;          ///< Cached dedup tree header
    uint64_t            dedup_hdr_lba;      ///< Cached dedup tree header LBA
    int                 hdr_cached;         ///< Non-zero when dedup_hdr is valid
    uint64_t            last_dedup_lba;     ///< block_lba of the most recently written/found sector
    uint64_t            last_dedup_offset;  ///< block_offset of the most recently written/found sector
};

int  obmafs3_write_media_image_data(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t offset,
                                    const void *buf, size_t size, uint16_t sector_size, struct sector_map_cache *cache,
                                    struct dedup_block_cache *db_cache);
int  obmafs3_flush_dedup_block_cache(struct obmafs3_ctx *ctx, uint16_t sector_size, struct dedup_block_cache *db_cache);
void obmafs3_free_dedup_block_cache(struct obmafs3_ctx *ctx, struct dedup_block_cache *db_cache);
void obmafs3_dedup_node_cache_init(struct obmafs3_ctx *ctx);
void obmafs3_dedup_node_cache_free(struct obmafs3_ctx *ctx);
void obmafs3_dedup_key_set_free(struct obmafs3_ctx *ctx);
void obmafs3_dedup_pending_flush_and_free(struct obmafs3_ctx *ctx);
int  obmafs3_dedup_pending_save(struct obmafs3_ctx *ctx);
int  obmafs3_dedup_pending_load(struct obmafs3_ctx *ctx);
int  obmafs3_dedup_keyset_save(struct obmafs3_ctx *ctx);
int  obmafs3_dedup_keyset_load(struct obmafs3_ctx *ctx);
void obmafs3_dedup_warmup_start(struct obmafs3_ctx *ctx);
void obmafs3_dedup_warmup_wait(struct obmafs3_ctx *ctx);
void obmafs3_housekeeping_start(struct obmafs3_ctx *ctx);
void obmafs3_housekeeping_stop(struct obmafs3_ctx *ctx);
int obmafs3_flush_sector_map_cache(struct obmafs3_ctx *ctx, struct inode_record *inode, struct sector_map_cache *cache);
void obmafs3_free_sector_map_cache(struct sector_map_cache *cache);

int obmafs3_read_media_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                                  size_t size, uint16_t sector_size, void *leaf_cache, void *dedup_cache);
void *obmafs3_alloc_media_leaf_cache(void);
void  obmafs3_free_media_leaf_cache(void *leaf_cache);
void *obmafs3_alloc_media_dedup_cache(struct obmafs3_ctx *ctx);
void  obmafs3_free_media_dedup_cache(void *dedup_cache);
int   obmafs3_read_cd_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                                 size_t size);
int   obmafs3_read_subchannel_data(struct obmafs3_ctx *ctx, const struct inode_record *sub_inode, uint64_t offset,
                                   void *buf, size_t size);

/* --- Media tag operations --- */
int  obmafs3_media_tag_get(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type, void **data,
                           uint32_t *data_length);
void obmafs3_media_tag_data_free(void *data);
int  obmafs3_media_tag_put(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type, const void *data,
                           uint32_t data_length);
int  obmafs3_media_tag_delete(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t tag_type);
int  obmafs3_media_tag_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id);
int  obmafs3_media_tag_list(struct obmafs3_ctx *ctx, uint64_t inode_id, uint16_t **tag_types, uint32_t *count);
void obmafs3_media_tag_list_free(uint16_t *tag_types);

/* --- CD prefix/suffix/subchannel B+Tree operations --- */
int obmafs3_cd_prefix_get(struct obmafs3_ctx *ctx, uint64_t hash, uint8_t data[CD_PREFIX_DATA_SIZE]);
int obmafs3_cd_prefix_put(struct obmafs3_ctx *ctx, uint64_t hash, const uint8_t data[CD_PREFIX_DATA_SIZE]);
int obmafs3_cd_prefix_delete(struct obmafs3_ctx *ctx, uint64_t hash);

int obmafs3_cd_suffix_get(struct obmafs3_ctx *ctx, uint64_t hash, uint8_t data[CD_SUFFIX_DATA_SIZE]);
int obmafs3_cd_suffix_put(struct obmafs3_ctx *ctx, uint64_t hash, const uint8_t data[CD_SUFFIX_DATA_SIZE]);
int obmafs3_cd_suffix_delete(struct obmafs3_ctx *ctx, uint64_t hash);

int obmafs3_cd_subchannel_get(struct obmafs3_ctx *ctx, uint64_t hash, uint8_t data[CD_SUBCHANNEL_DATA_SIZE]);
int obmafs3_cd_subchannel_get_location(struct obmafs3_ctx *ctx, uint64_t hash, uint8_t data[CD_SUBCHANNEL_DATA_SIZE],
                                       uint64_t *leaf_lba, uint64_t *record_offset);
int obmafs3_cd_subchannel_put(struct obmafs3_ctx *ctx, uint64_t hash, const uint8_t data[CD_SUBCHANNEL_DATA_SIZE]);
int obmafs3_cd_subchannel_delete(struct obmafs3_ctx *ctx, uint64_t hash);

/* --- Image metadata operations --- */
int  obmafs3_metadata_get(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key, char *value, size_t value_size);
int  obmafs3_metadata_put(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key, const char *value);
int  obmafs3_metadata_delete(struct obmafs3_ctx *ctx, uint64_t inode_id, const char *key);
int  obmafs3_metadata_delete_all(struct obmafs3_ctx *ctx, uint64_t inode_id);
int  obmafs3_metadata_list(struct obmafs3_ctx *ctx, uint64_t inode_id, char ***keys, uint32_t *count);
void obmafs3_metadata_list_free(char **keys, uint32_t count);
int obmafs3_metadata_query(struct obmafs3_ctx *ctx, const char *key, const char *value, char ***paths, uint32_t *count);
void obmafs3_metadata_query_free(char **paths, uint32_t count);

/** A single metadata query filter condition.
 *  When key is "*" (single asterisk) the filter matches any key
 *  whose value satisfies the operator (wildcard key query). */
struct obmafs3_query_filter
{
    char    key[METADATA_KEY_MAX];     /**< Metadata key to match ("*" = any key) */
    char    value[METADATA_VALUE_MAX]; /**< Value operand (ignored for kQueryOpExists) */
    uint8_t op;                        /**< enum obmafs3_query_op */
};

#define OBMAFS3_QUERY_MAX_FILTERS 4

int obmafs3_metadata_query_filtered(struct obmafs3_ctx *ctx, const struct obmafs3_query_filter *filters,
                                    uint8_t filter_count, uint8_t combine, char ***paths, uint32_t *count);
int obmafs3_resolve_inode_path(struct obmafs3_ctx *ctx, uint64_t inode_id, char *path_buf, size_t path_buf_size);

/* --- Block refcount operations --- */
int obmafs3_refcount_get(struct obmafs3_ctx *ctx, uint64_t lba, uint32_t *ref_count);
int obmafs3_refcount_set(struct obmafs3_ctx *ctx, uint64_t lba, uint32_t ref_count);
int obmafs3_refcount_inc(struct obmafs3_ctx *ctx, uint64_t lba);
int obmafs3_refcount_dec(struct obmafs3_ctx *ctx, uint64_t lba, uint32_t *new_count);

/* --- CD ECC/EDC operations --- */
void *ecc_cd_init(void);
void  ecc_cd_free(void *ctx);
bool  ecc_cd_is_suffix_correct(void *context, const uint8_t *sector);
bool  ecc_cd_is_suffix_correct_mode2(void *context, const uint8_t *sector);
void  ecc_cd_reconstruct_prefix(uint8_t *sector, uint8_t type, int64_t lba);
void  ecc_cd_reconstruct(void *context, uint8_t *sector, uint8_t type);
void  cd_lba_to_msf(int64_t pos, uint8_t *minute, uint8_t *second, uint8_t *frame);

/* --- CD sector map cache --- */

/**
 * In-memory cache of cd_sector_map_entries, analogous to sector_map_cache.
 */
struct cd_sector_map_cache
{
    struct cd_sector_map_entry *entries;
    uint64_t                    count;
    uint64_t                    capacity;
};

int  obmafs3_flush_cd_sector_map_cache(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                       struct cd_sector_map_cache *cache);
void obmafs3_free_cd_sector_map_cache(struct cd_sector_map_cache *cache);

/* --- Checksum operations --- */
uint64_t obmafs3_checksum_xxh64(const void *data, size_t size);
void     obmafs3_checksum_block(const void *data, size_t size, uint8_t *out);

/* --- Compression operations --- */
int obmafs3_compress(struct ZSTD_CCtx_s *cctx, const void *src, size_t src_size, void *dst, size_t *dst_size,
                     int level);
int obmafs3_decompress(struct ZSTD_DCtx_s *dctx, const void *src, size_t src_size, void *dst, size_t dst_size);

/* --- Filesystem creation --- */
int obmafs3_create(const char *path, uint64_t total_size, uint64_t block_size, uint64_t dedup_block_size,
                   const char *label, const uint8_t *guid);

/* --- Filesystem checking --- */
int obmafs3_check(const char *path);

#endif /* OBMAFS3_OBMAFS_H */
