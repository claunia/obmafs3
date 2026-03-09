// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : disc.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Sector 0/1 parsing, encryption region map, key derivation.
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

#include "../../src/lib/aes128.h"
#include "import_ps3.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- PS3 crypto constants ---- */
const uint8_t PS3_ERK[16]    = {0x38, 0x0B, 0xCF, 0x0B, 0x53, 0x45, 0x5B, 0x3C,
                                0x78, 0x17, 0xAB, 0x4F, 0xA3, 0xBA, 0x90, 0xED};
const uint8_t PS3_ERK_IV[16] = {0x69, 0x47, 0x47, 0x72, 0xAF, 0x6F, 0xDA, 0xB3,
                                0x42, 0x74, 0x3A, 0xEF, 0xAA, 0x18, 0x62, 0x87};

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int ps3_read_region_map(int fd, struct ps3_region_map *map)
{
    uint8_t sector[PS3_SECTOR_SIZE];
    if(pread(fd, sector, PS3_SECTOR_SIZE, 0) < PS3_SECTOR_SIZE)
    {
        fprintf(stderr, "Error: cannot read sector 0\n");
        return -1;
    }

    memset(map, 0, sizeof(*map));

    uint32_t count = be32(sector + 0);
    /* uint32_t unknown = be32(sector + 4); */

    if(count > PS3_MAX_PLAIN_REGIONS) count = PS3_MAX_PLAIN_REGIONS;
    map->count = count;

    for(uint32_t i = 0; i < count; i++)
    {
        uint32_t off = 8 + i * 8;
        map->regions[i].start_sector = be32(sector + off);
        map->regions[i].end_sector   = be32(sector + off + 4);
    }

    /* Determine total sectors from file size */
    struct stat st;
    if(fstat(fd, &st) == 0)
        map->total_sectors = (uint64_t)st.st_size / PS3_SECTOR_SIZE;

    return 0;
}

int ps3_read_disc_info(int fd, char *disc_id, size_t disc_id_len)
{
    uint8_t sector[PS3_SECTOR_SIZE];
    if(pread(fd, sector, PS3_SECTOR_SIZE, PS3_SECTOR_SIZE) < PS3_SECTOR_SIZE)
    {
        fprintf(stderr, "Error: cannot read sector 1\n");
        return -1;
    }

    /* Verify "PlayStation3" at offset 0 */
    if(memcmp(sector, "PlayStation3", 12) != 0)
    {
        fprintf(stderr, "Error: sector 1 does not contain PlayStation3 identifier\n");
        return -1;
    }

    /* Disc ID at offset 0x10, 32 bytes, space-padded */
    size_t copy = disc_id_len - 1;
    if(copy > 32) copy = 32;
    memcpy(disc_id, sector + 0x10, copy);
    disc_id[copy] = '\0';

    /* Trim trailing spaces */
    for(int i = (int)copy - 1; i >= 0 && disc_id[i] == ' '; i--)
        disc_id[i] = '\0';

    return 0;
}

int ps3_is_encrypted_sector(const struct ps3_region_map *map, uint64_t sector)
{
    for(uint32_t i = 0; i < map->count; i++)
    {
        if(sector >= map->regions[i].start_sector && sector <= map->regions[i].end_sector)
            return 0; /* in a plaintext region */
    }
    return 1; /* encrypted */
}

void ps3_derive_disc_key(const uint8_t d1[16], uint8_t disc_key[16])
{
    struct aes128_ctx aes;
    aes128_init(&aes, PS3_ERK);
    aes128_cbc_encrypt(&aes, PS3_ERK_IV, d1, disc_key, 16);
}

void ps3_sector_iv(uint64_t sector_num, uint8_t iv[16])
{
    memset(iv, 0, 16);
    for(int j = 15; j >= 0 && sector_num != 0; j--)
    {
        iv[j] = (uint8_t)(sector_num & 0xFF);
        sector_num >>= 8;
    }
}
