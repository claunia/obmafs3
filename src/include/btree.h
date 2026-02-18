#ifndef OBMAFS3_BTREE_H
#define OBMAFS3_BTREE_H

#include <stdint.h>
#include "defs.h"

/* "BTREEHDR" as little-endian uint64 */
#define OBMAFS3_BTREE_HDR_MAGIC  0x5244484545525442ULL
/* "BTREENDE" as little-endian uint64 */
#define OBMAFS3_BTREE_NODE_MAGIC 0x45444E4545525442ULL

/// On-disk B+Tree header stored at the tree's anchor LBA.
struct __attribute__((packed)) btree_header
{
    uint64_t magic;              ///< "BTREEHDR"
    uint32_t data_type;          ///< Type of data stored in the btree
    uint64_t root_node_lba;      ///< LBA of the root node
    uint64_t free_node_lba;      ///< LBA of the first free node
    uint16_t node_size;          ///< Size of each node in bytes
    uint32_t total_nodes;        ///< Total number of nodes
    uint32_t free_nodes;         ///< Number of free nodes
    uint32_t tree_type;          ///< Type of btree (catalog, dedup, metadata, etc.)
    uint64_t last_block_lba;     ///< LBA of the last partially written data block (dedup trees)
    uint64_t last_block_offset;  ///< Byte offset within last_block_lba where next write starts
    uint8_t  checksum[32];       ///< Checksum of the btree header block
};

/// Common header shared by every B+Tree node (leaf and index).
struct __attribute__((packed)) btree_node_header
{
    uint64_t magic;          ///< "BTREENDE"
    uint8_t  record_type;    ///< Type of records in this node
    uint8_t  level;          ///< 0 = leaf node, >0 = index node (B+Tree depth)
    uint64_t left_link;      ///< LBA of left sibling node
    uint64_t right_link;     ///< LBA of right sibling node
    uint64_t overflow_link;  ///< LBA of overflow node
    uint16_t node_keys;      ///< Number of keys in this node
    uint16_t keys_length;    ///< Total length of keys in this node
    uint8_t  checksum[32];   ///< Checksum of the btree node block
};

/// B+Tree node containing a single filename record (used during lookups).
struct __attribute__((packed)) btree_node_filename
{
    struct btree_node_header header;
    uint64_t                 inode_id;        ///< Unique identifier for the file or directory
    uint64_t                 parent_id;       ///< Identifier of the parent directory
    uint8_t                  directory_flag;  ///< 1 if directory, 0 if file
    char                     name[256];       ///< Name in UTF-8 NUL-terminated format
};

/**
 * Catalog record stored in B+Tree leaf nodes.
 * Sorted by (parent_id, name) as the composite key.
 */
struct __attribute__((packed)) catalog_record
{
    uint64_t inode_id;        ///< Unique identifier for the file or directory
    uint64_t parent_id;       ///< Identifier of the parent directory
    uint8_t  directory_flag;  ///< 1 if directory, 0 if file
    char     name[256];       ///< Name in UTF-8 NUL-terminated format
};

/**
 * Inode record stored in B+Tree leaf nodes.
 * This is the public API type for inode operations and the on-disk format.
 */
struct __attribute__((packed)) inode_record
{
    uint64_t          inode_id;           ///< Unique identifier for the file
    uint32_t          uid;                ///< User ID of the file owner
    uint32_t          gid;                ///< Group ID of the file owner
    uint32_t          mode;               ///< File permissions
    uint64_t          creation_time;      ///< Creation timestamp
    uint64_t          modification_time;  ///< Modification timestamp
    uint64_t          access_time;        ///< Access timestamp
    uint64_t          file_size;          ///< File size in bytes
    struct extent_run extents[8];         ///< Array of extent runs for file data
    uint8_t           file_type;          ///< Type of file (regular, media image, etc.)
    uint64_t          sector_count;       ///< Total number of sectors in the media image
    uint64_t          sector_map_size;    ///< Number of sector_map_entries written so far
    uint32_t          ref_count;          ///< Number of catalog entries (hardlinks) pointing to this inode
};

/// B+Tree node for deduplication entries.
struct __attribute__((packed)) btree_node_dedup
{
    struct btree_node_header header;
    /// Followed by node_keys × struct dedup_entry records.
    /// Maximum entries per node =
    ///   (block_size - sizeof(btree_node_header)) / sizeof(dedup_entry).
    /// For a 4096-byte block: (4096 - 70) / 24 = 167 entries.
};

/** Index entry for B+Tree internal (index) nodes: key + child pointer. */
struct __attribute__((packed)) btree_index_entry
{
    uint64_t key;        ///< Smallest key reachable through child
    uint64_t child_lba;  ///< LBA of the child node
};

/**
 * Index entry for catalog B+Tree internal nodes.
 * Uses the full composite key (parent_id, name) so that directories
 * with hundreds of entries are correctly routed across multiple leaves.
 */
struct __attribute__((packed)) catalog_index_entry
{
    uint64_t parent_id;  ///< Smallest parent_id reachable through child
    char     name[256];  ///< Smallest name reachable through child
    uint64_t child_lba;  ///< LBA of the child node
};

/* ---- Media tag B+Tree ---- */

