// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import_wiiu.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-wiiu — Nintendo Wii U disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Internal header for the Wii U import tool.
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

#ifndef OBMAFS3_IMPORT_WIIU_H
#define OBMAFS3_IMPORT_WIIU_H

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* ---- Wii U disc constants ---- */
#define WIIU_SECTOR_SIZE       0x8000            ///< Wii U disc sector (32 KiB)
#define WIIU_DATA_SECTOR_SIZE  2048              ///< Dedup sector granularity
#define WIIU_ENCRYPTED_OFFSET  0x18000           ///< Encryption starts at this disc offset
#define WIIU_TOC_SECTOR        3                 ///< Sector index of the encrypted TOC
#define WIIU_HEADER_SECTORS    3                 ///< Plaintext disc header sectors (0-2)
#define WIIU_DISC_MAGIC        0xCC549EB9U       ///< Big-endian magic at disc offset 0x10000
#define WIIU_PART_HEADER_MAGIC 0xCC93A4F5U       ///< Big-endian magic at start of partition header
#define WIIU_TOC_SIGNATURE     0xCCA6E67BU       ///< First 4 bytes of correctly decrypted TOC
#define WIIU_TOC_ENTRY_SIZE    0x80              ///< Size of one TOC partition entry
#define WIIU_TOC_ENTRIES_OFF   0x800             ///< Offset of partition entries in decrypted TOC
#define WIIU_MAX_PARTITIONS    8                 ///< Maximum partitions we support

/* ---- WUX compressed format ---- */
#define WUX_MAGIC 0x30585557U ///< "WUX0" as little-endian uint32

struct wux_header
{
    uint32_t magic;             ///< "WUX0" (0x30585557 LE)
    uint32_t reserved;          ///< Reserved / version
    uint32_t sector_size;       ///< Sector size (0x8000)
    uint32_t reserved2;         ///< Must be 0
    uint64_t uncompressed_size; ///< Original disc size in bytes
    uint64_t reserved3;         ///< Must be 0
};

/* ---- Disc reader abstraction (handles both WUD and WUX) ---- */
struct wiiu_reader
{
    int       fd;                ///< File descriptor
    int       is_wux;            ///< 1 if WUX, 0 if raw WUD
    uint64_t  disc_size;         ///< Uncompressed disc size
    /* WUX-specific */
    uint32_t *wux_index;         ///< Sector index table (NULL if WUD)
    uint64_t  wux_data_offset;   ///< File offset where WUX data sectors start
    uint32_t  wux_sector_count;  ///< Number of logical sectors
};

/* ---- Wii U partition entry (parsed from TOC) ---- */
struct wiiu_partition
{
    char     name[128];          ///< Volume name (up to WIIU_TOC_ENTRY_SIZE bytes)
    char     identifier[26];     ///< Short identifier (first 25 bytes + NUL)
    uint32_t start_sector;       ///< First sector of this partition (header sector, plaintext)
    uint8_t  key[16];            ///< Decryption key (disc key for SI/UP/GI, title key for GM)
    int      has_title_key;      ///< 1 if a per-title key was resolved for this partition
};

/* ---- Wii U common key (hardcoded) ---- */
extern const uint8_t WIIU_COMMON_KEY[16];

/* ---- disc.c ---- */
int     wiiu_reader_open(const char *path, struct wiiu_reader *reader);
void    wiiu_reader_close(struct wiiu_reader *reader);
ssize_t wiiu_reader_pread(struct wiiu_reader *reader, void *buf, size_t count, uint64_t offset);
void    wiiu_print_disc_info(const uint8_t *header, uint64_t disc_size);

/* ---- import.c ---- */
int wiiu_parse_toc(struct wiiu_reader *reader, const uint8_t disc_key[16],
                   struct wiiu_partition *parts, int *part_count);
int wiiu_extract_title_keys(struct wiiu_reader *reader, const uint8_t disc_key[16],
                            struct wiiu_partition *parts, int part_count);
int wiiu_import(struct wiiu_reader *reader, int out_fd,
                const struct wiiu_partition *parts, int part_count);

/* ---- metadata.c ---- */
void wiiu_import_metadata(int out_fd, const uint8_t *header);

#endif /* OBMAFS3_IMPORT_WIIU_H */
