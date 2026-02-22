/*
 * import-aif - Import an Aaru Image Format (.aif) file into a mounted
 *              OBMAFS3 filesystem via standard POSIX I/O and ioctls.
 */

#include "enums.h"
#include "obmafs3_ioctl.h"
#include "tags.h"

/* Prevent type collision: obmafs3 and libaaruformat both define MediaTagType and CdEccContext */
#define MediaTagType AarufMediaTagType
#define CdEccContext AarufCdEccContext
#include <aaruformat.h>
#undef MediaTagType
#undef CdEccContext

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <iconv.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * Map libaaruformat DataType values to OBMAFS3 MediaTagType.
 *
 * The libaaruformat DataType enum starts at NoData=0, UserData=1,
 * CompactDiscPartialToc=2, ..., while OBMAFS3 tags.h starts at
 * kCdTableOfContents=0.  The media-tag values are offset by 2.
 *
 * @param aaruf_tag  libaaruformat DataType value.
 * @return OBMAFS3 MediaTagType, or -1 if not a media tag.
 */
static int aaruf_tag_to_obmafs(int aaruf_tag)
{
    /* NoData=0, UserData=1 are not media tags */
    if(aaruf_tag < 2) return -1;
    int obmafs_tag = aaruf_tag - 2;
    if(obmafs_tag > kMaxMediaTag) return -1;
    return obmafs_tag;
}

/**
 * Map libaaruformat TrackType to OBMAFS3 cd_sector_mode.
 *
 * @param tt  libaaruformat TrackType value.
 * @return OBMAFS3 cd_sector_mode, or -1 if unknown.
 */
static int aaruf_track_type_to_cd_mode(int tt)
{
    switch(tt)
    {
        case 0: /* Audio */
            return kCdSectorModeAudio;
        case 1: /* CdMode1 */
        case 5: /* Data */
            return kCdSectorMode1;
        case 2: /* CdMode2Formless */
            return kCdSectorMode2;
        case 3: /* CdMode2Form1 */
            return kCdSectorMode2Form1;
        case 4: /* CdMode2Form2 */
            return kCdSectorMode2Form2;
        default:
            return -1;
    }
}

/**
 * Return the user-data sector size for a given CD sector mode.
 */
static uint16_t cd_mode_sector_size(int mode)
{
    switch(mode)
    {
        case kCdSectorModeAudio:  return CD_RAW_SECTOR_SIZE; /* 2352 */
        case kCdSectorMode1:      return 2048;               /* CD_DATA_SIZE */
        case kCdSectorMode2:      return 2336;
        case kCdSectorMode2Form1: return 2048;               /* CD_DATA_SIZE */
        case kCdSectorMode2Form2: return 2328;
        default:                  return 0;
    }
}

/**
 * Check if the image is an optical disc.
 *
 * libaaruformat's ImageInfo.MetadataMediaType == 0 indicates optical media.
 */
static int is_optical_media(const ImageInfo *info)
{
    return info->MetadataMediaType == 0;
}

/**
 * Print usage information.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <aif-file> <output-path>\n"
            "\n"
            "Import an Aaru Image Format (.aif) file into a mounted OBMAFS3 filesystem.\n"
            "\n"
            "Arguments:\n"
            "  <aif-file>     Path to the source .aif file\n"
            "  <output-path>  Full path for the image within the mounted filesystem\n"
            "                 (e.g. /mnt/obmafs/images/myimage)\n"
            "\n"
            "Options:\n"
            "  -h, --help     Show this help\n",
            prog);
}

/**
 * Create all intermediate directories in a path.
 * Similar to 'mkdir -p'.  Only the directory portion of the path is
 * created; the final component is assumed to be the filename.
 *
 * @param path  Full path including the filename.
 * @return 0 on success, -1 on error (errno is set).
 */
static int mkdirs(const char *path)
{
    char *buf = strdup(path);
    if(!buf) return -1;

    /* Walk the path creating each directory component */
    for(char *p = buf + 1; *p; p++)
    {
        if(*p == '/')
        {
            *p = '\0';
            if(mkdir(buf, 0755) != 0 && errno != EEXIST)
            {
                int saved = errno;
                free(buf);
                errno = saved;
                return -1;
            }
            *p = '/';
        }
    }

    free(buf);
    return 0;
}