#define MEDIA_TAG_INLINE_MAX  512
#define MEDIA_TAG_FLAG_INLINE 0x01

/**
 * Media tag record stored in B+Tree leaf nodes.
 * Sorted by (inode_id, tag_type) composite key.
 * Tags up to MEDIA_TAG_INLINE_MAX bytes are stored inline;
 * larger tags are stored in separately allocated blocks.
 */
struct __attribute__((packed)) media_tag_record
{
    uint64_t inode_id;                           ///< Disk image inode this tag belongs to
    uint16_t tag_type;                           ///< MediaTagType enum value
    uint32_t data_length;                        ///< Total length of the tag data
    uint8_t  flags;                              ///< MEDIA_TAG_FLAG_INLINE if data is inline
    uint64_t data_lba;                           ///< LBA of external data blocks (0 if inline)
    uint64_t data_blocks;                        ///< Number of external blocks allocated (0 if inline)
    uint8_t  inline_data[MEDIA_TAG_INLINE_MAX];  ///< Inline data storage
};

/**
 * Index entry for media tag B+Tree internal nodes.
 * Uses composite key (inode_id, tag_type).
 */
struct __attribute__((packed)) media_tag_index_entry
{
    uint64_t inode_id;   ///< Smallest inode_id reachable through child
    uint16_t tag_type;   ///< Smallest tag_type reachable through child
    uint64_t child_lba;  ///< LBA of the child node
};

/* ---- Image metadata B+Trees ---- */

#define METADATA_KEY_MAX     256  /* 255 chars + NUL */
#define METADATA_VALUE_MAX   1025 /* 1024 chars + NUL */
#define METADATA_NODE_BLOCKS 8    /* Blocks per metadata tree node */

/**
 * Metadata record stored in the per-image metadata B+Tree leaf nodes.
 * Sorted by (inode_id, key) composite key.
 * Stores arbitrary key=value string pairs for disk/CD images.
 */
struct __attribute__((packed)) metadata_record
{
    uint64_t inode_id;                   ///< Disk image inode this entry belongs to
    char     key[METADATA_KEY_MAX];      ///< Metadata key (NUL-terminated, max 255 chars)
    char     value[METADATA_VALUE_MAX];  ///< Metadata value (NUL-terminated, max 1024 chars)
};

/**
 * Index entry for per-image metadata B+Tree internal nodes.
 * Uses composite key (inode_id, key).
 */
struct __attribute__((packed)) metadata_index_entry
{
    uint64_t inode_id;               ///< Smallest inode_id reachable through child
    char     key[METADATA_KEY_MAX];  ///< Smallest key reachable through child
    uint64_t child_lba;              ///< LBA of the child node
};

/**
 * Metadata index record stored in the reverse-index B+Tree leaf nodes.
 * Sorted by (key, value, inode_id) composite key.
 * Enables queries like "which images have dumper=natalia portillo?".
 */
struct __attribute__((packed)) metadata_idx_record
{
    char     key[METADATA_KEY_MAX];      ///< Metadata key
    char     value[METADATA_VALUE_MAX];  ///< Metadata value
    uint64_t inode_id;                   ///< Disk image inode
};

/**
 * Index entry for metadata reverse-index B+Tree internal nodes.
 * Uses composite key (key, value, inode_id).
 */
struct __attribute__((packed)) metadata_idx_index_entry
{
    char     key[METADATA_KEY_MAX];      ///< Smallest key reachable through child
    char     value[METADATA_VALUE_MAX];  ///< Smallest value reachable through child
    uint64_t inode_id;                   ///< Smallest inode_id reachable through child
    uint64_t child_lba;                  ///< LBA of the child node
};

/* ---- Compact Disc image B+Trees ---- */

#define CD_PREFIX_DATA_SIZE     16
#define CD_SUFFIX_DATA_SIZE     288
#define CD_SUBCHANNEL_DATA_SIZE 96

/**
 * CD prefix record stored in B+Tree leaf nodes.
 * Key: XXH64 hash of the prefix data.
 * Data: 16-byte prefix stored inline.
 */
struct __attribute__((packed)) cd_prefix_record
{
    uint64_t hash;                       ///< XXH64 hash of the prefix
    uint8_t  data[CD_PREFIX_DATA_SIZE];  ///< Inline prefix data
};

/**
 * CD suffix record stored in B+Tree leaf nodes.
 * Key: XXH64 hash of the suffix data.
 * Data: 288-byte suffix stored inline.
 */
struct __attribute__((packed)) cd_suffix_record
{
    uint64_t hash;                       ///< XXH64 hash of the suffix
    uint8_t  data[CD_SUFFIX_DATA_SIZE];  ///< Inline suffix data
};

/**
 * CD subchannel record stored in B+Tree leaf nodes.
 * Key: XXH64 hash of the subchannel data.
 * Data: 96-byte subchannel stored inline.
 */
struct __attribute__((packed)) cd_subchannel_record
{
    uint64_t hash;                           ///< XXH64 hash of the subchannel
    uint8_t  data[CD_SUBCHANNEL_DATA_SIZE];  ///< Inline subchannel data
};

#endif /* OBMAFS3_BTREE_H */