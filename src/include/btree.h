#ifndef OBMAFS3_BTREE_H
#define OBMAFS3_BTREE_H

#include <stdint.h>
#include "defs.h"

/* "BTREEHDR" as little-endian uint64 */
#define OBMAFS3_BTREE_HDR_MAGIC  0x5244484545525442ULL
/* "BTREENDE" as little-endian uint64 */
#define OBMAFS3_BTREE_NODE_MAGIC 0x45444E4545525442ULL

struct __attribute__((packed)) btree_header {
    uint64_t magic;          /**< "BTREEHDR" */
    uint32_t data_type;      /**< Type of data stored in the btree */
    uint64_t root_node_lba;  /**< LBA of the root node */
    uint64_t free_node_lba;  /**< LBA of the first free node */
    uint16_t node_size;      /**< Size of each node in bytes */
    uint32_t total_nodes;    /**< Total number of nodes */
    uint32_t free_nodes;     /**< Number of free nodes */
    uint32_t tree_type;      /**< Type of btree (catalog, dedup, metadata, etc.) */
    uint64_t last_block_lba; /**< LBA of the last partially written data block (dedup trees) */
    uint64_t last_block_offset; /**< Byte offset within last_block_lba where next write starts */
    uint8_t  checksum[32];   /**< Checksum of the btree header block */
};

struct __attribute__((packed)) btree_node_header {
    uint64_t magic;          /**< "BTREENDE" */
    uint8_t  record_type;    /**< Type of records in this node */
    uint8_t  level;          /**< 0 = leaf node, >0 = index node (B+Tree depth) */
    uint64_t left_link;      /**< LBA of left sibling node */
    uint64_t right_link;     /**< LBA of right sibling node */
    uint64_t overflow_link;  /**< LBA of overflow node */
    uint16_t node_keys;      /**< Number of keys in this node */
    uint16_t keys_length;    /**< Total length of keys in this node */
    uint8_t  checksum[32];   /**< Checksum of the btree node block */
};

struct __attribute__((packed)) btree_node_filename {
    struct btree_node_header header;
    uint64_t inode_id;       /**< Unique identifier for the file or directory */
    uint64_t parent_id;      /**< Identifier of the parent directory */
    uint8_t  directory_flag; /**< 1 if directory, 0 if file */
    char     name[256];      /**< Name in UTF-8 NUL-terminated format */
};

/**
 * Inode record stored in B+Tree leaf nodes.
 * This is the public API type for inode operations and the on-disk format.
 */
struct __attribute__((packed)) inode_record {
    uint64_t inode_id;           /**< Unique identifier for the file */
    uint32_t uid;                /**< User ID of the file owner */
    uint32_t gid;                /**< Group ID of the file owner */
    uint32_t mode;               /**< File permissions */
    uint64_t creation_time;      /**< Creation timestamp */
    uint64_t modification_time;  /**< Modification timestamp */
    uint64_t access_time;        /**< Access timestamp */
    uint64_t file_size;          /**< File size in bytes */
    struct extent_run extents[8];/**< Array of extent runs for file data */
    uint8_t  file_type;          /**< Type of file (regular, media image, etc.) */
    uint64_t sector_count;       /**< Total number of sectors in the media image */
    uint64_t sector_map_size;    /**< Number of sector_map_entries written so far */
};

struct __attribute__((packed)) btree_node_dedup {
    struct btree_node_header header;
    /* Followed by node_keys × struct dedup_entry records.
     * Maximum entries per node =
     *   (block_size - sizeof(btree_node_header)) / sizeof(dedup_entry).
     * For a 4096-byte block: (4096 - 70) / 24 = 167 entries. */
};

/** Index entry for B+Tree internal (index) nodes: key + child pointer. */
struct __attribute__((packed)) btree_index_entry {
    uint64_t key;           /**< Smallest key reachable through child */
    uint64_t child_lba;     /**< LBA of the child node */
};

#endif /* OBMAFS3_BTREE_H */