// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : checksum.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 checksum operations using XXH64.
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

#include "obmafs.h"

#include <string.h>
#include <xxhash.h>

/**
 * Compute an XXH64 hash of the given data.
 *
 * @param data  Pointer to the data buffer.
 * @param size  Number of bytes to hash.
 * @return The 64-bit XXH64 hash value (seed 0).
 */
uint64_t obmafs3_checksum_xxh64(const void *data, size_t size) { return XXH64(data, size, 0); }

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
