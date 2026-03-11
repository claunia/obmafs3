// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : obmafs3_ioctl.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 FUSE ioctl command definitions.
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

/*
 * Shared between the FUSE mount daemon and any user-space tool that
 * communicates with a mounted OBMAFS3 filesystem via ioctl().
 *
 * This header is intentionally self-contained: it does not include
 * the full internal headers (defs.h, btree.h) so that external tools
 * can use it without pulling in the obmafs library.
 */
#ifndef OBMAFS3_IOCTL_H
#define OBMAFS3_IOCTL_H

#include <stdint.h>
#include <sys/ioctl.h>

/* ---------- Size constants (duplicated with #ifndef guards so this
 *            header works both standalone and when full internal
 *            headers have already been included) ---------- */

#ifndef CD_RAW_SECTOR_SIZE
#define CD_RAW_SECTOR_SIZE 2352
#endif

#ifndef CD_RAW_PLUS_SUB
#define CD_RAW_PLUS_SUB 2448
#endif

#ifndef METADATA_KEY_MAX
#define METADATA_KEY_MAX 256 /* 255 chars + NUL */
#endif

#ifndef METADATA_VALUE_MAX
#define METADATA_VALUE_MAX 1025 /* 1024 chars + NUL */
#endif

/* ================================================================== */
/*  Media tag ioctls                                                   */
/* ================================================================== */

#define OBMAFS3_IOC_MAX_TAG_DATA 16368

struct obmafs3_ioctl_tag_arg
{
    uint16_t tag_type;
    uint32_t data_length;
    uint8_t  data[OBMAFS3_IOC_MAX_TAG_DATA];
};

#define OBMAFS3_IOC_SET_MEDIA_TAG _IOW('O', 1, struct obmafs3_ioctl_tag_arg)
#define OBMAFS3_IOC_GET_MEDIA_TAG _IOWR('O', 2, struct obmafs3_ioctl_tag_arg)

/* ================================================================== */
/*  Compact disc image ioctls                                          */
/* ================================================================== */

/** Convert an empty regular file to a CompactDiscImage. */
#define OBMAFS3_IOC_SET_CD_IMAGE _IO('O', 3)

struct obmafs3_ioctl_cd_write_arg
{
    int64_t  sector;      /**< Sector LBA to write */
    uint32_t buffer_size; /**< 2352 or 2448 */
    uint8_t  sector_mode; /**< enum obmafs3_cd_sector_mode */
    uint8_t  buffer[CD_RAW_PLUS_SUB];
};

#define OBMAFS3_IOC_CD_WRITE_LONG _IOW('O', 4, struct obmafs3_ioctl_cd_write_arg)

struct obmafs3_ioctl_cd_read_arg
{
    int64_t sector;                     /**< Sector LBA to read */
    uint8_t buffer[CD_RAW_SECTOR_SIZE]; /**< Output: reconstructed 2352-byte sector */
};

#define OBMAFS3_IOC_CD_READ_LONG _IOWR('O', 5, struct obmafs3_ioctl_cd_read_arg)

struct obmafs3_ioctl_cd_read_full_arg
{
    int64_t sector;                  /**< Sector LBA to read */
    uint8_t buffer[CD_RAW_PLUS_SUB]; /**< Output: 2352 raw + 96 subchannel */
};

#define OBMAFS3_IOC_CD_READ_LONG_SUB _IOWR('O', 6, struct obmafs3_ioctl_cd_read_full_arg)

/* ================================================================== */
/*  Image metadata ioctls                                              */
/* ================================================================== */

struct obmafs3_ioctl_metadata_set_arg
{
    char key[METADATA_KEY_MAX];     /**< Metadata key (NUL-terminated) */
    char value[METADATA_VALUE_MAX]; /**< Metadata value (NUL-terminated) */
};

#define OBMAFS3_IOC_SET_METADATA _IOW('O', 7, struct obmafs3_ioctl_metadata_set_arg)

struct obmafs3_ioctl_metadata_get_arg
{
    char key[METADATA_KEY_MAX];     /**< Input: key to look up */
    char value[METADATA_VALUE_MAX]; /**< Output: value */
};

#define OBMAFS3_IOC_GET_METADATA _IOWR('O', 8, struct obmafs3_ioctl_metadata_get_arg)

struct obmafs3_ioctl_metadata_delete_arg
{
    char key[METADATA_KEY_MAX]; /**< Key to delete */
};

