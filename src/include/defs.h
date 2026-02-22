#ifndef OBMAFS3_DEFS_H
#define OBMAFS3_DEFS_H

#include <stdint.h>

/* "TREELIST" as little-endian uint64 */
#define OBMAFS3_TREELIST_MAGIC 0x5453494C45455254ULL

/* "OBMABMAP" as little-endian uint64 */
#define OBMAFS3_BITMAP_MAGIC 0x50414D42414D424FULL

#define OBMAFS3_DEFAULT_BLOCK_SIZE       4096
#define OBMAFS3_DEFAULT_DEDUP_BLOCK_SIZE 4194304

/// Number of blocks in a compression group (16 × 4096 = 64 KiB).
#define OBMAFS3_COMPRESS_GROUP_BLOCKS 16

#define OBMAFS3_ROOT_INODE_ID 2

/* ---- CD sector geometry constants ---- */
#define CD_RAW_SECTOR_SIZE 2352
#define CD_RAW_PLUS_SUB    2448
#define CD_SUBCHANNEL_SIZE 96
#define CD_PREFIX_SIZE     16
#define CD_SUFFIX_SIZE     288
#define CD_DATA_SIZE       2048 /* 2352 - 16 - 288 */

/// Single deduplication hash-to-block mapping entry.
struct __attribute__((packed)) dedup_entry
{
    uint64_t hash;          ///< Hash of the deduplicated data block
    uint64_t block_lba;     ///< LBA where the deduplicated data block is stored
    uint64_t block_offset;  ///< Offset within the block where dedup data starts
};

/// On-disk header for the deduplication tree list.
struct __attribute__((packed)) tree_list_header
{
    uint64_t magic;         ///< "TREELIST"
    uint64_t tree_count;    ///< Total number of btrees in the list
    uint8_t  checksum[32];  ///< Checksum of the tree list header block
    /// Followed by an array of tree_list_entry structures
};

/// Entry in the deduplication tree list mapping a sector size to its B+Tree.
struct __attribute__((packed)) tree_list_entry
{
    uint16_t sector_size;  ///< Size of each sector in bytes
    uint64_t tree_lba;     ///< LBA where the btree is stored
};

/// Maps a logical sector to its deduplicated data hash.
struct __attribute__((packed)) sector_map_entry
{
    int64_t  sector;       ///< Logical sector number within the disk image
    uint16_t sector_size;  ///< Size of the sector in bytes
    uint64_t hash;         ///< Hash of the sector data for deduplication
};

/// Maps a CD logical sector to its deduplicated data, prefix, suffix and subchannel hashes.
struct __attribute__((packed)) cd_sector_map_entry
{
    int64_t  sector;            ///< Logical sector number within the CD image
    uint16_t sector_size;       ///< Size of the CD sector in bytes (e.g. 2048, 2336, 2352)
    uint64_t hash;              ///< Hash of the CD sector data for deduplication
    uint8_t  generated_prefix;  ///< Indicates if prefix can be generated and is therefore not stored
    uint64_t prefix_hash;       ///< Hash of the CD sector prefix data (e.g. 16 bytes before main data)
    uint8_t  generated_suffix;  ///< Indicates if suffix can be generated and is therefore not stored
    uint64_t suffix_hash;       ///< Hash of the CD sector suffix data (e.g. 288 bytes after main data)
    uint64_t subchannel_hash;   ///< Hash of the CD sector subchannel data (e.g. 96 bytes), 0 if not stored
    uint8_t  subheader[8];      ///< Subheader data for CD-ROM XA sectors, 0 if not applicable
    uint8_t  sector_mode;       ///< Audio, Mode 1, Mode 2 Form 1, Mode 2 Form 2, etc. for CD-ROM XA sectors
};

/// Contiguous range of allocated blocks.
struct __attribute__((packed)) extent_run
{
    uint64_t start_block;     ///< Starting physical LBA of the extent run
    uint64_t block_count;     ///< Number of physical blocks in the extent run
    uint64_t logical_blocks;  ///< Number of logical blocks this extent covers
    ///< When logical_blocks == block_count the data is stored
    ///< uncompressed (one physical block per logical block, no
    ///< header).  When logical_blocks > block_count the physical
    ///< blocks contain a block_header followed by ZSTD-compressed
    ///< data covering logical_blocks × block_size bytes.
};

/**
 * An overflow extent entry stored in the overflow tree.
 * Associates an inode_id with an extent_run for files that
 * need more than 8 inline extents.
 *
 * Sorted by (inode_id, logical_offset) in the B+Tree.
 */
struct __attribute__((packed)) overflow_extent
{
    uint64_t inode_id;       ///< Inode this extent belongs to
    uint64_t logical_offset; ///< First logical block covered by this extent (sort key)
    uint64_t start_block;    ///< Starting physical LBA of the extent run
    uint64_t block_count;    ///< Number of physical blocks in the extent run
    uint64_t logical_count;  ///< Number of logical blocks this extent covers
};

/**
 * Block reference count record stored in the refcount B+Tree.
 * Keyed by LBA.  Only blocks with ref_count > 1 need an entry;
 * absence means refcount == 1 (or unallocated).
 */
struct __attribute__((packed)) refcount_record
{
    uint64_t lba;        ///< Block LBA
    uint32_t ref_count;  ///< Number of inodes sharing this block
};

/// On-disk header for the allocation bitmap.
struct __attribute__((packed)) bitmap_header
{
    uint64_t magic;          ///< "OBMABMAP"
    uint64_t total_blocks;   ///< Total number of blocks tracked by bitmap
    uint64_t next_free_lba;  ///< Allocation hint: next LBA to try when searching for free space
    uint8_t  checksum[32];   ///< Checksum of the bitmap data
};

/// Context for CD-ROM EDC/ECC computation and verification.
typedef struct CdEccContext
{
    bool      inited_edc;   ///< True once EDC/ECC tables have been initialized.
    uint8_t  *ecc_b_table;  ///< Backward (B) ECC table (allocated, size implementation-defined).
    uint8_t  *ecc_f_table;  ///< Forward (F) ECC table.
    uint32_t *edc_table;    ///< EDC (CRC) lookup table.
} CdEccContext;

#endif /* OBMAFS3_DEFS_H */