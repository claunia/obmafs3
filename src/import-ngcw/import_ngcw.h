// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import_ngcw.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ngcw — Nintendo GameCube/Wii disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Internal header for the NGC/Wii import tool.
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

#ifndef OBMAFS3_IMPORT_NGCW_H
#define OBMAFS3_IMPORT_NGCW_H

#include <defs.h>

#include <stdint.h>
#include <stdio.h>

/* ---- In-memory junk map collector ---- */
struct ngcw_junk_entry
{
    uint64_t offset;
    uint64_t length;
    uint16_t partition_index;
    uint32_t seed[NGC_LFG_SEED_SIZE];
};

struct ngcw_junk_collector
{
    struct ngcw_junk_entry *entries;
    uint32_t                count;
    uint32_t                capacity;
};

void ngcw_junk_collector_init(struct ngcw_junk_collector *jc);
void ngcw_junk_collector_add(struct ngcw_junk_collector *jc, uint64_t offset, uint64_t length,
                             uint16_t partition_index, const uint32_t seed[NGC_LFG_SEED_SIZE]);
void ngcw_junk_collector_free(struct ngcw_junk_collector *jc);
int  ngcw_junk_collector_store(const struct ngcw_junk_collector *jc, int out_fd);

/* ---- disc.c ---- */
int  ngcw_open_iso(const char *path, int *fd, uint8_t *header, uint64_t *disc_size);
void ngcw_print_disc_info(const uint8_t *header, int disc_type, uint64_t disc_size);

/* ---- partition.c ---- */
int ngcw_read_partitions(int iso_fd, uint16_t *part_count, struct ngc_partition **parts);

/* ---- import.c ---- */
int ngcw_import_gc(int iso_fd, int out_fd, const uint8_t *header, uint64_t disc_size,
                   struct ngcw_junk_collector *jc);
int ngcw_import_wii(int iso_fd, int out_fd, const uint8_t *header, uint64_t disc_size,
                    uint16_t part_count, struct ngc_partition *parts,
                    struct ngcw_junk_collector *jc);

/* ---- metadata.c ---- */
void ngcw_import_metadata(int out_fd, const uint8_t *header, int disc_type);

#endif /* OBMAFS3_IMPORT_NGCW_H */
