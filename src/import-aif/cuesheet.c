/*
 * import-aif — CDRWin cue sheet generation.
 */

#include "import_aif.h"

/**
 * Map a MediaType enum value to a CDRWin "REM ORIGINAL MEDIA-TYPE" string.
 */
static const char *media_type_to_cdrwin_string(uint32_t media_type)
{
    switch(media_type)
    {
        case 10:  /* CD */
        case 11:  /* CDDA */
        case 12:  /* CDG */
        case 13:  /* CDEG */
        case 14:  /* CDI */
        case 15:  /* CDROM */
        case 16:  /* CDROMXA */
        case 17:  /* CDPLUS */
        case 18:  /* CDMO */
        case 19:  /* CDR */
        case 22:  /* VCD */
        case 23:  /* SVCD */
        case 24:  /* PCD */
        case 25:  /* SACD */
        case 26:  /* DDCD */
        case 27:  /* DDCDR */
        case 29:  /* DTSCD */
        case 30:  /* CDMIDI */
        case 31:  /* CDV */
        case 34:  /* CDIREADY */
        case 35:  /* FMTOWNS */
            return "CD";
        case 20:  /* CDRW */
            return "CD-RW";
        case 21:  /* CDMRW */
            return "CD-MRW";
        case 28:  /* DDCDRW */
            return "CD-RW";
        default:
            return "CD";
    }
}

/**
 * Map a MediaType enum value to its Aaru enum name string.
 *
 * Used for "REM METADATA AARU MEDIA-TYPE" line in the cue sheet.
 */
static const char *media_type_to_aaru_string(uint32_t media_type)
{
    switch(media_type)
    {
        case 10:  return "CD";
        case 11:  return "CDDA";
        case 12:  return "CDG";
        case 13:  return "CDEG";
        case 14:  return "CDI";
        case 15:  return "CDROM";
        case 16:  return "CDROMXA";
        case 17:  return "CDPLUS";
        case 18:  return "CDMO";
        case 19:  return "CDR";
        case 20:  return "CDRW";
        case 21:  return "CDMRW";
        case 22:  return "VCD";
        case 23:  return "SVCD";
        case 24:  return "PCD";
        case 25:  return "SACD";
        case 26:  return "DDCD";
        case 27:  return "DDCDR";
        case 28:  return "DDCDRW";
        case 29:  return "DTSCD";
        case 30:  return "CDMIDI";
        case 31:  return "CDV";
        case 32:  return "PD650";
        case 33:  return "PD650_WORM";
        case 34:  return "CDIREADY";
        case 35:  return "FMTOWNS";
        case 112: return "PS1CD";
        case 113: return "PS2CD";
        case 150: return "MEGACD";
        case 151: return "SATURNCD";
        case 152: return "GDROM";
        case 153: return "GDR";
        case 155: return "MilCD";
        case 171: return "SuperCDROM2";
        case 172: return "JaguarCD";
        case 173: return "ThreeDO";
        case 174: return "PCFX";
        case 175: return "NeoGeoCD";
        case 176: return "CDTV";
        case 177: return "CD32";
        case 179: return "Playdia";
        case 694: return "Pippin";
        case 740: return "VideoNow";
        case 741: return "VideoNowColor";
        case 742: return "VideoNowXp";
        default:  return "CD";
    }
}

/**
 * Map a libaaruformat TrackType + sector size (bps) to a CDRWin TRACK type string.
 */
static const char *track_type_to_cue_string(uint8_t track_type, uint32_t bps)
{
    switch(track_type)
    {
        case 0: /* Audio */
            return (bps == 2448) ? "CDG" : "AUDIO";
        case 1: /* Data */
            return "MODE1/2048";
        case 2: /* CdMode1 */
            return (bps == 2352) ? "MODE1/2352" : "MODE1/2048";
        case 3: /* CdMode2Formless */
            return (bps == 2352) ? "MODE2/2352" : "MODE2/2336";
        case 4: /* CdMode2Form1 */
            return (bps == 2352) ? "MODE2/2352" : "MODE2/2048";
        case 5: /* CdMode2Form2 */
            return (bps == 2352) ? "MODE2/2352" : "MODE2/2324";
        default:
            return "MODE1/2352";
    }
}