/**
 * Import media tags from an AIF file via ioctl.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 * @return 0 on success, negative on error.
 */
static int import_media_tags(void *aaruf_ctx, int fd)
{
    uint8_t tag_buf[4096];
    size_t  tag_buf_len = sizeof(tag_buf);
    int     count       = aaruf_get_readable_media_tags(aaruf_ctx, tag_buf, &tag_buf_len);

    if(count <= 0) return 0;

    /* The buffer contains 'count' uint32_t DataType values */
    for(int i = 0; i < count; i++)
    {
        uint32_t dt;
        memcpy(&dt, tag_buf + i * sizeof(uint32_t), sizeof(uint32_t));

        int obmafs_tag = aaruf_tag_to_obmafs((int)dt);
        if(obmafs_tag < 0) continue;

        /* Read the media tag data */
        uint32_t data_len = 0;
        uint8_t  probe    = 0;

        /* First call to get size */
        aaruf_read_media_tag(aaruf_ctx, &probe, dt, &data_len);
        if(data_len == 0) continue;

        if(data_len > OBMAFS3_IOC_MAX_TAG_DATA)
        {
            fprintf(stderr, "Warning: media tag %d too large (%u bytes), skipping\n", obmafs_tag, data_len);
            continue;
        }

        struct obmafs3_ioctl_tag_arg tag_arg;
        memset(&tag_arg, 0, sizeof(tag_arg));
        tag_arg.tag_type    = (uint16_t)obmafs_tag;
        tag_arg.data_length = data_len;

        int rrc = aaruf_read_media_tag(aaruf_ctx, tag_arg.data, dt, &data_len);
        if(rrc != 0) continue; /* Skip tags we can't read */

        if(ioctl(fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag_arg) != 0)
            fprintf(stderr, "Warning: failed to store media tag %d (errno=%d)\n", obmafs_tag, errno);
    }

    return 0;
}

/**
 * Import metadata strings from the AIF ImageInfo via ioctl.
 *
 * @param info  ImageInfo struct from libaaruformat.
 * @param fd    Open file descriptor on the mounted OBMAFS3 file.
 */
/**
 * Helper: read a UTF-16LE string metadata field from libaaruformat,
 * convert it to UTF-8 via iconv, and store it via the SET_METADATA ioctl.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 * @param getter     Function pointer to the aaruf_get_* accessor.
 * @param key        Metadata key name to store.
 */
static void import_utf16_metadata(void *aaruf_ctx, int fd,
                                  int32_t (*getter)(const void *, uint8_t *, int32_t *),
                                  const char *key)
{
    int32_t length = 0;
    if(getter(aaruf_ctx, NULL, &length) != AARUF_ERROR_BUFFER_TOO_SMALL || length <= 0)
        return;

    uint8_t *utf16 = malloc((size_t)length);
    if(!utf16) return;

    if(getter(aaruf_ctx, utf16, &length) != AARUF_STATUS_OK)
    {
        free(utf16);
        return;
    }

    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if(cd == (iconv_t)-1)
    {
        free(utf16);
        return;
    }

    struct obmafs3_ioctl_metadata_set_arg meta;
    memset(&meta, 0, sizeof(meta));
    strncpy(meta.key, key, METADATA_KEY_MAX - 1);

    char   *inbuf  = (char *)utf16;
    size_t  inleft = (size_t)length;
    char   *outbuf = meta.value;
    size_t  outleft = METADATA_VALUE_MAX - 1;

    if(iconv(cd, &inbuf, &inleft, &outbuf, &outleft) != (size_t)-1 || errno == E2BIG)
    {
        /* outbuf advanced past the converted bytes; meta.value is already NUL-filled */
        if(meta.value[0])
        {
            if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
                fprintf(stderr, "Warning: failed to set metadata '%s'\n", key);
        }
    }

    iconv_close(cd);
    free(utf16);
}