#define OBMAFS3_IOC_DELETE_METADATA _IOW('O', 9, struct obmafs3_ioctl_metadata_delete_arg)

#define METADATA_QUERY_PATH_MAX    1024
#define METADATA_QUERY_MAX_RESULTS 8

/**
 * List metadata keys for an image.  Paginated: set offset to 0 for the
 * first page, then advance by count for subsequent pages.  Returns
 * count == 0 when no more keys remain.
 */
struct obmafs3_ioctl_metadata_list_arg
{
    uint32_t offset;                     /**< Input: starting offset */
    uint32_t count;                      /**< Output: keys returned */
    char     keys[16][METADATA_KEY_MAX]; /**< Output: up to 16 keys */
};

#define OBMAFS3_IOC_LIST_METADATA _IOWR('O', 10, struct obmafs3_ioctl_metadata_list_arg)

/* ================================================================== */
/*  Metadata query (multi-filter)                                      */
/* ================================================================== */

/*
 * Query operator and combine enums are defined in enums.h when the
 * full internal headers are available.  For standalone use of this
 * ioctl header, we define them here with include guards.
 */

#ifndef OBMAFS3_ENUMS_H
enum obmafs3_query_op
{
    kQueryOpEqual      = 0,
    kQueryOpNotEqual   = 1,
    kQueryOpGreater    = 2,
    kQueryOpLess       = 3,
    kQueryOpGreaterEq  = 4,
    kQueryOpLessEq     = 5,
    kQueryOpContains   = 6,
    kQueryOpStartsWith = 7,
    kQueryOpExists     = 8
};

enum obmafs3_query_combine
{
    kQueryCombineAnd = 0,
    kQueryCombineOr  = 1
};
#endif /* OBMAFS3_ENUMS_H */

#ifndef OBMAFS3_QUERY_MAX_FILTERS
#define OBMAFS3_QUERY_MAX_FILTERS 4
#endif

/** A single ioctl filter condition: key <op> value. */
struct obmafs3_ioctl_query_filter
{
    char    key[METADATA_KEY_MAX];     /**< Metadata key to match */
    char    value[METADATA_VALUE_MAX]; /**< Value operand (ignored for kQueryOpExists) */
    uint8_t op;                        /**< enum obmafs3_query_op */
    uint8_t _pad[7];                   /**< Alignment padding */
};

/**
 * Query which images match one or more metadata filter conditions.
 * Returns a paginated list of paths.
 *
 * Set filter_count to the number of active filters (1..4).
 * Set combine to kQueryCombineAnd or kQueryCombineOr.
 * Set offset to 0 for the first page, advance by count for subsequent
 * pages.  Returns count == 0 when no more results remain.
 */
struct obmafs3_ioctl_metadata_query_arg
{
    uint8_t                           combine;                            /**< enum obmafs3_query_combine */
    uint8_t                           filter_count;                       /**< 1..OBMAFS3_QUERY_MAX_FILTERS */
    uint8_t                           _pad[6];                            /**< Alignment padding */
    struct obmafs3_ioctl_query_filter filters[OBMAFS3_QUERY_MAX_FILTERS]; /**< Filter conditions */
    uint32_t                          offset;                             /**< Input: starting offset */
    uint32_t                          count;                              /**< Output: paths returned this page */
    uint32_t                          total;                              /**< Output: total matching results */
    uint32_t                          _pad2;                              /**< Alignment padding */
    char paths[METADATA_QUERY_MAX_RESULTS][METADATA_QUERY_PATH_MAX];      /**< Output: up to 8 paths */
};

#define OBMAFS3_IOC_QUERY_METADATA _IOWR('O', 11, struct obmafs3_ioctl_metadata_query_arg)

/* ================================================================== */
/*  Media image ioctl                                                  */
/* ================================================================== */

/**
 * Convert an empty regular file to a MediaImage with a given sector
 * size.  Subsequent write() calls will be routed through the dedup
 * media-image data path.
 */
struct obmafs3_ioctl_set_media_image_arg
{
    uint16_t sector_size; /**< Sector size in bytes (e.g. 512, 2048) */
};

#define OBMAFS3_IOC_SET_MEDIA_IMAGE _IOW('O', 12, struct obmafs3_ioctl_set_media_image_arg)

/* ================================================================== */
/*  Sector tag ioctls                                                  */
/* ================================================================== */

#ifndef SECTOR_TAG_DATA_MAX
#define SECTOR_TAG_DATA_MAX 64
#endif

