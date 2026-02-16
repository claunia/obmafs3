struct block_header {
    uint64_t magic; //< "OBMABLCK"
    uint8_t flags; //< Flags indicating if the block is compressed, etc.
    uint8_t compression_type; //< Type of compression used for the block (if applicable)
    uint64_t original_size; //< Original size of the data before compression (if applicable)
    uint64_t compressed_size; //< Size of the data in the block (after compression if applicable)
    uint8_t checksum[32]; //< Checksum of the block data for integrity verification
};