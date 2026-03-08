// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : partition.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ngcw — Nintendo GameCube/Wii disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Wii partition table parsing and title key decryption.
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

#include "../../src/lib/nintendo.h"
#include "import_ngcw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int ngcw_read_partitions(int iso_fd, uint16_t *part_count, struct ngc_partition **parts)
{
    /* Read the 4 partition table info entries at 0x40000 */
    uint8_t ptable_raw[32];
    if(pread(iso_fd, ptable_raw, 32, 0x40000) < 32)
    {
        fprintf(stderr, "Error: cannot read partition table info\n");
        return -1;
    }

    /* Count total partitions across all 4 tables */
    uint16_t total = 0;
    struct ngc_wii_part_info infos[4];
    for(int t = 0; t < 4; t++)
    {
        infos[t].count  = ngc_be32(ptable_raw + t * 8);
        infos[t].offset = ngc_be32(ptable_raw + t * 8 + 4);
        total += (uint16_t)infos[t].count;
    }

    if(total == 0)
    {
        *part_count = 0;
        *parts      = NULL;
        return 0;
    }

    *parts = calloc(total, sizeof(struct ngc_partition));
    if(!*parts) return -1;

    uint16_t idx = 0;
    for(int t = 0; t < 4; t++)
    {
        if(infos[t].count == 0) continue;

        uint64_t table_offset = (uint64_t)infos[t].offset << 2;
        size_t   table_size   = infos[t].count * 8; /* 8 bytes per entry */
        uint8_t *table_data   = malloc(table_size);
        if(!table_data) { free(*parts); *parts = NULL; return -1; }

        if(pread(iso_fd, table_data, table_size, (off_t)table_offset) < (ssize_t)table_size)
        {
            fprintf(stderr, "Error: cannot read partition table %d\n", t);
            free(table_data);
            free(*parts);
            *parts = NULL;
            return -1;
        }

        for(uint32_t p = 0; p < infos[t].count && idx < total; p++)
        {
            struct ngc_wii_part_entry *pe = (struct ngc_wii_part_entry *)(table_data + p * 8);
            uint64_t part_offset = (uint64_t)ngc_be32((uint8_t *)&pe->offset) << 2;

            (*parts)[idx].offset = part_offset;
            (*parts)[idx].type   = ngc_be32((uint8_t *)&pe->type);

            /* Read the ticket at the partition offset */
            struct ngc_wii_ticket ticket;
            if(pread(iso_fd, &ticket, sizeof(ticket), (off_t)part_offset) < (ssize_t)sizeof(ticket))
            {
                fprintf(stderr, "Error: cannot read ticket for partition %u\n", idx);
                free(table_data);
                free(*parts);
                *parts = NULL;
                return -1;
            }

            /* Decrypt the title key */
            ngc_decrypt_title_key(&ticket, (*parts)[idx].title_key);

            /* Read partition header to get data offset and size
             * Offset 0x2B8 from partition start: data offset (>> 2)
             * Offset 0x2BC from partition start: data size (>> 2) */
            uint8_t phdr[8];
            if(pread(iso_fd, phdr, 8, (off_t)(part_offset + 0x2B8)) < 8)
            {
                fprintf(stderr, "Error: cannot read partition header for partition %u\n", idx);
                free(table_data);
                free(*parts);
                *parts = NULL;
                return -1;
            }

            (*parts)[idx].data_offset = part_offset + ((uint64_t)ngc_be32(phdr) << 2);
            (*parts)[idx].data_size   = (uint64_t)ngc_be32(phdr + 4) << 2;

            idx++;
        }

        free(table_data);
    }

    *part_count = idx;
    return 0;
}
