struct obmafs3_sb {
    uint64_t magic; //< "OBMAFS_3"
    uint8_t guid[16]; //< Unique identifier for the filesystem instance
    uint64_t block_size; //< Size of each block for non-deduplicated data
    uint64_t dedup_block_size; //< Size of each block for deduplicated data
    uint64_t total_bytes; //< Total size of the filesystem in bytes
    uint64_t catalog_lba; //< Logical block address of the catalog (btree) structure
    uint64_t inode_lba; //< Logical block address of the inode (btree) structure
    uint64_t overflow_lba; //< Logical block address of the overflow tree for large files
    uint64_t dedup_lba; //< Logical block address of the deduplication tree list header
    uint64_t metadata_lba; //< Logical block address of the metadata (btree)structure
    uint64_t media_tag_lba; //< Logical block address of the media tag (btree) structure
    uint16_t checksum_type; //< Type of checksum used for the filesystem
    uint64_t creation_time; //< Creation time of the filesystem
    uint8_t volume_label[256]; //< Volume label of the filesystem
};