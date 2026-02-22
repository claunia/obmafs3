// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : block.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     On-disk block structure for OBMAFS3.
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