/**
 * Convert an LBA sector address to MSF format (MM:SS:FF).
 */
static void lba_to_msf(int64_t lba, int *m, int *s, int *f)
{
    *f = (int)(lba % 75);
    *s = (int)((lba / 75) % 60);
    *m = (int)(lba / 75 / 60);
}

/**
 * Write an Aaru-format CDRWin cue sheet file for a compact disc image.
 *
 * The cue file path is derived from the output path by stripping the
 * extension and appending ".cue".
 *
 * @param aaruf_ctx   libaaruformat context.
 * @param info        ImageInfo from libaaruformat.
 * @param output_path Path to the imported image file.
 * @param base_len    Length of output_path without extension.
 * @return 0 on success, -1 on error.
 */
int write_cue_file(void *aaruf_ctx, const ImageInfo *info,
                   const char *output_path, size_t base_len)
{
    /* Get track information */
    uint8_t track_buf[4096];
    size_t  track_buf_len = sizeof(track_buf);
    int     track_count   = aaruf_get_tracks(aaruf_ctx, track_buf, &track_buf_len);

    if(track_count <= 0)
    {
        fprintf(stderr, "Warning: no tracks found, cannot write cue sheet\n");
        return -1;
    }

    TrackEntry *tracks = (TrackEntry *)track_buf;

    /* Build the cue file path */
    char *cue_path = malloc(base_len + sizeof(".cue"));
    if(!cue_path) return -1;
    memcpy(cue_path, output_path, base_len);
    memcpy(cue_path + base_len, ".cue", sizeof(".cue"));

    FILE *fp = fopen(cue_path, "w");
    if(!fp)
    {
        fprintf(stderr, "Warning: failed to create '%s' (errno=%d: %s)\n",
                cue_path, errno, strerror(errno));
        free(cue_path);
        return -1;
    }

    /* ---- REM comments / metadata ---- */
    fprintf(fp, "REM ORIGINAL MEDIA-TYPE: %s\n", media_type_to_cdrwin_string(info->MediaType));
    fprintf(fp, "REM METADATA AARU MEDIA-TYPE: %s\n", media_type_to_aaru_string(info->MediaType));

    if(info->Application[0])
        fprintf(fp, "REM Ripping Tool: %s\n", info->Application);
    if(info->ApplicationVersion[0])
        fprintf(fp, "REM Ripping Tool Version: %s\n", info->ApplicationVersion);

    /* ---- MCN (CATALOG) ---- */
    {
        uint8_t  mcn_buf[16];
        uint32_t mcn_len = sizeof(mcn_buf);
        /* CD_MCN = 6 in MediaTagType */
        if(aaruf_read_media_tag(aaruf_ctx, mcn_buf, 6, &mcn_len) == 0 && mcn_len > 0)
        {
            /* MCN is typically 13 bytes, ensure NUL termination */
            if(mcn_len < sizeof(mcn_buf))
                mcn_buf[mcn_len] = '\0';
            else
                mcn_buf[sizeof(mcn_buf) - 1] = '\0';
            fprintf(fp, "CATALOG %s\n", (char *)mcn_buf);
        }
    }

    /* ---- FILE declaration ---- */
    /* Use just the basename of the output path for the FILE line */
    const char *file_basename = strrchr(output_path, '/');
    file_basename = file_basename ? file_basename + 1 : output_path;
    fprintf(fp, "FILE \"%s\" BINARY\n", file_basename);

    /* ---- Determine how many sessions exist ---- */
    uint8_t max_session = 1;
    for(int t = 0; t < track_count; t++)
    {
        if(tracks[t].session > max_session)
            max_session = tracks[t].session;
    }

    /* ---- TRACK blocks ---- */
    uint8_t current_session = 0;

    for(int t = 0; t < track_count; t++)
    {
        TrackEntry *trk = &tracks[t];

        /* Session marker */
        if(trk->session != current_session)
        {
            /* Emit LEAD-OUT for the previous session (if not the first) */
            if(current_session > 0 && current_session < max_session && t > 0)
            {
                int64_t lead_out_lba = tracks[t - 1].end + 1;
                int     lm, ls, lf;
                lba_to_msf(lead_out_lba, &lm, &ls, &lf);
                fprintf(fp, "REM LEAD-OUT %02d:%02d:%02d\n", lm, ls, lf);
            }

            current_session = trk->session;

            /* Emit REM SESSION if multi-session */
            if(max_session > 1)
                fprintf(fp, "REM SESSION %d\n", current_session);
        }

        /* Determine the cooked sector size for this track */
        int      mode = aaruf_track_type_to_cd_mode(trk->type);
        uint16_t ss   = (mode >= 0) ? cd_mode_sector_size(mode) : 2048;

        /* TRACK line */
        const char *cue_type = track_type_to_cue_string(trk->type, ss);
        fprintf(fp, "  TRACK %02d %s\n", trk->sequence, cue_type);

        /* FLAGS */
        {
            /* Flag bits: bit0=Pre-emphasis, bit1=Copy permitted,
             * bit2=Data track, bit3=Four-channel audio */
            int has_flags = 0;
            char flags_str[64] = "";
            size_t fpos = 0;

            if(trk->flags & 0x02) { fpos += (size_t)snprintf(flags_str + fpos, sizeof(flags_str) - fpos, " DCP"); has_flags = 1; }
            if(trk->flags & 0x08) { fpos += (size_t)snprintf(flags_str + fpos, sizeof(flags_str) - fpos, " 4CH"); has_flags = 1; }
            if(trk->flags & 0x01) { fpos += (size_t)snprintf(flags_str + fpos, sizeof(flags_str) - fpos, " PRE"); has_flags = 1; }

            /* Only emit FLAGS if there's something besides DataTrack */
            if(has_flags)
                fprintf(fp, "    FLAGS%s\n", flags_str);
        }

        /* ISRC */
        {
            /* Check if ISRC is non-empty (not all zeros) */
            int has_isrc = 0;
            for(int i = 0; i < 13; i++)
            {
                if(trk->isrc[i] != 0) { has_isrc = 1; break; }
            }
            if(has_isrc)
                fprintf(fp, "    ISRC %.13s\n", trk->isrc);
        }

        /* INDEX 00 (pregap) — only for tracks that are not the first in their session */
        if(trk->pregap > 0)
        {
            /* Check if this is the first track in its session */
            int first_in_session = 1;
            for(int p = t - 1; p >= 0; p--)
            {
                if(tracks[p].session == trk->session)
                {
                    first_in_session = 0;
                    break;
                }
            }

            if(!first_in_session)
            {
                int64_t pregap_lba = trk->start - trk->pregap;
                int     pm, ps, pf;
                lba_to_msf(pregap_lba, &pm, &ps, &pf);
                fprintf(fp, "    INDEX 00 %02d:%02d:%02d\n", pm, ps, pf);
            }
        }

        /* INDEX 01 (track start) */
        {
            int im, is, ifi;
            lba_to_msf(trk->start, &im, &is, &ifi);
            fprintf(fp, "    INDEX 01 %02d:%02d:%02d\n", im, is, ifi);
        }
    }

    /* Final session LEAD-OUT is NOT written (matches Aaru behavior) */

    fclose(fp);
    fprintf(stderr, "Cue sheet saved to %s\n", cue_path);
    free(cue_path);
    return 0;
}
