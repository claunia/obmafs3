// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import_ps3.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Internal header for the PS3 import tool.
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

#ifndef OBMAFS3_IMPORT_PS3_H
#define OBMAFS3_IMPORT_PS3_H

#include <stdint.h>
#include <unistd.h>

/* ---- PS3 constants ---- */
#define PS3_SECTOR_SIZE 2048
#define PS3_MAX_PLAIN_REGIONS 32

/* PS3 encryption round key — used with AES-128-CBC encrypt to derive disc key from d1 */
extern const uint8_t PS3_ERK[16];
/* PS3 IV for key derivation */
extern const uint8_t PS3_ERK_IV[16];

/* ---- Encryption region map (from sector 0) ---- */
struct ps3_plain_region
{
    uint32_t start_sector;
    uint32_t end_sector; /* inclusive */
};

struct ps3_region_map
{
    struct ps3_plain_region regions[PS3_MAX_PLAIN_REGIONS];
    uint32_t                count;
    uint64_t                total_sectors;
};

/* ---- IRD parsed data ---- */
struct ps3_ird_data
{
    int      valid;
    char     game_id[10];
    char     game_name[256];
    char     update_ver[5];
    char     game_ver[6];
    char     app_ver[6];
    uint8_t  d1[16];
    uint8_t  d2[16];
    uint8_t  pic[115];
    int      has_pic;
    uint8_t *header_gz;
    uint32_t header_gz_len;
    uint8_t *footer_gz;
    uint32_t footer_gz_len;
};

/* ---- SFO parsed data ---- */
struct ps3_sfo_entry
{
    char key[64];
    char value[256];
};

struct ps3_sfo_data
{
    struct ps3_sfo_entry entries[32];
    int                  count;
};

/* ---- disc.c ---- */
int  ps3_read_region_map(int fd, struct ps3_region_map *map);
int  ps3_read_disc_info(int fd, char *disc_id, size_t disc_id_len);
int  ps3_is_encrypted_sector(const struct ps3_region_map *map, uint64_t sector);
void ps3_derive_disc_key(const uint8_t d1[16], uint8_t disc_key[16]);
void ps3_sector_iv(uint64_t sector_num, uint8_t iv[16]);

/* ---- ird.c ---- */
int ps3_parse_ird(const char *path, struct ps3_ird_data *ird);
void ps3_ird_free(struct ps3_ird_data *ird);

/* ---- iso9660.c ---- */
int ps3_iso9660_read_file(int fd, const char *path, uint8_t **data, size_t *size);

/* ---- sfo.c ---- */
int ps3_parse_sfo(const uint8_t *data, size_t size, struct ps3_sfo_data *sfo);
const char *ps3_sfo_get(const struct ps3_sfo_data *sfo, const char *key);

/* ---- import.c ---- */
int ps3_import(int iso_fd, int out_fd, const uint8_t disc_key[16], const struct ps3_region_map *map);

/* ---- metadata.c ---- */
void ps3_import_metadata(int out_fd, const char *disc_id, const struct ps3_sfo_data *sfo,
                         const struct ps3_ird_data *ird);

#endif /* OBMAFS3_IMPORT_PS3_H */
