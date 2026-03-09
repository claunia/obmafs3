// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Sector-by-sector import with per-region decryption.
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

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_progress(uint64_t current, uint64_t total, const char *label)
{
    if(total == 0) return;
    int pct       = (int)((current * 100) / total);
    int bar_width = 30;
    int filled    = (int)((current * bar_width) / total);
    if(filled > bar_width) filled = bar_width;

    printf("\r  \033[36m%s\033[0m \033[90m[\033[0m", label);
    for(int i = 0; i < bar_width; i++)
    {
        if(i < filled)       printf("\033[32m\xe2\x96\x88\033[0m");
        else if(i == filled) printf("\033[33m\xe2\x96\x93\033[0m");
        else                 printf("\033[90m\xe2\x96\x91\033[0m");
    }
    printf("\033[90m]\033[0m %3d%% ", pct);

    double cur_gb = (double)current / (1024.0 * 1024.0 * 1024.0);
    double tot_gb = (double)total / (1024.0 * 1024.0 * 1024.0);
    if(tot_gb >= 1.0)
        printf("\033[90m(%.1f/%.1f GB)\033[0m", cur_gb, tot_gb);
    else
        printf("\033[90m(%.1f/%.1f MB)\033[0m", cur_gb * 1024.0, tot_gb * 1024.0);

    fflush(stdout);
}

#define IMPORT_BATCH_SECTORS 64

int ps3_import(int iso_fd, int out_fd, const uint8_t disc_key[16], const struct ps3_region_map *map)
{
    struct aes128_ctx aes;
    aes128_init(&aes, disc_key);

    uint64_t total_sectors  = map->total_sectors;
    uint64_t disc_size      = total_sectors * PS3_SECTOR_SIZE;
    uint64_t plain_count    = 0;
    uint64_t enc_count      = 0;

    uint8_t  sector_buf[PS3_SECTOR_SIZE];
    uint8_t  dec_buf[PS3_SECTOR_SIZE];
    uint8_t  batch_buf[PS3_SECTOR_SIZE * IMPORT_BATCH_SECTORS];
    uint32_t batch_used = 0;

    for(uint64_t sec = 0; sec < total_sectors; sec++)
    {
        if((sec & 0xFFF) == 0) print_progress(sec * PS3_SECTOR_SIZE, disc_size, "Importing PS3 disc");

        ssize_t n = pread(iso_fd, sector_buf, PS3_SECTOR_SIZE, (off_t)(sec * PS3_SECTOR_SIZE));
        if(n < PS3_SECTOR_SIZE)
        {
            if(n > 0) memset(sector_buf + n, 0, PS3_SECTOR_SIZE - (size_t)n);
            else      memset(sector_buf, 0, PS3_SECTOR_SIZE);
        }

        const uint8_t *data_to_write;

        if(ps3_is_encrypted_sector(map, sec))
        {
            uint8_t iv[16];
            ps3_sector_iv(sec, iv);
            aes128_cbc_decrypt(&aes, iv, sector_buf, dec_buf, PS3_SECTOR_SIZE);
            data_to_write = dec_buf;
            enc_count++;
        }
        else
        {
            data_to_write = sector_buf;
            plain_count++;
        }

        memcpy(batch_buf + batch_used, data_to_write, PS3_SECTOR_SIZE);
        batch_used += PS3_SECTOR_SIZE;

        if(batch_used >= sizeof(batch_buf))
        {
            ssize_t w = write(out_fd, batch_buf, batch_used);
            if(w < (ssize_t)batch_used)
            {
                fprintf(stderr, "\nError: write failed at sector %" PRIu64 "\n", sec);
                return -1;
            }
            batch_used = 0;
        }
    }

    /* Flush remaining */
    if(batch_used > 0)
    {
        ssize_t w = write(out_fd, batch_buf, batch_used);
        if(w < (ssize_t)batch_used)
        {
            fprintf(stderr, "\nError: write failed at final flush\n");
            return -1;
        }
    }

    print_progress(disc_size, disc_size, "Importing PS3 disc");
    printf("\n  \033[32m\xe2\x9c\x93\033[0m \033[1m%" PRIu64 "\033[0m plaintext, \033[1m%" PRIu64
           "\033[0m encrypted sectors imported\n",
           plain_count, enc_count);
    return 0;
}
