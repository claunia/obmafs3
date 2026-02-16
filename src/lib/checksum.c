/*
 * checksum.c - OBMAFS3 checksum operations using XXH64
 */
#include "obmafs.h"

#include <xxhash.h>
#include <string.h>

uint64_t obmafs3_checksum_xxh64(const void *data, size_t size)
{
    return XXH64(data, size, 0);
}

void obmafs3_checksum_block(const void *data, size_t size, uint8_t *out)
{
    uint64_t hash = XXH64(data, size, 0);
    memset(out, 0, 32);
    memcpy(out, &hash, sizeof(hash));
}
