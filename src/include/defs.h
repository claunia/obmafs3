struct dedup_entry {
    uint64_t hash; //< Hash of the deduplicated data block
    uint64_t block_lba; //< Logical block address where the deduplicated data block is stored
    uint64_t block_offset; //< Offset within the block where the deduplicated data starts
};

struct tree_list_header {
    uint64_t magic; //< "TREELIST"
    uint64_t tree_count; //< Total number of btrees in the list
    uint8_t checksum[32]; //< Checksum of the tree list header block for integrity verification
    // Followed by an array of tree_list_entry structures, one for each btree in the list
};

struct tree_list_entry {
    uint16_t sector_size; //< Size of each sector in bytes
    uint64_t tree_lba; //< Logical block address where the btree is stored
};

struct sector_map_entry {
    int64_t sector;
    uint16_t sector_size; //< Size of the sector in bytes
    uint64_t hash; //< Hash of the data in the sector for deduplication purposes
};

struct extent_run {
    uint64_t start_block; //< Starting block of the extent run
    uint64_t block_count; //< Number of blocks in the extent run
};