/** Write a per-sector tag to a media image file. */
struct obmafs3_ioctl_sector_tag_write_arg
{
    int64_t  sector;                       /**< Logical sector number */
    uint16_t tag_type;                     /**< SectorTagType enum value */
    uint16_t data_length;                  /**< Actual bytes of tag data */
    uint8_t  data[SECTOR_TAG_DATA_MAX];    /**< Tag data */
};

#define OBMAFS3_IOC_SET_SECTOR_TAG _IOW('O', 13, struct obmafs3_ioctl_sector_tag_write_arg)

/** Read a per-sector tag from a media image file. */
struct obmafs3_ioctl_sector_tag_read_arg
{
    int64_t  sector;                       /**< Input: logical sector number */
    uint16_t tag_type;                     /**< Input: SectorTagType enum value */
    uint16_t data_length;                  /**< Output: actual bytes of tag data */
    uint8_t  data[SECTOR_TAG_DATA_MAX];    /**< Output: tag data */
};

#define OBMAFS3_IOC_GET_SECTOR_TAG _IOWR('O', 14, struct obmafs3_ioctl_sector_tag_read_arg)

/* ================================================================== */
/*  Nintendo disc image ioctl                                          */
/* ================================================================== */

/**
 * Maximum number of Wii partitions we support per disc.
 * Real discs have 1-4 partitions (game + update + channel).
 */
#define OBMAFS3_NGC_MAX_PARTITIONS 8

/**
 * Per-partition descriptor passed during SET_NINTENDO_IMAGE.
 * For Wii: contains the AES title key (already decrypted from the ticket).
 * For GameCube: unused (no partitions).
 */
struct obmafs3_ioctl_ngc_partition_arg
{
    uint64_t data_offset;                      /**< Byte offset of partition data on disc */
    uint64_t data_size;                        /**< Size of partition data in bytes */
    uint8_t  title_key[16];                    /**< Decrypted AES-128 title key */
};

/**
 * Convert an empty regular file to a Nintendo disc image.
 * Sets the file type to kFileTypeNintendo and initialises the
 * Nintendo sector map.
 *
 * disc_type: 0 = GameCube, 1 = Wii
 */
struct obmafs3_ioctl_set_nintendo_image_arg
{
    uint8_t  disc_type;                        /**< 0 = GameCube, 1 = Wii */
    uint64_t disc_size;                        /**< Total disc size in bytes */
    uint16_t partition_count;                  /**< Number of partitions (0 for GC) */
    struct obmafs3_ioctl_ngc_partition_arg partitions[OBMAFS3_NGC_MAX_PARTITIONS];
};

#define OBMAFS3_IOC_SET_NINTENDO_IMAGE _IOW('O', 15, struct obmafs3_ioctl_set_nintendo_image_arg)

/** Add one junk map entry for a Nintendo disc image. */
struct obmafs3_ioctl_add_junk_entry_arg
{
    uint64_t offset;           /**< Disc offset (GC) or logical offset in partition (Wii) */
    uint64_t length;           /**< Junk region length in bytes */
    uint16_t partition_index;  /**< Partition index (0xFFFF = GC / outside partitions) */
    uint32_t seed[17];         /**< LFG seed (17 × uint32, big-endian) */
};

#define OBMAFS3_IOC_ADD_JUNK_ENTRY _IOW('O', 16, struct obmafs3_ioctl_add_junk_entry_arg)

/* ================================================================== */
/*  PS3 disc image ioctl                                               */
/* ================================================================== */

#define OBMAFS3_PS3_MAX_REGIONS 32

struct obmafs3_ioctl_ps3_region
{
    uint32_t start_sector;  /**< First sector of unencrypted region */
    uint32_t end_sector;    /**< Last sector of unencrypted region (inclusive) */
};

struct obmafs3_ioctl_set_ps3_image_arg
{
    uint64_t disc_size;     /**< Total disc size in bytes */
    uint8_t  disc_key[16];  /**< Derived AES-128 disc key */
    uint16_t region_count;  /**< Number of unencrypted regions */
    struct obmafs3_ioctl_ps3_region regions[OBMAFS3_PS3_MAX_REGIONS];
};

#define OBMAFS3_IOC_SET_PS3_IMAGE _IOW('O', 17, struct obmafs3_ioctl_set_ps3_image_arg)

#endif /* OBMAFS3_IOCTL_H */
