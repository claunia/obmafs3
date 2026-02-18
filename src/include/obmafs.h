#ifndef OBMAFS3_OBMAFS_H
#define OBMAFS3_OBMAFS_H

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
    int                 fd;                 ///< File descriptor for the backing file
    struct obmafs3_sb   sb;                 ///< Cached superblock
    struct btree_header catalog_hdr;        ///< Cached catalog tree header
    struct btree_header inode_hdr;          ///< Cached inode tree header
    struct btree_header overflow_hdr;       ///< Cached overflow tree header
    struct btree_header media_tag_hdr;      ///< Cached media tag tree header
    struct btree_header cd_prefix_hdr;      ///< Cached CD prefix tree header
    struct btree_header cd_suffix_hdr;      ///< Cached CD suffix tree header
    struct btree_header cd_subchannel_hdr;  ///< Cached CD subchannel tree header
    struct btree_header metadata_hdr;       ///< Cached metadata tree header
    struct btree_header metadata_idx_hdr;   ///< Cached metadata index tree header
    struct btree_header refcount_hdr;       ///< Cached refcount tree header
    uint8_t            *bitmap;             ///< In-memory allocation bitmap
    uint64_t            bitmap_size;        ///< Size of allocation bitmap in bytes
    uint64_t            next_free_lba;      ///< Allocation hint (persisted in bitmap header)
    int                 compression;        ///< Non-zero to compress data blocks on write
    int                 zstd_level;         ///< ZSTD compression level (1-15)
    uint8_t            *hdr_buf;            ///< Reusable buffer for B+Tree header I/O
    uint8_t            *node_buf;           ///< Reusable buffer for B+Tree node traversal
    uint8_t            *io_buf;             ///< Reusable buffer for data block I/O
    uint8_t            *io_buf2;            ///< Reusable second buffer for decompression / work
    uint8_t            *comp_buf;           ///< Reusable buffer for compression output
    size_t              comp_buf_size;      ///< Size of comp_buf in bytes
    struct ZSTD_CCtx_s *zstd_cctx;          ///< Reusable ZSTD compression context
    struct ZSTD_DCtx_s *zstd_dctx;          ///< Reusable ZSTD decompression context
};

/* Open flags */
#define OBMAFS3_OPEN_SKIP_BITMAP 0x01  ///< Do not load/validate bitmap
#define OBMAFS3_OPEN_LENIENT     0x02  ///< Tolerate checksum errors (for fsck)

/* --- Context management --- */
int  obmafs3_open(const char *path, struct obmafs3_ctx **ctx);
int  obmafs3_open_flags(const char *path, int flags, struct obmafs3_ctx **ctx);
void obmafs3_close(struct obmafs3_ctx *ctx);

/* --- Superblock operations --- */
int obmafs3_sb_read(int fd, struct obmafs3_sb *sb);
int obmafs3_sb_write(int fd, const struct obmafs3_sb *sb);
int obmafs3_sb_validate(const struct obmafs3_sb *sb);

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
    uint8_t *data;         ///< In-memory dedup data block buffer
    uint64_t block_lba;    ///< LBA of this dedup block
    uint64_t offset;       ///< Next write offset within the block
    uint64_t capacity;     ///< Total capacity (dedup_block_size)
    uint64_t std_blocks;   ///< Number of standard blocks per dedup block
    int      dirty;        ///< Whether the buffer has been modified
    int      initialized;  ///< Non-zero once first init has run
    void    *bg_compress;  ///< Opaque background compression context
    void    *node_cache;   ///< Opaque dedup B+Tree node cache
    struct btree_header dedup_hdr;     ///< Cached dedup tree header
    uint64_t            dedup_hdr_lba; ///< Cached dedup tree header LBA
    int                 hdr_cached;    ///< Non-zero when dedup_hdr is valid
};

int  obmafs3_write_media_image_data(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t offset,
                                    const void *buf, size_t size, uint16_t sector_size, struct sector_map_cache *cache,
                                    struct dedup_block_cache *db_cache);
int  obmafs3_flush_dedup_block_cache(struct obmafs3_ctx *ctx, uint16_t sector_size, struct dedup_block_cache *db_cache);
void obmafs3_free_dedup_block_cache(struct dedup_block_cache *db_cache);

/** Start a background compression worker thread for the dedup block cache. */
int  obmafs3_bg_compress_start(struct obmafs3_ctx *ctx, struct dedup_block_cache *db_cache);
/** Wait for any pending background compression and shut down the worker. */
void obmafs3_bg_compress_stop(struct dedup_block_cache *db_cache);
int obmafs3_flush_sector_map_cache(struct obmafs3_ctx *ctx, struct inode_record *inode, struct sector_map_cache *cache);
void obmafs3_free_sector_map_cache(struct sector_map_cache *cache);

int obmafs3_read_media_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                                  size_t size, uint16_t sector_size);

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
int  obmafs3_resolve_inode_path(struct obmafs3_ctx *ctx, uint64_t inode_id, char *path_buf, size_t path_buf_size);

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
