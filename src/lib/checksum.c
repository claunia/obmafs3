/*
 * checksum.c - OBMAFS3 checksum operations using XXH64
 */
#include "obmafs.h"

#include <xxhash.h>
#include <string.h>

/**
 * Compute an XXH64 hash of the given data.
 *
 * @param data  Pointer to the data buffer.
 * @param size  Number of bytes to hash.
 * @return The 64-bit XXH64 hash value (seed 0).
 */
uint64_t obmafs3_checksum_xxh64(const void *data, size_t size)
{
    return XXH64(data, size, 0);
}

/**
 * Compute an XXH64 hash and store it in a 32-byte output buffer.
 *
 * The first 8 bytes of @p out receive the hash; the remaining 24 bytes
 * are zeroed.
 *
 * @param data  Pointer to the data buffer.
 * @param size  Number of bytes to hash.
 * @param out   Output buffer (must be at least 32 bytes).
 */
void obmafs3_checksum_block(const void *data, size_t size, uint8_t *out)
{
    uint64_t hash = XXH64(data, size, 0);
    memset(out, 0, 32);
    memcpy(out, &hash, sizeof(hash));
}