static void import_metadata(void *aaruf_ctx, const ImageInfo *info, int fd)
{
    struct obmafs3_ioctl_metadata_set_arg meta;

    if(info->Application[0])
    {
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.key, "application", METADATA_KEY_MAX - 1);
        strncpy(meta.value, info->Application, METADATA_VALUE_MAX - 1);
        if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
            fprintf(stderr, "Warning: failed to set metadata 'application'\n");
    }

    if(info->ApplicationVersion[0])
    {
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.key, "application_version", METADATA_KEY_MAX - 1);
        strncpy(meta.value, info->ApplicationVersion, METADATA_VALUE_MAX - 1);
        if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
            fprintf(stderr, "Warning: failed to set metadata 'application_version'\n");
    }

    /* Store media type as a numeric string */
    {
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.key, "media_type", METADATA_KEY_MAX - 1);
        snprintf(meta.value, METADATA_VALUE_MAX, "%d", info->MediaType);
        if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
            fprintf(stderr, "Warning: failed to set metadata 'media_type'\n");
    }

    /* Store CHS geometry if available */
    {
        uint32_t cylinders = 0, heads = 0, sectors_per_track = 0;
        if(aaruf_get_geometry(aaruf_ctx, &cylinders, &heads, &sectors_per_track) == AARUF_STATUS_OK)
        {
            memset(&meta, 0, sizeof(meta));
            strncpy(meta.key, "geometry", METADATA_KEY_MAX - 1);
            snprintf(meta.value, METADATA_VALUE_MAX, "%" PRIu32 "/%" PRIu32 "/%" PRIu32, cylinders, heads,
                     sectors_per_track);
            if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
                fprintf(stderr, "Warning: failed to set metadata 'geometry'\n");
        }
    }

    /* Store media sequence if available (multi-volume sets) */
    {
        int32_t sequence = 0, last_sequence = 0;
        if(aaruf_get_media_sequence(aaruf_ctx, &sequence, &last_sequence) == AARUF_STATUS_OK && sequence > 0)
        {
            memset(&meta, 0, sizeof(meta));
            strncpy(meta.key, "media_sequence", METADATA_KEY_MAX - 1);
            snprintf(meta.value, METADATA_VALUE_MAX, "%" PRId32 "/%" PRId32, sequence, last_sequence);
            if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
                fprintf(stderr, "Warning: failed to set metadata 'media_sequence'\n");
        }
    }

    /* Store UTF-16LE string metadata fields (converted to UTF-8) */
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_creator, "dumper");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_comments, "comments");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_title, "title");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_manufacturer, "manufacturer");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_model, "model");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_serial_number, "serial_number");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_barcode, "barcode");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_part_number, "part_number");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_manufacturer, "drive_manufacturer");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_model, "drive_model");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_serial_number, "drive_serial_number");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_firmware_revision, "drive_firmware_revision");
}

/**
 * Import a non-optical (flat) media image: read sectors sequentially
 * and write them through the mounted filesystem.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 * @param info       ImageInfo from libaaruformat.
 * @return 0 on success, -1 on error.
 */
