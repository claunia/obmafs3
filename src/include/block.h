#ifndef OBMAFS3_BLOCK_H
#define OBMAFS3_BLOCK_H

#include <stdint.h>

/* "OBMABLCK" as little-endian uint64 */
#define OBMAFS3_BLOCK_MAGIC 0x4B434C42414D424FULL

#define OBMAFS3_BLOCK_FLAG_COMPRESSED 0x01

/// On-disk header prepended to every data block.
struct __attribute__((packed)) block_header
{
    uint64_t magic;             ///< "OBMABLCK"
    uint8_t  flags;             ///< Flags indicating if the block is compressed, etc.
    uint8_t  compression_type;  ///< Type of compression used (if applicable)
    uint64_t original_size;     ///< Original size of the data before compression
    uint64_t compressed_size;   ///< Size of the data after compression
    uint8_t  checksum[32];      ///< Checksum of the block data for integrity verification
};

#endif /* OBMAFS3_BLOCK_H */