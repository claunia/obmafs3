// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : convert.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-aif
//
// --[ Description ] ----------------------------------------------------------
//
//     Type mapping / conversion helpers.
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

#include "import_aif.h"

/**
 * Map libaaruformat MediaTagType values to OBMAFS3 MediaTagType.
 *
 * @param aaruf_tag  libaaruformat MediaTagType value.
 * @return OBMAFS3 MediaTagType, or -1 if not a media tag.
 */
int aaruf_tag_to_obmafs(int aaruf_tag)
{
    switch(aaruf_tag)
    {
        case CD_TOC:                        return kCdTableOfContents;
        case CD_SessionInfo:                return kCdSessionInfo;
        case CD_FullTOC:                    return kCdFullTOC;
        case CD_PMA:                        return kCdPMA;
        case CD_ATIP:                       return kCdATIP;
        case CD_TEXT:                        return kCdTEXT;
        case CD_MCN:                        return kCdMCN;
        case DVD_PFI:                       return kDvdPFI;
        case DVD_CMI:                       return kDvdCMI;
        case DVD_DiscKey:                   return kDvdDiscKey;
        case DVD_BCA:                       return kDvdBCA;
        case DVD_DMI:                       return kDvdDMI;
        case DVD_MediaIdentifier:           return kDvdMediaIdentifier;
        case DVD_MKB:                       return kDvdMKB;
        case DVDRAM_DDS:                    return kDvdRamDDS;
        case DVDRAM_MediumStatus:           return kDvdRamMediumStatus;
        case DVDRAM_SpareArea:              return kDvdRamSpareArea;
        case DVDR_RMD:                      return kDvdRecordableRMD;
        case DVDR_PreRecordedInfo:          return kDvdRecordablePreRecordedInfo;
        case DVDR_MediaIdentifier:          return kDvdRecordableMediaIdentifier;
        case DVDR_PFI:                      return kDvdRecordablePFI;
        case DVD_ADIP:                      return kDvdADIP;
        case HDDVD_CPI:                     return kHdDvdCPI;
        case HDDVD_MediumStatus:            return kHdDvdMediumStatus;
        case DVDDL_LayerCapacity:           return kDvdDlLayerCapacity;
        case DVDDL_MiddleZoneAddress:       return kDvdDlMiddleZoneAddress;
        case DVDDL_JumpIntervalSize:        return kDvdDlJumpIntervalSize;
        case DVDDL_ManualLayerJumpLBA:      return kDvdDlManualLayerJumpLBA;
        case BD_DI:                         return kBdDI;
        case BD_BCA:                        return kBdBCA;
        case BD_DDS:                        return kBdDDS;
        case BD_CartridgeStatus:            return kBdCartridgeStatus;
        case BD_SpareArea:                  return kBdSpareArea;
        case AACS_VolumeIdentifier:         return kAACS_VolumeIdentifier;
        case AACS_SerialNumber:             return kAACS_SerialNumber;
        case AACS_MediaIdentifier:          return kAACS_MediaIdentifier;
        case AACS_MKB:                      return kAACS_MKB;
        case AACS_DataKeys:                 return kAACS_DataKeys;
        case AACS_LBAExtents:               return kAACS_LBAExtents;
        case AACS_CPRM_MKB:                return kAACS_CPRM_MKB;
        case Hybrid_RecognizedLayers:       return kHybrid_RecognizedLayers;
        case MMC_WriteProtection:           return kMMC_WriteProtection;
        case MMC_DiscInformation:           return kMMC_DiscInformation;
        case MMC_TrackResourcesInformation: return kMMC_TrackResourcesInformation;
        case MMC_POWResourcesInformation:   return kMMC_POWResourcesInformation;
        case SCSI_INQUIRY:                  return kSCSI_INQUIRY;
        case SCSI_MODEPAGE_2A:              return kSCSI_MODEPAGE_2A;
        case ATA_IDENTIFY:                  return kATA_IDENTIFY;
        case ATAPI_IDENTIFY:                return kATAPI_IDENTIFY;
        case PCMCIA_CIS:                    return kPCMCIA_CIS;
        case SD_CID:                        return kSecureDigital_CID;
        case SD_CSD:                        return kSecureDigital_CSD;
        case SD_SCR:                        return kSecureDigital_SCR;
        case SD_OCR:                        return kSecureDigital_OCR;
        case MMC_CID:                       return kMMC_CID;
        case MMC_CSD:                       return kMMC_CSD;
        case MMC_OCR:                       return kMMC_OCR;
        case MMC_ExtendedCSD:               return kMMC_ExtendedCSD;
        case Xbox_SecuritySector:           return kXbox_SecuritySector;
        case Floppy_LeadOut:                return kFloppy_LeadOut;
        case DiscControlBlock:              return kDiscControlBlock;
        case CD_FirstTrackPregap:           return kCD_FirstTrackPregap;
        case CD_LeadOut:                    return kCD_LeadOut;
        case SCSI_MODESENSE_6:              return kSCSI_MODESENSE_6;
        case SCSI_MODESENSE_10:             return kSCSI_MODESENSE_10;
        case USB_Descriptors:               return kUSB_Descriptors;
        case Xbox_DMI:                      return kXbox_DMI;
        case Xbox_PFI:                      return kXbox_PFI;
        case CD_LeadIn:                     return -1; /* no OBMAFS3 equivalent */
        case MiniDiscType:                  return kMiniDiscType;
        case MiniDiscD5:                    return kMiniDiscD5;
        case MiniDiscUTOC:                  return kMiniDiscUTOC;
        case MiniDiscDTOC:                  return kMiniDiscDTOC;
        case DVD_DiscKey_Decrypted:         return kDVD_DiscKey_Decrypted;
        case DVD_PFI_2ndLayer:              return kDVD_PFI_2ndLayer;
        case Floppy_WriteProtect:           return kFloppy_WriteProtect;
        default:                            return -1;
    }
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
        case kCdSectorModeAudio:
            return CD_RAW_SECTOR_SIZE; /* 2352 */
        case kCdSectorMode1:
            return 2048; /* CD_DATA_SIZE */
        case kCdSectorMode2:
            return 2336;
        case kCdSectorMode2Form1:
            return 2048; /* CD_DATA_SIZE */
        case kCdSectorMode2Form2:
            return 2328;
        default:
            return 0;
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
        case 10:  // CD
        case 11:  // CDDA
        case 12:  // CDG
        case 13:  // CDEG
        case 14:  // CDI
        case 15:  // CDROM
        case 16:  // CDROMXA
        case 17:  // CDPLUS
        case 18:  // CDMO
        case 19:  // CDR
        case 20:  // CDRW
        case 21:  // CDMRW
        case 22:  // VCD
        case 23:  // SVCD
        case 24:  // PCD
        case 29:  // DTSCD
        case 30:  // CDMIDI
        case 31:  // CDV
        case 34:  // CDIREADY
        case 35:  // FMTOWNS

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