static int import_flat_image(void *aaruf_ctx, int fd, const ImageInfo *info)
{
    uint32_t buf_cap = (uint32_t)info->SectorSize;
    uint64_t sectors = info->Sectors;

    uint8_t *buf = malloc(buf_cap);
    if(!buf) return -1;

    fprintf(stderr, "Importing %" PRIu64 " sectors (nominal sector size %u)...\n", sectors, (unsigned)buf_cap);

    for(uint64_t s = 0; s < sectors; s++)
    {
        uint32_t length = buf_cap;
        uint8_t  status = 0;

        int rrc = aaruf_read_sector(aaruf_ctx, s, false, buf, &length, &status);

        if(rrc == AARUF_ERROR_BUFFER_TOO_SMALL && length > buf_cap)
        {
            /* Sector is larger than current buffer — grow and retry */
            uint8_t *tmp = realloc(buf, length);
            if(!tmp)
            {
                fprintf(stderr, "Error: out of memory re-allocating sector buffer to %u bytes\n", length);
                free(buf);
                return -1;
            }
            buf     = tmp;
            buf_cap = length;
            rrc     = aaruf_read_sector(aaruf_ctx, s, false, buf, &length, &status);
        }

        if(rrc != AARUF_STATUS_OK)
        {
            fprintf(stderr, "Warning: failed to read sector %" PRIu64 " (rc=%d), filling with zeroes\n", s, rrc);
            memset(buf, 0, buf_cap);
            length = buf_cap;
        }

        /* length may be smaller than buf_cap for this sector — write only what was returned */
        ssize_t written = write(fd, buf, length);
        if(written < 0 || (uint32_t)written != length)
        {
            fprintf(stderr, "Error: write failed at sector %" PRIu64 " (errno=%d)\n", s, errno);
            free(buf);
            return -1;
        }

        /* Progress every 10000 sectors */
        if((s % 10000) == 0 && s > 0)
            fprintf(stderr, "\r  %" PRIu64 "/%" PRIu64 " sectors (%.1f%%)", s, sectors, (double)s / (double)sectors * 100.0);
    }

    fprintf(stderr, "\r  %" PRIu64 "/%" PRIu64 " sectors (100.0%%)\n", sectors, sectors);

    free(buf);
    return 0;
}

/**
 * Import a CD/optical image with track-aware sector handling via ioctls.
 *
 * Reads track information from the AIF, then reads each raw sector
 * (optionally with subchannel data) and sends it through the
 * OBMAFS3_IOC_CD_WRITE_LONG ioctl.  The FUSE layer handles all
 * prefix/suffix/subchannel/ECC/dedup processing internally.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 * @param info       ImageInfo from libaaruformat.
 * @return 0 on success, -1 on error.
 */
