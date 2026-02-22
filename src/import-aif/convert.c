/*
 * import-aif — Type mapping / conversion helpers.
 */

#include "import_aif.h"

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
int aaruf_tag_to_obmafs(int aaruf_tag)
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
int aaruf_track_type_to_cd_mode(int tt)
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
uint16_t cd_mode_sector_size(int mode)
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
 * Check if the media type represents a compact disc variant.
 *
 * Covers standard CD formats, gaming CDs, and specialty disc formats.
 */
int is_compact_disc_media(uint32_t media_type)
{
    switch(media_type)
    {
        // Standard Compact Disc formats (10–35)
        case 10:   // CD
        case 11:   // CDDA
        case 12:   // CDG
        case 13:   // CDEG
        case 14:   // CDI
        case 15:   // CDROM
        case 16:   // CDROMXA
        case 17:   // CDPLUS
        case 18:   // CDMO
        case 19:   // CDR
        case 20:   // CDRW
        case 21:   // CDMRW
        case 22:   // VCD
        case 23:   // SVCD
        case 24:   // PCD
        case 29:   // DTSCD
        case 30:   // CDMIDI
        case 31:   // CDV
        case 34:   // CDIREADY
        case 35:   // FMTOWNS

        // Gaming console CDs
        case 112:  // PS1CD
        case 113:  // PS2CD
        case 150:  // MEGACD
        case 151:  // SATURNCD
        case 152:  // GDROM
        case 153:  // GDR
        case 155:  // MilCD
        case 171:  // SuperCDROM2
        case 172:  // JaguarCD
        case 173:  // ThreeDO
        case 174:  // PCFX
        case 175:  // NeoGeoCD
        case 176:  // CDTV
        case 177:  // CD32
        case 179:  // Playdia
        case 694:  // Pippin

        // VideoNow
        case 740:  // VideoNow
        case 741:  // VideoNowColor
        case 742:  // VideoNowXp
            return 1;

        default:
            return 0;
    }
}
