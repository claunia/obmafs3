// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : iso9660.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Minimal ISO 9660 parser to extract files by path.
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ISO_SECTOR_SIZE 2048

static inline uint32_t iso_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Search a directory extent for a named entry.
 * Returns: 0 on success (extent_lba and size filled), -1 if not found.
 */
static int find_entry(int fd, uint32_t dir_lba, uint32_t dir_size, const char *name, size_t name_len,
                      uint32_t *extent_lba, uint32_t *size, int *is_dir)
{
    uint8_t *dir_data = malloc(dir_size);
    if(!dir_data) return -1;

    if(pread(fd, dir_data, dir_size, (off_t)dir_lba * ISO_SECTOR_SIZE) < (ssize_t)dir_size)
    {
        free(dir_data);
        return -1;
    }

    uint32_t pos = 0;
    while(pos < dir_size)
    {
        uint8_t rec_len = dir_data[pos];
        if(rec_len == 0)
        {
            /* Skip to next sector boundary */
            pos = ((pos / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }
        if(pos + rec_len > dir_size) break;

        uint8_t  flags    = dir_data[pos + 25];
        uint8_t  id_len   = dir_data[pos + 32];
        const uint8_t *id = dir_data + pos + 33;

        /* Skip . and .. entries */
        if(id_len == 1 && (id[0] == 0 || id[0] == 1)) { pos += rec_len; continue; }

        /* ISO 9660 appends ";1" version suffix — strip it for comparison */
        size_t cmp_len = id_len;
        if(cmp_len >= 2 && id[cmp_len - 2] == ';') cmp_len -= 2;

        if(cmp_len == name_len && strncasecmp((const char *)id, name, cmp_len) == 0)
        {
            *extent_lba = iso_le32(dir_data + pos + 2);
            *size       = iso_le32(dir_data + pos + 10);
            *is_dir     = (flags & 0x02) ? 1 : 0;
            free(dir_data);
            return 0;
        }

        pos += rec_len;
    }

    free(dir_data);
    return -1;
}

int ps3_iso9660_read_file(int fd, const char *path, uint8_t **data, size_t *size)
{
    /* Read PVD at sector 16 */
    uint8_t pvd[ISO_SECTOR_SIZE];
    if(pread(fd, pvd, ISO_SECTOR_SIZE, 16 * ISO_SECTOR_SIZE) < ISO_SECTOR_SIZE) return -1;

    /* Verify PVD: type=1, id="CD001" */
    if(pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;

    /* Root directory record at offset 156 (34 bytes) */
    uint32_t root_lba  = iso_le32(pvd + 156 + 2);
    uint32_t root_size = iso_le32(pvd + 156 + 10);

    /* Walk the path components */
    uint32_t cur_lba  = root_lba;
    uint32_t cur_size = root_size;

    /* Make a mutable copy of the path */
    char path_buf[512];
    strncpy(path_buf, path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    /* Strip leading slash */
    char *p = path_buf;
    if(*p == '/') p++;

    char *saveptr;
    char *component = strtok_r(p, "/", &saveptr);
    while(component)
    {
        char *next = strtok_r(NULL, "/", &saveptr);

        uint32_t entry_lba, entry_size;
        int      entry_is_dir;
        if(find_entry(fd, cur_lba, cur_size, component, strlen(component),
                      &entry_lba, &entry_size, &entry_is_dir) != 0)
            return -1;

        if(next != NULL)
        {
            /* Intermediate component must be a directory */
            if(!entry_is_dir) return -1;
            cur_lba  = entry_lba;
            cur_size = entry_size;
        }
        else
        {
            /* Final component — read the file */
            if(entry_is_dir) return -1;
            if(entry_size > 64 * 1024 * 1024) return -1; /* sanity: 64 MiB max */

            uint8_t *file_data = malloc(entry_size);
            if(!file_data) return -1;

            if(pread(fd, file_data, entry_size, (off_t)entry_lba * ISO_SECTOR_SIZE) < (ssize_t)entry_size)
            {
                free(file_data);
                return -1;
            }

            *data = file_data;
            *size = entry_size;
            return 0;
        }

        component = next;
    }

    return -1;
}
