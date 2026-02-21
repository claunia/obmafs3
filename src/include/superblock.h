#ifndef OBMAFS3_SUPERBLOCK_H
#define OBMAFS3_SUPERBLOCK_H

#include <stdint.h>

/* "OBMAFS_3" as little-endian uint64 */
#define OBMAFS3_SB_MAGIC 0x335F5346414D424FULL

/**
 * Compute the LBA of the backup superblock from total_bytes and block_size.
 * The backup always occupies the last block of the filesystem.
 */
#define OBMAFS3_BACKUP_SB_LBA(total_bytes, block_size) \
    (((total_bytes) / (block_size)) - 1)

/// On-disk superblock occupying LBA 0.
struct __attribute__((packed)) obmafs3_sb
{
    uint64_t magic;              ///< "OBMAFS_3"
    uint8_t  guid[16];           ///< Unique identifier for the filesystem instance
    uint64_t block_size;         ///< Size of each block for non-deduplicated data
    uint64_t dedup_block_size;   ///< Size of each block for deduplicated data
    uint64_t total_bytes;        ///< Total size of the filesystem in bytes
    uint64_t catalog_lba;        ///< LBA of the catalog (btree) structure
    uint64_t inode_lba;          ///< LBA of the inode (btree) structure
    uint64_t overflow_lba;       ///< LBA of the overflow tree for large files
    uint64_t dedup_lba;          ///< LBA of the deduplication tree list header
    uint64_t metadata_lba;       ///< LBA of the metadata (btree) structure
    uint64_t media_tag_lba;      ///< LBA of the media tag (btree) structure
    uint64_t cd_prefix_lba;      ///< LBA of the CD prefix (btree) structure
    uint64_t cd_suffix_lba;      ///< LBA of the CD suffix (btree) structure
    uint64_t cd_subchannel_lba;  ///< LBA of the CD subchannel (btree) structure
    uint64_t metadata_idx_lba;   ///< LBA of the metadata index (btree) structure
    uint64_t refcount_lba;       ///< LBA of the block refcount (btree) structure
    uint16_t checksum_type;      ///< Type of checksum used for the filesystem
    uint64_t creation_time;      ///< Creation time of the filesystem
    uint64_t next_inode_id;      ///< Next available inode ID
    uint64_t bitmap_lba;         ///< LBA of the first allocation bitmap block
    uint64_t bitmap_blocks;      ///< Number of blocks used by the allocation bitmap
    uint8_t  volume_label[256];  ///< Volume label of the filesystem
    uint8_t  checksum[32];       ///< Checksum of the superblock (XXH64, 8 bytes used, 24 zeroed)
    uint64_t keyset_lba;         ///< LBA of the persisted dedup key set (0 = none)
    uint64_t keyset_blocks;      ///< Number of blocks used by the persisted key set
    uint64_t pending_lba;        ///< LBA of the persisted pending insert buffer (0 = none)
    uint64_t pending_blocks;     ///< Number of blocks used by the persisted pending buffer
    uint32_t btree_clump_size;   ///< Nodes to pre-allocate per growth for non-dedup trees (0 = default)
    uint32_t dedup_clump_size;   ///< Nodes to pre-allocate per growth for dedup trees (0 = default)
};

#endif /* OBMAFS3_SUPERBLOCK_H */