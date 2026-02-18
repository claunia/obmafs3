/*
 * fuse_ops.h - OBMAFS3 FUSE operation declarations
 */
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
