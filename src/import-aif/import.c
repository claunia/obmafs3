// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-aif
//
// --[ Description ] ----------------------------------------------------------
//
//     Sector data import (flat images and CD images).
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

/**
 * Import a non-optical (flat) media image: read sectors sequentially
 * and write them through the mounted filesystem.
 *
 * @param aaruf_ctx  libaaruformat context.
 * @param fd         Open file descriptor on the mounted OBMAFS3 file.
 * @param info       ImageInfo from libaaruformat.
 * @return 0 on success, -1 on error.
 */
int import_flat_image(void *aaruf_ctx, int fd, const ImageInfo *info)
{
    uint32_t buf_cap = (uint32_t)info->SectorSize;
    uint64_t sectors = info->Sectors;

    uint8_t *buf = malloc(buf_cap);
    if(!buf) return -1;

    ui_info("Sectors:", "%" PRIu64, sectors);
    ui_info("Sector size:", "%u bytes", (unsigned)buf_cap);

    for(uint64_t s = 0; s < sectors; s++)
    {
        uint32_t length = buf_cap;
        uint8_t  status = 0;

        int rrc = aaruf_read_sector(aaruf_ctx, s, false, buf, &length, &status);

        if(rrc == AARUF_ERROR_BUFFER_TOO_SMALL && length > buf_cap)
        {
            uint8_t *tmp = realloc(buf, length);
            if(!tmp)
            {
                ui_progress_clear();
                ui_error("Out of memory re-allocating sector buffer to %u bytes", length);
                free(buf);
                return -1;
            }
            buf     = tmp;
            buf_cap = length;
            rrc     = aaruf_read_sector(aaruf_ctx, s, false, buf, &length, &status);
        }

        if(rrc != AARUF_STATUS_OK)
        {
            memset(buf, 0, buf_cap);
            length = buf_cap;
        }

        ssize_t written = write(fd, buf, length);
        if(written < 0 || (uint32_t)written != length)
        {
            ui_progress_clear();
            ui_error("Write failed at sector %" PRIu64 " (errno=%d)", s, errno);
            free(buf);
            return -1;
        }

        if((s % 1000) == 0 || s + 1 == sectors)
            ui_progress("Importing", s + 1, sectors);
    }

    ui_progress("Importing", sectors, sectors);
    ui_progress_clear();
    ui_ok("Imported %" PRIu64 " sectors", sectors);

    free(buf);
    return 0;
}