static int import_cd_image(void *aaruf_ctx, int fd, const ImageInfo *info)
{
    /* Get track information */
    uint8_t track_buf[4096];
    size_t  track_buf_len = sizeof(track_buf);
    int     track_count   = aaruf_get_tracks(aaruf_ctx, track_buf, &track_buf_len);

    if(track_count <= 0)
    {
        fprintf(stderr, "Warning: no tracks found, cannot import as CD image\n");
        return -1;
    }

    /* Parse track entries */
    TrackEntry *tracks = (TrackEntry *)track_buf;

    uint64_t total_sectors = info->Sectors;
    uint64_t imported      = 0;

    /* Initialize ECC context for prefix/suffix reconstruction */
    void *ecc_ctx = aaruf_ecc_cd_init();
    if(!ecc_ctx)
    {
        fprintf(stderr, "Error: failed to initialize ECC context\n");
        return -1;
    }

    /* Check if subchannel data is available */
    int has_subchannel = 0;
    {
        /* CdSectorSubchannelAaru = 8 in aaru.h SectorTagType enum;
         * MaxSectorTag = 21, so the bool array has 22 entries. */
        uint8_t stag_buf[22];
        size_t  stag_len = sizeof(stag_buf);
        int     strc     = aaruf_get_readable_sector_tags(aaruf_ctx, stag_buf, &stag_len);
        if(strc == 0 && stag_len >= 9 && stag_buf[8])
            has_subchannel = 1;
    }
    fprintf(stderr, "  Subchannel: %s\n", has_subchannel ? "available" : "not available");

    fprintf(stderr, "Importing CD image: %" PRIu64 " sectors, %d tracks\n", total_sectors, track_count);

    for(int t = 0; t < track_count; t++)
    {
        TrackEntry *trk   = &tracks[t];
        int         mode  = aaruf_track_type_to_cd_mode(trk->type);
        uint16_t    ss    = cd_mode_sector_size(mode);
        int64_t     start = trk->start;
        int64_t     end   = trk->end;

        if(mode < 0 || ss == 0)
        {
            fprintf(stderr, "Warning: unknown track type %d for track %d, skipping\n",
                    trk->type, trk->sequence);
            continue;
        }

        fprintf(stderr, "  Track %d: sectors %" PRId64 "-%" PRId64 " (%s, %u bytes/sector)\n",
                trk->sequence, start, end,
                mode == kCdSectorModeAudio ? "Audio" :
                mode == kCdSectorMode1 ? "Mode1" :
                mode == kCdSectorMode2 ? "Mode2" :
                mode == kCdSectorMode2Form1 ? "Mode2Form1" : "Mode2Form2",
                ss);

        for(int64_t s = start; s <= end; s++)
        {
            struct obmafs3_ioctl_cd_write_arg cd_arg;
            memset(&cd_arg, 0, sizeof(cd_arg));
            cd_arg.sector_mode = (uint8_t)mode;

            /* Try to read a raw (long) sector first */
            uint32_t length = CD_RAW_SECTOR_SIZE;
            uint8_t  status = 0;

            int rrc = aaruf_read_sector_long(aaruf_ctx, (uint64_t)s, false, cd_arg.buffer, &length, &status);

            if(rrc == 0 && length == CD_RAW_SECTOR_SIZE)
            {
                /* Full 2352-byte raw sector */
                cd_arg.buffer_size = CD_RAW_SECTOR_SIZE;
            }
            else
            {
                /* Fall back to cooked sector read, then reconstruct
                 * the full 2352-byte raw sector with proper prefix
                 * and suffix using the ECC engine. */
                memset(cd_arg.buffer, 0, CD_RAW_SECTOR_SIZE);

                /* Determine where cooked data sits within the 2352-byte raw sector */
                uint16_t data_offset;
                switch(mode)
                {
                    case kCdSectorModeAudio:
                        data_offset = 0;
                        break;
                    case kCdSectorMode1:
                    case kCdSectorMode2:
                        data_offset = 16; /* after sync+header */
                        break;
                    case kCdSectorMode2Form1:
                    case kCdSectorMode2Form2:
                        data_offset = 24; /* after sync+header+subheader */
                        break;
                    default:
                        data_offset = 16;
                        break;
                }

                length = ss;
                rrc    = aaruf_read_sector(aaruf_ctx, (uint64_t)s, false,
                                           cd_arg.buffer + data_offset, &length, &status);
                if(rrc != 0)
                {
                    fprintf(stderr, "Warning: failed to read sector %" PRId64 " (rc=%d), filling with zeroes\n",
                            s, rrc);
                    memset(cd_arg.buffer, 0, CD_RAW_SECTOR_SIZE);
                }

                /* Reconstruct sync+header prefix from LBA and track type */
                if(mode != kCdSectorModeAudio)
                    aaruf_ecc_cd_reconstruct_prefix(cd_arg.buffer, trk->type, s);

                /* Reconstruct EDC/ECC suffix (Mode1, Mode2Form1, Mode2Form2) */
                if(mode == kCdSectorMode1 || mode == kCdSectorMode2Form1 || mode == kCdSectorMode2Form2)
                    aaruf_ecc_cd_reconstruct(ecc_ctx, cd_arg.buffer, trk->type);

                cd_arg.buffer_size = CD_RAW_SECTOR_SIZE;
            }

            /* Append subchannel data if available */
            if(has_subchannel)
            {
                uint32_t sub_len = 96;
                int      src     = aaruf_read_sector_tag(aaruf_ctx, (uint64_t)s, false,
                                                         cd_arg.buffer + CD_RAW_SECTOR_SIZE,
                                                         &sub_len, 8 /* CdSectorSubchannelAaru */);
                if(src == 0 && sub_len == 96)
                    cd_arg.buffer_size = CD_RAW_PLUS_SUB;
            }

            if(ioctl(fd, OBMAFS3_IOC_CD_WRITE_LONG, &cd_arg) != 0)
            {
                fprintf(stderr, "Error: CD_WRITE_LONG failed at sector %" PRId64 " (errno=%d)\n", s, errno);
                return -1;
            }

            imported++;
            if(imported > 0 && (imported % 10000) == 0)
                fprintf(stderr, "\r  %" PRIu64 "/%" PRIu64 " sectors (%.1f%%)",
                        imported, total_sectors, (double)imported / (double)total_sectors * 100.0);
        }
    }

    fprintf(stderr, "\r  %" PRIu64 "/%" PRIu64 " sectors (100.0%%)\n", imported, imported);

    aaruf_ecc_cd_free(ecc_ctx);
    return 0;
}

