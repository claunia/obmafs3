// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : disc.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-wiiu — Nintendo Wii U disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Disc reader (WUD/WUX) and identification.
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

#include "import_wiiu.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- WUX support ---- */

static int wux_init(struct wiiu_reader *reader)
{
    struct wux_header hdr;
    if(pread(reader->fd, &hdr, sizeof(hdr), 0) < (ssize_t)sizeof(hdr)) return -1;

    if(hdr.magic != WUX_MAGIC) return -1;

    reader->is_wux    = 1;
    reader->disc_size = hdr.uncompressed_size;

    uint32_t sector_size = hdr.sector_size;
    if(sector_size == 0 || sector_size != WIIU_SECTOR_SIZE)
    {
        fprintf(stderr, "Error: WUX sector size 0x%X is not 0x%X\n", sector_size, WIIU_SECTOR_SIZE);
        return -1;
    }

    reader->wux_sector_count = (uint32_t)(reader->disc_size / sector_size);

    /* Read index table (starts at offset 0x20) */
    size_t idx_bytes = (size_t)reader->wux_sector_count * sizeof(uint32_t);
    reader->wux_index = malloc(idx_bytes);
    if(!reader->wux_index)
    {
        fprintf(stderr, "Error: cannot allocate WUX index (%zu bytes)\n", idx_bytes);
        return -1;
    }

    if(pread(reader->fd, reader->wux_index, idx_bytes, 0x20) < (ssize_t)idx_bytes)
    {
        fprintf(stderr, "Error: cannot read WUX index table\n");
        free(reader->wux_index);
        reader->wux_index = NULL;
        return -1;
    }

    /* Data starts at next sector-aligned offset after header + index table */
    uint64_t raw_data_off = 0x20 + idx_bytes;
    reader->wux_data_offset = (raw_data_off + WIIU_SECTOR_SIZE - 1) & ~((uint64_t)WIIU_SECTOR_SIZE - 1);

    return 0;
}

int wiiu_reader_open(const char *path, struct wiiu_reader *reader)
{
    memset(reader, 0, sizeof(*reader));

    reader->fd = open(path, O_RDONLY);
    if(reader->fd < 0)
    {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Try WUX first */
    if(wux_init(reader) == 0)
        return 0;

    /* Fallback: raw WUD */
    reader->is_wux = 0;
    struct stat st;
    if(fstat(reader->fd, &st) < 0)
    {
        fprintf(stderr, "Error: cannot stat %s: %s\n", path, strerror(errno));
        close(reader->fd);
        return -1;
    }
    reader->disc_size = (uint64_t)st.st_size;

    return 0;
}

void wiiu_reader_close(struct wiiu_reader *reader)
{
    if(reader->wux_index) { free(reader->wux_index); reader->wux_index = NULL; }
    if(reader->fd >= 0)   { close(reader->fd); reader->fd = -1; }
}

ssize_t wiiu_reader_pread(struct wiiu_reader *reader, void *buf, size_t count, uint64_t offset)
{
    if(!reader->is_wux)
        return pread(reader->fd, buf, count, (off_t)offset);

    /* WUX: translate via index table */
    uint8_t *out  = (uint8_t *)buf;
    size_t   done = 0;

    while(done < count)
    {
        uint64_t cur_off = offset + done;
        if(cur_off >= reader->disc_size) break;

        uint32_t logical_sector = (uint32_t)(cur_off / WIIU_SECTOR_SIZE);
        uint32_t sector_offset  = (uint32_t)(cur_off % WIIU_SECTOR_SIZE);

        if(logical_sector >= reader->wux_sector_count) break;

        uint32_t physical_sector = reader->wux_index[logical_sector];
        uint64_t file_offset     = reader->wux_data_offset + (uint64_t)physical_sector * WIIU_SECTOR_SIZE + sector_offset;

        size_t chunk = WIIU_SECTOR_SIZE - sector_offset;
        if(chunk > count - done) chunk = count - done;

        ssize_t n = pread(reader->fd, out + done, chunk, (off_t)file_offset);
        if(n <= 0) break;

        done += (size_t)n;
    }

    return done > 0 ? (ssize_t)done : -1;
}

void wiiu_print_disc_info(const uint8_t *header, uint64_t disc_size)
{
    /* Wii U header: product code as ASCII string at offset 0 */
    char product_code[23];
    memcpy(product_code, header, 22);
    product_code[22] = '\0';

    /* Trim trailing zeroes/spaces */
    for(int i = 21; i >= 0 && (product_code[i] == '\0' || product_code[i] == ' '); i--)
        product_code[i] = '\0';

    printf("\n  \xF0\x9F\x8E\xAE \033[1;36mNintendo Wii U\033[0m disc\n");
    printf("  \033[90m\xe2\x94\x9c\xe2\x94\x80\033[0m Product:    \033[1;33m%s\033[0m\n", product_code);
    printf("  \033[90m\xe2\x94\x94\xe2\x94\x80\033[0m Size:       \033[32m%.2f GB\033[0m (%" PRIu64 " bytes)\n\n",
           disc_size / (1024.0 * 1024.0 * 1024.0), disc_size);
}
