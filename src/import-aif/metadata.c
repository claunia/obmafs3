// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : metadata.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-aif
//
// --[ Description ] ----------------------------------------------------------
//
//     Media tag and metadata import.
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

#include "errors.h"
#include "import_aif.h"
#include "ui.h"

#include <iconv.h>

/**
 * Import media tags from an AIF file via ioctl.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 * @return 0 on success, negative on error.
 */
int import_media_tags(void *aaruf_ctx, int fd)
{
    /* aaruf_get_readable_media_tags fills an array of booleans
     * indexed by AarufMediaTagType.  Entry [i] is true if tag i
     * is present in the image. */
    uint8_t tag_avail[MaxMediaTag + 1];
    size_t  tag_avail_len = sizeof(tag_avail);
    int     rc = aaruf_get_readable_media_tags(aaruf_ctx, tag_avail, &tag_avail_len);

    if(rc != AARUF_STATUS_OK) return 0;

    /* Walk every possible tag type and process the ones that are present */
    for(int i = 0; i <= MaxMediaTag; i++)
    {
        if(!tag_avail[i]) continue;

        int obmafs_tag = aaruf_tag_to_obmafs(i);
        if(obmafs_tag < 0) continue;

        /* Read the media tag data */
        uint32_t data_len = 0;
        uint8_t  probe    = 0;

        /* First call to get size */
        rc = aaruf_read_media_tag(aaruf_ctx, &probe, (uint32_t)i, &data_len);
        if(rc != AARUF_ERROR_BUFFER_TOO_SMALL || data_len == 0) continue;

        if(data_len > OBMAFS3_IOC_MAX_TAG_DATA)
        {
            ui_warn("Media tag %d too large (%u bytes), skipping", obmafs_tag, data_len);
            continue;
        }

        struct obmafs3_ioctl_tag_arg tag_arg;
        memset(&tag_arg, 0, sizeof(tag_arg));
        tag_arg.tag_type    = (uint16_t)obmafs_tag;
        tag_arg.data_length = data_len;

        rc = aaruf_read_media_tag(aaruf_ctx, tag_arg.data, (uint32_t)i, &data_len);
        if(rc != AARUF_STATUS_OK) continue; /* Skip tags we can't read */

        if(ioctl(fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag_arg) != 0)
            ui_warn("Failed to store media tag %d (errno=%d)", obmafs_tag, errno);
    }

    return 0;
}

/**
 * Helper: read a UTF-16LE string metadata field from libaaruformat,
 * convert it to UTF-8 via iconv, and store it via the SET_METADATA ioctl.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 * @param getter     Function pointer to the aaruf_get_* accessor.
 * @param key        Metadata key name to store.
 */
static void import_utf16_metadata(void *aaruf_ctx, int fd, int32_t (*getter)(const void *, uint8_t *, int32_t *),
                                  const char *key)
{
    int32_t length = 0;
    if(getter(aaruf_ctx, NULL, &length) != AARUF_ERROR_BUFFER_TOO_SMALL || length <= 0) return;

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

    char  *inbuf   = (char *)utf16;
    size_t inleft  = (size_t)length;
    char  *outbuf  = meta.value;
    size_t outleft = METADATA_VALUE_MAX - 1;

    if(iconv(cd, &inbuf, &inleft, &outbuf, &outleft) != (size_t)-1 || errno == E2BIG)
    {
        /* outbuf advanced past the converted bytes; meta.value is already NUL-filled */
        if(meta.value[0])
        {
            if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
                ui_warn("Failed to set metadata '%s'", key);
        }
    }

    iconv_close(cd);
    free(utf16);
}

/**
 * Import metadata strings from the AIF ImageInfo via ioctl.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param info       ImageInfo struct from libaaruformat.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 */
void import_metadata(void *aaruf_ctx, const ImageInfo *info, int fd)
{
    struct obmafs3_ioctl_metadata_set_arg meta;

    memset(&meta, 0, sizeof(meta));
    strncpy(meta.key, "ImportApplication", METADATA_KEY_MAX - 1);
    strncpy(meta.value, "import-aif (OBMAFS3)", METADATA_VALUE_MAX - 1);
    if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
        ui_warn("Failed to set metadata 'ImportApplication'");

    if(info->Application[0])
    {
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.key, "Application", METADATA_KEY_MAX - 1);
        strncpy(meta.value, info->Application, METADATA_VALUE_MAX - 1);
        if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
            ui_warn("Failed to set metadata 'Application'");
    }

    if(info->ApplicationVersion[0])
    {
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.key, "ApplicationVersion", METADATA_KEY_MAX - 1);
        strncpy(meta.value, info->ApplicationVersion, METADATA_VALUE_MAX - 1);
        if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
            ui_warn("Failed to set metadata 'ApplicationVersion'");
    }

    /* Store media type as a numeric string */
    {
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.key, "AaruMediaType", METADATA_KEY_MAX - 1);
        snprintf(meta.value, METADATA_VALUE_MAX, "%d", info->MediaType);
        if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
            ui_warn("Failed to set metadata 'AaruMediaType'");
    }

    /* Store CHS geometry if available */
    {
        uint32_t cylinders = 0, heads = 0, sectors_per_track = 0;
        if(aaruf_get_geometry(aaruf_ctx, &cylinders, &heads, &sectors_per_track) == AARUF_STATUS_OK)
        {
            memset(&meta, 0, sizeof(meta));
            strncpy(meta.key, "Geometry", METADATA_KEY_MAX - 1);
            snprintf(meta.value, METADATA_VALUE_MAX, "%" PRIu32 "/%" PRIu32 "/%" PRIu32, cylinders, heads,
                     sectors_per_track);
            if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
                ui_warn("Failed to set metadata 'Geometry'");
        }
    }

    /* Store media sequence if available (multi-volume sets) */
    {
        int32_t sequence = 0, last_sequence = 0;
        if(aaruf_get_media_sequence(aaruf_ctx, &sequence, &last_sequence) == AARUF_STATUS_OK && sequence > 0)
        {
            memset(&meta, 0, sizeof(meta));
            strncpy(meta.key, "MediaSequence", METADATA_KEY_MAX - 1);
            snprintf(meta.value, METADATA_VALUE_MAX, "%" PRId32 "/%" PRId32, sequence, last_sequence);
            if(ioctl(fd, OBMAFS3_IOC_SET_METADATA, &meta) != 0)
                ui_warn("Failed to set metadata 'MediaSequence'");
        }
    }

    /* Store UTF-16LE string metadata fields (converted to UTF-8) */
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_creator, "Dumper");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_comments, "Comments");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_title, "Title");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_manufacturer, "Manufacturer");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_model, "Model");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_serial_number, "SerialNumber");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_barcode, "Barcode");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_media_part_number, "PartNumber");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_manufacturer, "DriveManufacturer");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_model, "DriveModel");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_serial_number, "DriveSerialNumber");
    import_utf16_metadata(aaruf_ctx, fd, aaruf_get_drive_firmware_revision, "DriveFirmwareRevision");
}
