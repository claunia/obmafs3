// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : ps3_read.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — PS3 disc image read path
//
// --[ Description ] ----------------------------------------------------------
//
//     Read path for PS3 disc images: reads decrypted data from dedup,
//     re-encrypts encrypted sectors with per-sector IV for byte-identical
//     reconstruction.
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

#include "aes128.h"
#include "obmafs.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS3_SECTOR_SIZE 2048
#define PS3_MAX_PLAIN_REGIONS 32

/* ---- Cached PS3 info loaded from metadata ---- */

struct ps3_read_info
{
    uint8_t  disc_key[16];
    uint32_t plain_count;
    struct { uint32_t start, end; } plain[PS3_MAX_PLAIN_REGIONS];
};

static int hex2(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int load_ps3_info(struct obmafs3_ctx *ctx, uint64_t iid, struct ps3_read_info *info)
{
    char v[256];
    memset(info, 0, sizeof(*info));

    /* Disc key */
    if(obmafs3_metadata_get(ctx, iid, "__ps3_disc_key__", v, sizeof(v)) != OBMAFS3_OK)
        return OBMAFS3_ERR_NOTFOUND;
    for(int b = 0; b < 16 && v[b * 2] && v[b * 2 + 1]; b++)
    {
        int h = hex2(v[b * 2]), l = hex2(v[b * 2 + 1]);
        if(h >= 0 && l >= 0) info->disc_key[b] = (uint8_t)((h << 4) | l);
    }

    /* Region count */
    if(obmafs3_metadata_get(ctx, iid, "__ps3_region_count__", v, sizeof(v)) == OBMAFS3_OK)
        info->plain_count = (uint32_t)atoi(v);

    /* Regions */
    for(uint32_t i = 0; i < info->plain_count && i < PS3_MAX_PLAIN_REGIONS; i++)
    {
        char k[64];
        snprintf(k, sizeof(k), "__ps3_region_%u_start__", i);
        if(obmafs3_metadata_get(ctx, iid, k, v, sizeof(v)) == OBMAFS3_OK)
            info->plain[i].start = (uint32_t)strtoul(v, NULL, 10);
        snprintf(k, sizeof(k), "__ps3_region_%u_end__", i);
        if(obmafs3_metadata_get(ctx, iid, k, v, sizeof(v)) == OBMAFS3_OK)
            info->plain[i].end = (uint32_t)strtoul(v, NULL, 10);
    }

    return OBMAFS3_OK;
}

static int is_plain_sector(const struct ps3_read_info *info, uint64_t sector)
{
    for(uint32_t i = 0; i < info->plain_count; i++)
    {
        if(sector >= info->plain[i].start && sector <= info->plain[i].end)
            return 1;
    }
    return 0;
}

int obmafs3_read_ps3_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                                size_t size)
{
    /* Read decrypted data from dedup */
    int rc = obmafs3_read_media_image_data(ctx, inode, offset, buf, size, PS3_SECTOR_SIZE, NULL, NULL);
    if(rc != OBMAFS3_OK) return rc;

    /* Load PS3 info */
    struct ps3_read_info info;
    rc = load_ps3_info(ctx, inode->inode_id, &info);
    if(rc != OBMAFS3_OK) return OBMAFS3_OK; /* not a PS3 image or no keys — return as-is */

    /* Re-encrypt encrypted sectors */
    struct aes128_ctx aes;
    aes128_init(&aes, info.disc_key);

    uint8_t *out = (uint8_t *)buf;
    uint64_t pos = offset;
    size_t   rem = size;

    while(rem > 0)
    {
        uint64_t sec_num = pos / PS3_SECTOR_SIZE;
        uint64_t sec_off = pos % PS3_SECTOR_SIZE;

        size_t avail = PS3_SECTOR_SIZE - (size_t)sec_off;
        size_t ch    = rem < avail ? rem : avail;

        if(!is_plain_sector(&info, sec_num))
        {
            /* Re-encrypt this sector. We need the full sector to encrypt,
             * but we may only have a partial sector in our buffer.
             * For simplicity: if we have a full aligned sector, encrypt in-place.
             * For partial sectors, we must read the full sector, encrypt, then copy. */
            if(sec_off == 0 && ch == PS3_SECTOR_SIZE)
            {
                /* Full sector — encrypt in place */
                uint8_t iv[16];
                memset(iv, 0, 16);
                for(int j = 15; j >= 0 && sec_num != 0; j--)
                {
                    iv[j] = (uint8_t)(sec_num & 0xFF);
                    sec_num >>= 8;
                }
                uint8_t enc[PS3_SECTOR_SIZE];
                aes128_cbc_encrypt(&aes, iv, out, enc, PS3_SECTOR_SIZE);
                memcpy(out, enc, PS3_SECTOR_SIZE);
            }
            else
            {
                /* Partial — read full sector from dedup, encrypt, copy piece */
                uint64_t sec_base = (pos / PS3_SECTOR_SIZE) * PS3_SECTOR_SIZE;
                uint8_t  full[PS3_SECTOR_SIZE];
                rc = obmafs3_read_media_image_data(ctx, inode, sec_base, full, PS3_SECTOR_SIZE, PS3_SECTOR_SIZE,
                                                   NULL, NULL);
                if(rc != OBMAFS3_OK) return rc;

                uint64_t sn = pos / PS3_SECTOR_SIZE;
                uint8_t  iv[16];
                memset(iv, 0, 16);
                for(int j = 15; j >= 0 && sn != 0; j--)
                {
                    iv[j] = (uint8_t)(sn & 0xFF);
                    sn >>= 8;
                }
                uint8_t enc[PS3_SECTOR_SIZE];
                aes128_cbc_encrypt(&aes, iv, full, enc, PS3_SECTOR_SIZE);
                memcpy(out, enc + sec_off, ch);
            }
        }

        out += ch;
        pos += ch;
        rem -= ch;
    }

    return OBMAFS3_OK;
}
