// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : sha1.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — embedded SHA-1
//
// --[ Description ] ----------------------------------------------------------
//
//     Minimal SHA-1 implementation for Nintendo Wii disc hash verification.
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

#ifndef OBMAFS3_SHA1_H
#define OBMAFS3_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_SIZE 20
#define SHA1_BLOCK_SIZE  64

struct sha1_ctx
{
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[SHA1_BLOCK_SIZE];
};

void sha1_init(struct sha1_ctx *ctx);
void sha1_update(struct sha1_ctx *ctx, const void *data, size_t len);
void sha1_final(struct sha1_ctx *ctx, uint8_t digest[SHA1_DIGEST_SIZE]);

/** Convenience: compute SHA-1 of a single buffer. */
void sha1_hash(const void *data, size_t len, uint8_t digest[SHA1_DIGEST_SIZE]);

#endif /* OBMAFS3_SHA1_H */
