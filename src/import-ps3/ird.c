// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : ird.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     IRD file parser (versions 6-9).
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

#include "import_ps3.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ps3_parse_ird(const char *path, struct ps3_ird_data *ird)
{
    memset(ird, 0, sizeof(*ird));

    int fd = open(path, O_RDONLY);
    if(fd < 0) return -1;

    struct stat st;
    if(fstat(fd, &st) < 0) { close(fd); return -1; }

    uint8_t *raw = malloc((size_t)st.st_size);
    if(!raw) { close(fd); return -1; }
    if(read(fd, raw, (size_t)st.st_size) != st.st_size) { free(raw); close(fd); return -1; }
    close(fd);

    /* Check if gzip-compressed */
    uint8_t *data;
    size_t   data_len;

    if(st.st_size >= 2 && raw[0] == 0x1F && raw[1] == 0x8B)
    {
        /* Gzip-compressed — decompress */
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        zs.next_in  = raw;
        zs.avail_in = (uInt)st.st_size;

        if(inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) { free(raw); return -1; }

        size_t   out_cap = (size_t)st.st_size * 4;
        uint8_t *out     = malloc(out_cap);
        if(!out) { inflateEnd(&zs); free(raw); return -1; }

        zs.next_out  = out;
        zs.avail_out = (uInt)out_cap;

        while(1)
        {
            int ret = inflate(&zs, Z_NO_FLUSH);
            if(ret == Z_STREAM_END) break;
            if(ret != Z_OK)
            {
                /* Need more output space */
                if(ret == Z_BUF_ERROR || zs.avail_out == 0)
                {
                    size_t   used    = out_cap - zs.avail_out;
                    out_cap *= 2;
                    uint8_t *tmp = realloc(out, out_cap);
                    if(!tmp) { free(out); inflateEnd(&zs); free(raw); return -1; }
                    out          = tmp;
                    zs.next_out  = out + used;
                    zs.avail_out = (uInt)(out_cap - used);
                    continue;
                }
                free(out);
                inflateEnd(&zs);
                free(raw);
                return -1;
            }
        }

        data_len = out_cap - zs.avail_out;
        data     = out;
        inflateEnd(&zs);
        free(raw);
    }
    else
    {
        data     = raw;
        data_len = (size_t)st.st_size;
    }

    /* Parse IRD */
    if(data_len < 20 || memcmp(data, "3IRD", 4) != 0)
    {
        free(data);
        return -1;
    }

    uint8_t version = data[4];
    size_t  pos     = 5;

    /* Game ID: 9 bytes */
    if(pos + 9 > data_len) { free(data); return -1; }
    memcpy(ird->game_id, data + pos, 9);
    ird->game_id[9] = '\0';
    pos += 9;

    /* Game name: .NET BinaryReader length-prefixed string (7-bit encoded length for values < 128) */
    if(pos >= data_len) { free(data); return -1; }
    uint8_t name_len = data[pos++];
    if(name_len > 0 && pos + name_len <= data_len)
    {
        size_t copy = name_len < sizeof(ird->game_name) - 1 ? name_len : sizeof(ird->game_name) - 1;
        memcpy(ird->game_name, data + pos, copy);
        ird->game_name[copy] = '\0';
        pos += name_len;
    }

    /* Fixed-width version strings */
    if(pos + 4 <= data_len) { memcpy(ird->update_ver, data + pos, 4); ird->update_ver[4] = '\0'; pos += 4; }
    if(pos + 5 <= data_len) { memcpy(ird->game_ver, data + pos, 5); ird->game_ver[5] = '\0'; pos += 5; }
    if(pos + 5 <= data_len) { memcpy(ird->app_ver, data + pos, 5); ird->app_ver[5] = '\0'; pos += 5; }

    /* v7: extra ID field */
    if(version == 7) pos += 4;

    /* Header gz: u32 LE length + data */
    if(pos + 4 > data_len) { free(data); ird->valid = 1; return 0; }
    uint32_t hdr_len = le32(data + pos); pos += 4;
    if(pos + hdr_len <= data_len)
    {
        ird->header_gz     = malloc(hdr_len);
        if(ird->header_gz) { memcpy(ird->header_gz, data + pos, hdr_len); ird->header_gz_len = hdr_len; }
        pos += hdr_len;
    }

    /* Footer gz: u32 LE length + data */
    if(pos + 4 > data_len) { free(data); ird->valid = 1; return 0; }
    uint32_t ftr_len = le32(data + pos); pos += 4;
    if(pos + ftr_len <= data_len)
    {
        ird->footer_gz     = malloc(ftr_len);
        if(ird->footer_gz) { memcpy(ird->footer_gz, data + pos, ftr_len); ird->footer_gz_len = ftr_len; }
        pos += ftr_len;
    }

    /* Region count + hashes (skip hashes — we don't need them) */
    if(pos < data_len)
    {
        uint8_t rc = data[pos++];
        pos += rc * 16; /* skip MD5 hashes */
    }

    /* File count + entries (skip) */
    if(pos + 4 <= data_len)
    {
        uint32_t fc = le32(data + pos); pos += 4;
        pos += fc * 24; /* key(8) + md5(16) per entry */
    }

    /* Padding (4 bytes, e.g. 01000000) */
    pos += 4;

    /* v9: PIC(115) then d1(16), d2(16)
     * v<9: d1(16), d2(16) then PIC(115) */
    if(version >= 9)
    {
        if(pos + 115 <= data_len)
        {
            memcpy(ird->pic, data + pos, 115);
            ird->has_pic = 1;
            pos += 115;
        }
    }

    if(pos + 32 <= data_len)
    {
        memcpy(ird->d1, data + pos, 16); pos += 16;
        memcpy(ird->d2, data + pos, 16); pos += 16;
    }

    if(version < 9)
    {
        if(pos + 115 <= data_len)
        {
            memcpy(ird->pic, data + pos, 115);
            ird->has_pic = 1;
            pos += 115;
        }
    }

    ird->valid = 1;
    free(data);
    return 0;
}

void ps3_ird_free(struct ps3_ird_data *ird)
{
    free(ird->header_gz);
    free(ird->footer_gz);
    ird->header_gz = NULL;
    ird->footer_gz = NULL;
}
