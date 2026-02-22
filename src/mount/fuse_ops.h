// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fuse_ops.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : FUSE interface for OBMAFS3
//
// --[ Description ] ----------------------------------------------------------
//
//     FUSE operation declarations for OBMAFS3.
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

#ifndef OBMAFS3_FUSE_OPS_H
#define OBMAFS3_FUSE_OPS_H

#define FUSE_USE_VERSION 35

#include <fuse3/fuse.h>
#include "obmafs.h"

#define OBMAFS3_MAX_DISK_IMAGE_MAPS 32

/** Maps a file extension to a sector size for disk image handling */
struct disk_image_mapping
{
    char     extension[32];  ///< File extension (without dot), e.g. "dsk"
    uint16_t sector_size;    ///< Sector size in bytes, e.g. 512
};

extern struct fuse_operations obmafs3_fuse_ops;
extern struct obmafs3_ctx    *g_ctx;

/** Disk image extension-to-sector-size mappings */
extern struct disk_image_mapping g_disk_image_maps[OBMAFS3_MAX_DISK_IMAGE_MAPS];
extern int                       g_disk_image_map_count;

/**
 * Parse a semicolon-separated disk_images specification string.
 * Format: "ext1=size1;ext2=size2;..."
 * Returns 0 on success, -1 on parse error.
 */
int parse_disk_image_maps(const char *spec);

#endif /* OBMAFS3_FUSE_OPS_H */
