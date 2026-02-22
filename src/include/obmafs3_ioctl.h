/*
 * obmafs3_ioctl.h - OBMAFS3 FUSE ioctl command definitions
 *
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

/**
 * Query which images have a given key=value pair.
 * Returns a paginated list of paths.
 */
struct obmafs3_ioctl_metadata_query_arg
{
    char     key[METADATA_KEY_MAX];                                      /**< Input: key */
    char     value[METADATA_VALUE_MAX];                                  /**< Input: value */
    uint32_t offset;                                                     /**< Input: starting offset */
    uint32_t count;                                                      /**< Output: paths returned */
    char     paths[METADATA_QUERY_MAX_RESULTS][METADATA_QUERY_PATH_MAX]; /**< Output: up to 8 paths */
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

#endif /* OBMAFS3_IOCTL_H */
