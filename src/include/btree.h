// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : btree.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     On-disk B+Tree structure for OBMAFS3.
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

#ifndef OBMAFS3_BTREE_H
#define OBMAFS3_BTREE_H

#include <stdint.h>
#include "defs.h"

/* "BTREEHDR" as little-endian uint64 */
#define OBMAFS3_BTREE_HDR_MAGIC  0x5244484545525442ULL
/* "BTREENDE" as little-endian uint64 */
#define OBMAFS3_BTREE_NODE_MAGIC 0x45444E4545525442ULL

/// Default clump size for dedup trees (nodes per allocation).
#define OBMAFS3_DEDUP_CLUMP_SIZE   1024
/// Default clump size for all other trees (nodes per allocation).
#define OBMAFS3_DEFAULT_CLUMP_SIZE 64

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
 * Index entry for overflow B+Tree internal nodes.
 * Uses composite key (inode_id, logical_offset) so that files with
 * many extents are correctly routed across multiple leaves.
 */
struct __attribute__((packed)) overflow_index_entry
{
    uint64_t inode_id;        ///< Smallest inode_id reachable through child
    uint64_t logical_offset;  ///< Smallest logical_offset reachable through child
    uint64_t child_lba;       ///< LBA of the child node
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

/* ---- User-defined index registry (on-disk linked list) ---- */

#define USER_INDEX_VALUE_TYPE_STRING  0
#define USER_INDEX_VALUE_TYPE_NUMERIC 1

/**
 * A single entry in the user-defined index registry.
 * Maps a metadata key name to its dedicated secondary B+Tree.
 */
struct __attribute__((packed)) user_index_registry_entry
{
    char     key[METADATA_KEY_MAX];  ///< Metadata key this index covers
    uint8_t  value_type;             ///< 0 = string, 1 = numeric
    uint8_t  _pad[7];               ///< Alignment padding
    uint64_t index_header_lba;       ///< LBA of the per-key B+Tree header
};

/**
 * On-disk linked list block for the user-defined index registry.
 * Each block contains up to N entries followed by a next pointer.
 * The maximum entries per block depends on block size:
 *   (block_size - 16) / sizeof(user_index_registry_entry)
 */
struct __attribute__((packed)) user_index_registry_block
{
    uint64_t next_lba;     ///< LBA of next registry block (0 = end of chain)
    uint32_t entry_count;  ///< Number of valid entries in this block
    uint32_t _pad;         ///< Alignment padding
    /* Followed by entry_count * user_index_registry_entry */
};

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

/* ---- Numeric metadata index B+Tree ---- */

/**
 * Numeric metadata index record stored in leaf nodes.
 * Sorted by (key, numeric_value, inode_id) composite key.
 * numeric_value is an int64_t parsed from the string metadata value.
 * Enables efficient range queries on numeric metadata fields.
 */
struct __attribute__((packed)) metadata_numeric_idx_record
{
    char     key[METADATA_KEY_MAX];      ///< Metadata key
    int64_t  value;                      ///< Numeric value (parsed from string)
    uint64_t inode_id;                   ///< Disk image inode
};

/**
 * Index entry for numeric metadata index B+Tree internal nodes.
 * Uses composite key (key, numeric_value, inode_id).
 */
struct __attribute__((packed)) metadata_numeric_idx_index_entry
{
    char     key[METADATA_KEY_MAX];      ///< Smallest key reachable through child
    int64_t  value;                      ///< Smallest numeric value reachable through child
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

/* ---- Sector Tag B+Trees (hash-dedup variant) ---- */

/// Maximum inline sector tag data size (covers all known tag types).
#define SECTOR_TAG_DATA_MAX 64

/**
 * Sector Tag Data Tree record.  Hash-keyed dictionary of unique tag blobs.
 * Key: XXH64(tag_type || data[0..data_length-1]).
 */
struct __attribute__((packed)) sector_tag_data_record
{
    uint64_t hash;                          ///< XXH64(tag_type || data)
    uint16_t tag_type;                      ///< SectorTagType enum value
    uint16_t data_length;                   ///< Actual bytes of tag data
    uint8_t  data[SECTOR_TAG_DATA_MAX];     ///< Inline tag data
};

/**
 * Sector Tag Ref Tree record.  Maps (inode_id, sector, tag_type) to
 * a hash in the Sector Tag Data Tree.
 */
struct __attribute__((packed)) sector_tag_ref_record
{
    uint64_t inode_id;     ///< Image inode
    int64_t  sector;       ///< Logical sector number
    uint16_t tag_type;     ///< SectorTagType discriminator
    uint64_t tag_hash;     ///< XXH64(tag_type || data) -> key into data tree
};

/// Index entry for the Sector Tag Ref Tree.
struct __attribute__((packed)) sector_tag_ref_index_entry
{
    uint64_t inode_id;     ///< Smallest inode_id reachable through child
    int64_t  sector;       ///< Smallest sector reachable through child
    uint16_t tag_type;     ///< Smallest tag_type reachable through child
    uint64_t child_lba;    ///< LBA of the child node
};

/* ---- Junk Map B+Tree (Nintendo disc junk/padding seeds) ---- */

/// Junk map leaf record: one junk region with its LFG seed inline.
/// Keyed by composite (inode_id, offset).
/// Records per leaf (4096-byte block): (4096 - 70) / 94 = 42.
struct __attribute__((packed)) junk_map_record
{
    uint64_t inode_id;                     ///< Image inode
    uint64_t offset;                       ///< Disc offset (GC) or logical offset in partition (Wii)
    uint64_t length;                       ///< Junk region length in bytes
    uint16_t partition_index;              ///< Partition index (0xFFFF = GC / outside partitions)
    uint32_t seed[NGC_LFG_SEED_SIZE];     ///< 17-word LFG seed (big-endian)
};

/// Index entry for the Junk Map B+Tree.
struct __attribute__((packed)) junk_map_index_entry
{
    uint64_t inode_id;     ///< Smallest inode_id reachable through child
    uint64_t offset;       ///< Smallest offset reachable through child
    uint64_t child_lba;    ///< LBA of the child node
};

#endif /* OBMAFS3_BTREE_H */