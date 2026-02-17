#ifndef OBMAFS3_OBMAFS_H
#define OBMAFS3_OBMAFS_H

#include <stdint.h>
#include <stddef.h>

#include "superblock.h"
#include "btree.h"
#include "block.h"
#include "defs.h"
#include "enums.h"

/* Error codes */
#define OBMAFS3_OK            0
#define OBMAFS3_ERR_IO       -1
#define OBMAFS3_ERR_NOMEM    -2
#define OBMAFS3_ERR_BADMAGIC -3
#define OBMAFS3_ERR_CHECKSUM -4
#define OBMAFS3_ERR_NOTFOUND -5
#define OBMAFS3_ERR_INVAL    -6
#define OBMAFS3_ERR_EXISTS   -7
#define OBMAFS3_ERR_NOSPC    -8

/** Filesystem context */
struct obmafs3_ctx {
    int fd;                          /**< File descriptor for the backing file */
    struct obmafs3_sb sb;            /**< Cached superblock */
    struct btree_header catalog_hdr; /**< Cached catalog tree header */
    struct btree_header inode_hdr;   /**< Cached inode tree header */
    struct btree_header overflow_hdr;/**< Cached overflow tree header */
    uint8_t *bitmap;                 /**< In-memory allocation bitmap */
    uint64_t bitmap_size;            /**< Size of allocation bitmap in bytes */
    int compression;                 /**< Non-zero to compress data blocks on write */
    int zstd_level;                  /**< ZSTD compression level (1-15) */
};

/* Open flags */
#define OBMAFS3_OPEN_SKIP_BITMAP  0x01  /**< Do not load/validate bitmap */
#define OBMAFS3_OPEN_LENIENT      0x02  /**< Tolerate checksum errors (for fsck) */

/* --- Context management --- */
int  obmafs3_open(const char *path, struct obmafs3_ctx **ctx);
int  obmafs3_open_flags(const char *path, int flags, struct obmafs3_ctx **ctx);
void obmafs3_close(struct obmafs3_ctx *ctx);

/* --- Superblock operations --- */
int obmafs3_sb_read(int fd, struct obmafs3_sb *sb);
int obmafs3_sb_write(int fd, const struct obmafs3_sb *sb);
int obmafs3_sb_validate(const struct obmafs3_sb *sb);

/* --- Block I/O --- */
int obmafs3_block_read(struct obmafs3_ctx *ctx, uint64_t lba,
                       void *buf, size_t size);
int obmafs3_block_write(struct obmafs3_ctx *ctx, uint64_t lba,
                        const void *buf, size_t size);

/* --- B+Tree operations --- */
int obmafs3_btree_header_read(struct obmafs3_ctx *ctx, uint64_t lba,
                              struct btree_header *hdr);
int obmafs3_btree_header_read_lenient(struct obmafs3_ctx *ctx, uint64_t lba,
                                      struct btree_header *hdr,
                                      int *checksum_ok);
int obmafs3_btree_header_write(struct obmafs3_ctx *ctx, uint64_t lba,
                               const struct btree_header *hdr);

/* --- Catalog operations --- */
int  obmafs3_catalog_lookup(struct obmafs3_ctx *ctx, uint64_t parent_id,
                            const char *name,
                            struct btree_node_filename *entry);
int  obmafs3_catalog_list(struct obmafs3_ctx *ctx, uint64_t parent_id,
                          struct btree_node_filename **entries,
                          uint32_t *count);
void obmafs3_catalog_list_free(struct btree_node_filename *entries);

/* --- Inode operations --- */
int obmafs3_inode_get(struct obmafs3_ctx *ctx, uint64_t inode_id,
                      struct btree_node_inode *inode);
int obmafs3_inode_put(struct obmafs3_ctx *ctx,
                      const struct btree_node_inode *inode);
int obmafs3_inode_delete(struct obmafs3_ctx *ctx, uint64_t inode_id);

/* --- Allocation bitmap --- */
int  obmafs3_bitmap_read(struct obmafs3_ctx *ctx);
int  obmafs3_bitmap_write(struct obmafs3_ctx *ctx);
void obmafs3_bitmap_set(struct obmafs3_ctx *ctx, uint64_t lba,
                        uint64_t count);
void obmafs3_bitmap_clear(struct obmafs3_ctx *ctx, uint64_t lba,
                          uint64_t count);
int  obmafs3_bitmap_is_set(struct obmafs3_ctx *ctx, uint64_t lba);
int  obmafs3_bitmap_find_free(struct obmafs3_ctx *ctx, uint64_t count,
                              uint64_t *start_lba);

