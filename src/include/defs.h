#ifndef OBMAFS3_DEFS_H
#define OBMAFS3_DEFS_H

#include <stdint.h>

/* "TREELIST" as little-endian uint64 */
#define OBMAFS3_TREELIST_MAGIC 0x5453494C45455254ULL

#define OBMAFS3_DEFAULT_BLOCK_SIZE       4096
#define OBMAFS3_DEFAULT_DEDUP_BLOCK_SIZE 4096

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

struct __attribute__((packed)) extent_run {
    uint64_t start_block;  /**< Starting block of the extent run */
    uint64_t block_count;  /**< Number of blocks in the extent run */
};

#endif /* OBMAFS3_DEFS_H */