/**
 * Import a compact disc image with track-aware sector handling via ioctls.
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
int import_cd_image(void *aaruf_ctx, int fd, const ImageInfo *info)
{
    /* Get track information */
    uint8_t *track_buf = NULL;
    size_t  track_buf_len = 0;
    int     err   = aaruf_get_tracks(aaruf_ctx, track_buf, &track_buf_len);

    if(err != AARUF_ERROR_BUFFER_TOO_SMALL || track_buf_len == 0)
    {
        ui_error("No tracks found, cannot import as CD image");
        return -1;
    }

    track_buf = malloc(track_buf_len);
    err   = aaruf_get_tracks(aaruf_ctx, track_buf, &track_buf_len);

    if(err != AARUF_STATUS_OK)
    {
        ui_error("Failed to read track information");
        return -1;
    }

    /* Parse track entries */
    TrackEntry *tracks = (TrackEntry *)track_buf;
    int         track_count = (int)(track_buf_len / sizeof(TrackEntry));

    /* Count total importable sectors from track ranges (start-pregap..end) */
    uint64_t total_sectors = 0;
    for(int t = 0; t < track_count; t++)
    {
        int64_t first = tracks[t].start - tracks[t].pregap;
        if(tracks[t].end >= first) total_sectors += (uint64_t)(tracks[t].end - first + 1);
    }
    uint64_t imported = 0;

    ui_info("Image sectors:", "%" PRIu64, info->Sectors);
    ui_info("Track sectors:", "%" PRIu64, total_sectors);

    /* Initialize ECC context for prefix/suffix reconstruction */
    void *ecc_ctx = aaruf_ecc_cd_init();
    if(!ecc_ctx)
    {
        ui_error("Failed to initialize ECC context");
        return -1;
    }

    /* Check if subchannel data is available */
    int has_subchannel = 0;
    {
        uint8_t stag_buf[22];
        size_t  stag_len = sizeof(stag_buf);
        int     strc     = aaruf_get_readable_sector_tags(aaruf_ctx, stag_buf, &stag_len);
        if(strc == 0 && stag_len >= 9 && stag_buf[8]) has_subchannel = 1;
    }
    ui_info("Subchannel:", "%s%s%s", has_subchannel ? C_GREEN : C_DIM,
            has_subchannel ? "available" : "not available", C_RESET);
    ui_info("Tracks:", "%d", track_count);

    /* Iterate track by track; only sectors within each track's range
     * (start-pregap .. end) are readable from the AIF.  The sector
     * field in the ioctl conveys the LBA, so gaps between tracks are
     * simply not written. */
    for(int t = 0; t < track_count; t++)
    {
        TrackEntry *trk   = &tracks[t];
        int         mode  = aaruf_track_type_to_cd_mode(trk->type);
        uint16_t    ss    = cd_mode_sector_size(mode);
        int64_t     start = trk->start - trk->pregap;
        int64_t     end   = trk->end;

        if(start < 0) start = 0;

        if(mode < 0 || ss == 0)
        {
            ui_warn("Unknown track type %d for track %d, skipping", trk->type, trk->sequence);
            continue;
        }

        const char *mode_name = mode == kCdSectorModeAudio    ? "Audio"
                                : mode == kCdSectorMode1      ? "Mode1"
                                : mode == kCdSectorMode2      ? "Mode2"
                                : mode == kCdSectorMode2Form1 ? "Mode2Form1"
                                : mode == kCdSectorMode2Form2 ? "Mode2Form2"
                                                              : "Unknown";
        ui_progress_clear();
        ui_track_info(trk->sequence, start, end, trk->pregap, mode_name, ss);

        for(int64_t s = start; s <= end; s++)
        {
            struct obmafs3_ioctl_cd_write_arg cd_arg;
            memset(&cd_arg, 0, sizeof(cd_arg));
            cd_arg.sector      = s;
            cd_arg.sector_mode = (uint8_t)mode;

            /* Try to read a raw (long) sector first */
            uint32_t length = CD_RAW_SECTOR_SIZE;
            uint8_t  status = 0;

            int rrc = aaruf_read_sector_long(aaruf_ctx, (uint64_t)s, false, cd_arg.buffer, &length, &status);

            if(rrc == AARUF_STATUS_OK && length == CD_RAW_SECTOR_SIZE)
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
                rrc = aaruf_read_sector(aaruf_ctx, (uint64_t)s, false, cd_arg.buffer + data_offset, &length, &status);
                if(rrc != AARUF_STATUS_OK)
                {
                    memset(cd_arg.buffer, 0, CD_RAW_SECTOR_SIZE);
                }

                /* Reconstruct sync+header prefix from LBA and track type */
                if(mode != kCdSectorModeAudio) aaruf_ecc_cd_reconstruct_prefix(cd_arg.buffer, trk->type, s);

                /* Reconstruct EDC/ECC suffix (Mode1, Mode2Form1, Mode2Form2) */
                if(mode == kCdSectorMode1 || mode == kCdSectorMode2Form1 || mode == kCdSectorMode2Form2)
                    aaruf_ecc_cd_reconstruct(ecc_ctx, cd_arg.buffer, trk->type);

                cd_arg.buffer_size = CD_RAW_SECTOR_SIZE;
            }

            /* Append subchannel data if available */
            if(has_subchannel)
            {
                uint32_t sub_len = 96;
                int      src = aaruf_read_sector_tag(aaruf_ctx, (uint64_t)s, false, cd_arg.buffer + CD_RAW_SECTOR_SIZE,
                                                     &sub_len, 8 /* CdSectorSubchannelAaru */);
                if(src == AARUF_STATUS_OK && sub_len == 96) cd_arg.buffer_size = CD_RAW_PLUS_SUB;
            }

            if(ioctl(fd, OBMAFS3_IOC_CD_WRITE_LONG, &cd_arg) != 0)
            {
                ui_progress_clear();
                ui_error("CD_WRITE_LONG failed at sector %" PRId64 " (errno=%d)", s, errno);
                aaruf_ecc_cd_free(ecc_ctx);
                return -1;
            }

            imported++;
            if((imported % 500) == 0 || imported == total_sectors)
                ui_progress("CD sectors", imported, total_sectors);
        }
    }

    ui_progress("CD sectors", imported, total_sectors);
    ui_progress_clear();
    ui_ok("Imported %" PRIu64 " sectors across %d tracks", imported, track_count);

    aaruf_ecc_cd_free(ecc_ctx);
    return 0;
}