/* --- Block allocation --- */
int      obmafs3_alloc_block(struct obmafs3_ctx *ctx, uint64_t *lba);
int      obmafs3_alloc_blocks(struct obmafs3_ctx *ctx, uint64_t count,
                              uint64_t *start_lba);
int      obmafs3_free_block(struct obmafs3_ctx *ctx, uint64_t lba);
int      obmafs3_free_blocks(struct obmafs3_ctx *ctx, uint64_t lba,
                             uint64_t count);
uint64_t obmafs3_alloc_inode_id(struct obmafs3_ctx *ctx);

/* --- Catalog mutations --- */
int obmafs3_catalog_insert(struct obmafs3_ctx *ctx,
                           const struct btree_node_filename *entry);
int obmafs3_catalog_delete(struct obmafs3_ctx *ctx, uint64_t parent_id,
                           const char *name);

/* --- File data operations --- */
int obmafs3_read_file_data(struct obmafs3_ctx *ctx,
                           const struct btree_node_inode *inode,
                           uint64_t offset, void *buf, size_t size);
int obmafs3_write_file_data(struct obmafs3_ctx *ctx,
                            struct btree_node_inode *inode,
                            uint64_t offset, const void *buf, size_t size);

/* --- Deduplication operations --- */
int obmafs3_dedup_get_tree(struct obmafs3_ctx *ctx, uint16_t sector_size,
                           struct btree_header *hdr, uint64_t *hdr_lba);
int obmafs3_dedup_lookup(struct obmafs3_ctx *ctx,
                         const struct btree_header *hdr,
                         uint64_t hash, struct dedup_entry *entry);

/**
 * In-memory cache of sector_map_entries.  When non-NULL is passed to
 * obmafs3_write_media_image_data, entries are accumulated here instead
 * of being written to disk on every FUSE write call.  Call
 * obmafs3_flush_sector_map_cache to write them out (e.g. on close).
 */
struct sector_map_cache {
    struct sector_map_entry *entries;
    uint64_t count;
    uint64_t capacity;
};

/**
 * Persistent cache of the dedup data block accumulator.  When non-NULL
 * is passed to obmafs3_write_media_image_data, the 4 MiB in-memory
 * dedup block is kept alive across calls — avoiding a costly disk
 * read + decompression on every FUSE write.  The caller must call
 * obmafs3_flush_dedup_block_cache before close/release, then
 * obmafs3_free_dedup_block_cache to free the memory.
 */
struct dedup_block_cache {
    uint8_t  *data;          /**< In-memory dedup data block buffer */
    uint64_t  block_lba;     /**< LBA of this dedup block */
    uint64_t  offset;        /**< Next write offset within the block */
    uint64_t  capacity;      /**< Total capacity (dedup_block_size) */
    uint64_t  std_blocks;    /**< Number of standard blocks per dedup block */
    int       dirty;         /**< Whether the buffer has been modified */
    int       initialized;   /**< Non-zero once first init has run */
};

int obmafs3_write_media_image_data(struct obmafs3_ctx *ctx,
                                   struct btree_node_inode *inode,
                                   uint64_t offset, const void *buf,
                                   size_t size, uint16_t sector_size,
                                   struct sector_map_cache *cache,
                                   struct dedup_block_cache *db_cache);
int obmafs3_flush_dedup_block_cache(struct obmafs3_ctx *ctx,
                                    uint16_t sector_size,
                                    struct dedup_block_cache *db_cache);
void obmafs3_free_dedup_block_cache(struct dedup_block_cache *db_cache);
int obmafs3_flush_sector_map_cache(struct obmafs3_ctx *ctx,
                                   struct btree_node_inode *inode,
                                   struct sector_map_cache *cache);
void obmafs3_free_sector_map_cache(struct sector_map_cache *cache);

int obmafs3_read_media_image_data(struct obmafs3_ctx *ctx,
                                  const struct btree_node_inode *inode,
                                  uint64_t offset, void *buf,
                                  size_t size, uint16_t sector_size);

/* --- Checksum operations --- */
uint64_t obmafs3_checksum_xxh64(const void *data, size_t size);
void     obmafs3_checksum_block(const void *data, size_t size, uint8_t *out);

/* --- Compression operations --- */
int obmafs3_compress(const void *src, size_t src_size,
                     void *dst, size_t *dst_size, int level);
int obmafs3_decompress(const void *src, size_t src_size,
                       void *dst, size_t dst_size);

/* --- Filesystem creation --- */
int obmafs3_create(const char *path, uint64_t total_size,
                   uint64_t block_size, uint64_t dedup_block_size,
                   const char *label, const uint8_t *guid);

/* --- Filesystem checking --- */
int obmafs3_check(const char *path);

#endif /* OBMAFS3_OBMAFS_H */
