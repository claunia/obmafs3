// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : junk_map.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ngcw — Nintendo GameCube/Wii disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Collects junk map entries during import and stores them via ioctl.
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

#include <obmafs3_ioctl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

void ngcw_junk_collector_init(struct ngcw_junk_collector *jc)
{
    memset(jc, 0, sizeof(*jc));
}

void ngcw_junk_collector_add(struct ngcw_junk_collector *jc, uint64_t offset, uint64_t length,
                             uint16_t partition_index, const uint32_t seed[NGC_LFG_SEED_SIZE])
{
    /* Try to merge with the last entry if adjacent, same partition, same seed */
    if(jc->count > 0)
    {
        struct ngcw_junk_entry *last = &jc->entries[jc->count - 1];
        if(last->partition_index == partition_index && last->offset + last->length == offset &&
           memcmp(last->seed, seed, sizeof(last->seed)) == 0)
        {
            last->length += length;
            return;
        }
    }

    /* Grow if needed */
    if(jc->count >= jc->capacity)
    {
        uint32_t new_cap = jc->capacity ? jc->capacity * 2 : 64;
        struct ngcw_junk_entry *nr = realloc(jc->entries, new_cap * sizeof(*nr));
        if(!nr)
        {
            fprintf(stderr, "Error: out of memory for junk map\n");
            return;
        }
        jc->entries  = nr;
        jc->capacity = new_cap;
    }

    struct ngcw_junk_entry *e = &jc->entries[jc->count];
    e->offset          = offset;
    e->length          = length;
    e->partition_index = partition_index;
    memcpy(e->seed, seed, NGC_LFG_SEED_SIZE * sizeof(uint32_t));
    jc->count++;
}

void ngcw_junk_collector_free(struct ngcw_junk_collector *jc)
{
    free(jc->entries);
    memset(jc, 0, sizeof(*jc));
}

int ngcw_junk_collector_store(const struct ngcw_junk_collector *jc, int out_fd)
{
    if(jc->count == 0) return 0;

    /* Add each entry to the B+Tree via ioctl */
    for(uint32_t i = 0; i < jc->count; i++)
    {
        struct obmafs3_ioctl_add_junk_entry_arg arg;
        arg.offset          = jc->entries[i].offset;
        arg.length          = jc->entries[i].length;
        arg.partition_index = jc->entries[i].partition_index;
        memcpy(arg.seed, jc->entries[i].seed, sizeof(arg.seed));

        if(ioctl(out_fd, OBMAFS3_IOC_ADD_JUNK_ENTRY, &arg) < 0)
        {
            fprintf(stderr, "\n  \033[31m✗\033[0m Failed to add junk entry %u: %s\n", i, strerror(errno));
            return -1;
        }

        if((i & 0x7F) == 0)
        {
            int pct = (int)((uint64_t)i * 100 / jc->count);
            int bar = (int)((uint64_t)i * 20 / jc->count);
            printf("\r  \033[90m[\033[0m");
            for(int b = 0; b < 20; b++)
                printf(b < bar ? "\033[32m\xe2\x96\x88\033[0m" : "\033[90m\xe2\x96\x91\033[0m");
            printf("\033[90m]\033[0m %3d%% \033[90m(%u/%u entries)\033[0m", pct, i, jc->count);
            fflush(stdout);
        }
    }

    printf("\r  \033[32m\xe2\x9c\x93\033[0m Stored \033[1m%u\033[0m junk region(s) in B+Tree                      \n",
           jc->count);
    return 0;
}
