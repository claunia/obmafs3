// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : sfo.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     PARAM.SFO binary parser.
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

#include <stdio.h>
#include <string.h>

static inline uint16_t sfo_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t sfo_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ps3_parse_sfo(const uint8_t *data, size_t size, struct ps3_sfo_data *sfo)
{
    memset(sfo, 0, sizeof(*sfo));
    if(size < 20) return -1;

    /* Header: magic "\0PSF", version, key_table_offset, data_table_offset, entry_count */
    if(data[0] != 0 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F') return -1;

    uint32_t key_table  = sfo_le32(data + 8);
    uint32_t data_table = sfo_le32(data + 12);
    uint32_t entries    = sfo_le32(data + 16);

    if(entries > 32) entries = 32;

    for(uint32_t i = 0; i < entries; i++)
    {
        uint32_t idx_off = 20 + i * 16;
        if(idx_off + 16 > size) break;

        uint16_t key_off        = sfo_le16(data + idx_off);
        uint16_t data_format    = sfo_le16(data + idx_off + 2);
        uint32_t data_used_size = sfo_le32(data + idx_off + 4);
        /* uint32_t data_max_size  = sfo_le32(data + idx_off + 8); */
        uint32_t data_off       = sfo_le32(data + idx_off + 12);

        /* Read key */
        uint32_t abs_key = key_table + key_off;
        if(abs_key >= size) continue;
        const char *key = (const char *)(data + abs_key);
        size_t klen = strnlen(key, size - abs_key);
        if(klen >= sizeof(sfo->entries[0].key)) klen = sizeof(sfo->entries[0].key) - 1;

        /* Read value */
        uint32_t abs_data = data_table + data_off;
        if(abs_data >= size) continue;

        struct ps3_sfo_entry *e = &sfo->entries[sfo->count];
        memcpy(e->key, key, klen);
        e->key[klen] = '\0';

        if(data_format == 0x0204 || data_format == 0x0004)
        {
            /* UTF-8 string */
            size_t vlen = data_used_size;
            if(vlen > 0 && data[abs_data + vlen - 1] == 0) vlen--;
            if(vlen >= sizeof(e->value)) vlen = sizeof(e->value) - 1;
            memcpy(e->value, data + abs_data, vlen);
            e->value[vlen] = '\0';
        }
        else if(data_format == 0x0404 && data_used_size == 4)
        {
            /* uint32 LE */
            uint32_t val = sfo_le32(data + abs_data);
            snprintf(e->value, sizeof(e->value), "%u", val);
        }
        else
        {
            continue; /* skip unknown formats */
        }

        sfo->count++;
    }

    return 0;
}

const char *ps3_sfo_get(const struct ps3_sfo_data *sfo, const char *key)
{
    for(int i = 0; i < sfo->count; i++)
    {
        if(strcmp(sfo->entries[i].key, key) == 0) return sfo->entries[i].value;
    }
    return NULL;
}
