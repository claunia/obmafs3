#ifndef OBMAFS3_DEFS_H
#define OBMAFS3_DEFS_H

#include <stdint.h>

/* "TREELIST" as little-endian uint64 */
#define OBMAFS3_TREELIST_MAGIC 0x5453494C45455254ULL

/* "OBMABMAP" as little-endian uint64 */
#define OBMAFS3_BITMAP_MAGIC 0x50414D42414D424FULL

#define OBMAFS3_DEFAULT_BLOCK_SIZE       4096
#define OBMAFS3_DEFAULT_DEDUP_BLOCK_SIZE 4194304

#define OBMAFS3_ROOT_INODE_ID 2

struct __attribute__((packed)) dedup_entry {
    uint64_t hash;         /**< Hash of the deduplicated data block */
    uint64_t block_lba;    /**< LBA where the deduplicated data block is stored */
    uint64_t block_offset; /**< Offset within the block where dedup data starts */
};

struct __attribute__((packed)) tree_list_header {
    uint64_t magic;        /**< "TREELIST" */
    uint64_t tree_count;   /**< Total number of btrees in the list */
    uint8_t  checksum[32]; /**< Checksum of the tree list header block */
    /* Followed by an array of tree_list_entry structures */
};

struct __attribute__((packed)) tree_list_entry {
    uint16_t sector_size;  /**< Size of each sector in bytes */
    uint64_t tree_lba;     /**< LBA where the btree is stored */
};

struct __attribute__((packed)) sector_map_entry {
    int64_t  sector;       /**< Logical sector number within the disk image */
    uint16_t sector_size;  /**< Size of the sector in bytes */
    uint64_t hash;         /**< Hash of the sector data for deduplication */
};

struct __attribute__((packed)) cd_sector_map_entry {
    int64_t sector;       /**< Logical sector number within the CD image */
    uint16_t sector_size;  /**< Size of the CD sector in bytes (e.g. 2048, 2336, 2352) */
    uint64_t hash;         /**< Hash of the CD sector data for deduplication */
    uint8_t generated_prefix; /**< Indicates if prefix can be generated and is therefore not stored */
    uint64_t prefix_hash;  /**< Hash of the CD sector prefix data (e.g. 16 bytes before main data) */
    uint8_t generated_suffix; /**< Indicates if suffix can be generated and is therefore not stored */
    uint64_t suffix_hash;  /**< Hash of the CD sector suffix data (e.g. 288 bytes after main data) */
    uint64_t subchannel_hash; /**< Hash of the CD sector subchannel data (e.g. 96 bytes), 0 if not stored*/
    uint8_t subheader[8]; /**< Subheader data for CD-ROM XA sectors, 0 if not applicable */
    uint8_t sector_mode /* Audio, Mode 1, Mode 2 Form 1, Mode 2 Form 2, etc. for CD-ROM XA sectors */;
};

struct __attribute__((packed)) extent_run {
    uint64_t start_block;  /**< Starting block of the extent run */
    uint64_t block_count;  /**< Number of blocks in the extent run */
};

/**
 * An overflow extent entry stored in the overflow tree.
 * Associates an inode_id with an extent_run for files that
 * need more than 8 inline extents.
 */
struct __attribute__((packed)) overflow_extent {
    uint64_t inode_id;     /**< Inode this extent belongs to */
    uint64_t start_block;  /**< Starting block of the extent run */
    uint64_t block_count;  /**< Number of blocks in the extent run */
};

struct __attribute__((packed)) bitmap_header {
    uint64_t magic;          /**< "OBMABMAP" */
    uint64_t total_blocks;   /**< Total number of blocks tracked by bitmap */
    uint8_t  checksum[32];   /**< Checksum of the bitmap data */
};

#endif /* OBMAFS3_DEFS_H */