/**
 * Entry point for import-aif.
 *
 * Opens an Aaru Image Format file, creates a new file on the mounted
 * OBMAFS3 filesystem, and imports all sector data, media tags, and
 * metadata through standard POSIX I/O and ioctls.
 */
int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        { "help", no_argument, NULL, 'h'},
        {  NULL,            0, NULL,   0}
    };

    int opt;

    while((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1)
    {
        switch(opt)
        {
            case 'h':
            default:
                usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    if(optind + 2 > argc)
    {
        fprintf(stderr, "Error: expected <aif-file> and <output-path>\n");
        usage(argv[0]);
        return 1;
    }

    const char *aif_path    = argv[optind];
    const char *output_path = argv[optind + 1];

    /* ---- Open the AIF file ---- */
    fprintf(stderr, "Opening AIF: %s\n", aif_path);
    void *aaruf_ctx = aaruf_open(aif_path, false, NULL);
    if(!aaruf_ctx)
    {
        fprintf(stderr, "Error: failed to open AIF file: %s\n", aif_path);
        return 1;
    }

    /* Get image info */
    ImageInfo info;
    memset(&info, 0, sizeof(info));
    aaruf_get_image_info(aaruf_ctx, &info);

    fprintf(stderr, "  Sectors:    %" PRIu64 "\n", info.Sectors);
    fprintf(stderr, "  SectorSize: %u\n", info.SectorSize);
    fprintf(stderr, "  MediaType:  %d\n", info.MediaType);

    int optical = is_optical_media(&info);
    fprintf(stderr, "  Image type: %s\n", optical ? "Optical (CD/DVD/BD)" : "Flat media image");

    /* ---- Create parent directories ---- */
    if(mkdirs(output_path) != 0)
    {
        fprintf(stderr, "Error: failed to create parent directories for '%s' (errno=%d)\n", output_path, errno);
        aaruf_close(aaruf_ctx);
        return 1;
    }

    /* ---- Create the output file ---- */
    fprintf(stderr, "Creating: %s\n", output_path);
    int fd = open(output_path, O_CREAT | O_RDWR | O_EXCL, 0644);
    if(fd < 0)
    {
        fprintf(stderr, "Error: failed to create '%s' (errno=%d: %s)\n", output_path, errno, strerror(errno));
        aaruf_close(aaruf_ctx);
        return 1;
    }

    /* ---- Convert to appropriate file type ---- */
    if(optical)
    {
        if(ioctl(fd, OBMAFS3_IOC_SET_CD_IMAGE) != 0)
        {
            fprintf(stderr, "Error: SET_CD_IMAGE ioctl failed (errno=%d: %s)\n", errno, strerror(errno));
            close(fd);
            unlink(output_path);
            aaruf_close(aaruf_ctx);
            return 1;
        }
        fprintf(stderr, "  File type: CompactDiscImage\n");
    }
    else
    {
        struct obmafs3_ioctl_set_media_image_arg mia;
        memset(&mia, 0, sizeof(mia));
        mia.sector_size = (uint16_t)info.SectorSize;

        if(ioctl(fd, OBMAFS3_IOC_SET_MEDIA_IMAGE, &mia) != 0)
        {
            fprintf(stderr, "Error: SET_MEDIA_IMAGE ioctl failed (errno=%d: %s)\n", errno, strerror(errno));
            close(fd);
            unlink(output_path);
            aaruf_close(aaruf_ctx);
            return 1;
        }
        fprintf(stderr, "  File type: MediaImage (sector size %u)\n", info.SectorSize);
    }

    /* ---- Import sector data ---- */
    int rc;
    if(optical)
        rc = import_cd_image(aaruf_ctx, fd, &info);
    else
        rc = import_flat_image(aaruf_ctx, fd, &info);

    if(rc != 0)
    {
        fprintf(stderr, "Error: import failed\n");
        close(fd);
        aaruf_close(aaruf_ctx);
        return 1;
    }

    /* ---- Import media tags ---- */
    fprintf(stderr, "Importing media tags...\n");
    import_media_tags(aaruf_ctx, fd);

    /* ---- Import metadata ---- */
    fprintf(stderr, "Importing metadata...\n");
    import_metadata(aaruf_ctx, &info, fd);

    /* Compute output base path (without extension) for sidecar files */
    size_t      base_len = strlen(output_path);
    const char *dot      = strrchr(output_path, '.');
    const char *slash    = strrchr(output_path, '/');
    if(dot && (!slash || dot > slash))
        base_len = (size_t)(dot - output_path);

    /* ---- Export CICM metadata XML if available ---- */
    {
        size_t cicm_len = 0;
        if(aaruf_get_cicm_metadata(aaruf_ctx, NULL, &cicm_len) == AARUF_ERROR_BUFFER_TOO_SMALL && cicm_len > 0)
        {
            uint8_t *cicm_buf = malloc(cicm_len);
            if(cicm_buf)
            {
                if(aaruf_get_cicm_metadata(aaruf_ctx, cicm_buf, &cicm_len) == AARUF_STATUS_OK)
                {
                    char *xml_path = malloc(base_len + sizeof(".metadata.xml"));
                    if(xml_path)
                    {
                        memcpy(xml_path, output_path, base_len);
                        memcpy(xml_path + base_len, ".metadata.xml", sizeof(".metadata.xml"));

                        int xml_fd = open(xml_path, O_CREAT | O_WRONLY | O_EXCL, 0644);
                        if(xml_fd >= 0)
                        {
                            ssize_t written = write(xml_fd, cicm_buf, cicm_len);
                            if(written < 0 || (size_t)written != cicm_len)
                                fprintf(stderr, "Warning: incomplete write of CICM metadata XML\n");
                            else
                                fprintf(stderr, "CICM metadata saved to %s\n", xml_path);
                            close(xml_fd);
                        }
                        else
                        {
                            fprintf(stderr, "Warning: failed to create '%s' (errno=%d: %s)\n",
                                    xml_path, errno, strerror(errno));
                        }
                        free(xml_path);
                    }
                }
                free(cicm_buf);
            }
        }
    }

    /* ---- Export Aaru JSON metadata if available ---- */
    {
        size_t json_len = 0;
        if(aaruf_get_aaru_json_metadata(aaruf_ctx, NULL, &json_len) == AARUF_ERROR_BUFFER_TOO_SMALL && json_len > 0)
        {
            uint8_t *json_buf = malloc(json_len);
            if(json_buf)
            {
                if(aaruf_get_aaru_json_metadata(aaruf_ctx, json_buf, &json_len) == AARUF_STATUS_OK)
                {
                    char *json_path = malloc(base_len + sizeof(".metadata.json"));
                    if(json_path)
                    {
                        memcpy(json_path, output_path, base_len);
                        memcpy(json_path + base_len, ".metadata.json", sizeof(".metadata.json"));

                        int json_fd = open(json_path, O_CREAT | O_WRONLY | O_EXCL, 0644);
                        if(json_fd >= 0)
                        {
                            ssize_t written = write(json_fd, json_buf, json_len);
                            if(written < 0 || (size_t)written != json_len)
                                fprintf(stderr, "Warning: incomplete write of Aaru JSON metadata\n");
                            else
                                fprintf(stderr, "Aaru JSON metadata saved to %s\n", json_path);
                            close(json_fd);
                        }
                        else
                        {
                            fprintf(stderr, "Warning: failed to create '%s' (errno=%d: %s)\n",
                                    json_path, errno, strerror(errno));
                        }
                        free(json_path);
                    }
                }
                free(json_buf);
            }
        }
    }

    /* ---- Cleanup ---- */
    close(fd);
    aaruf_close(aaruf_ctx);

    fprintf(stderr, "Import complete: %s -> %s\n", aif_path, output_path);
    return 0;
}
