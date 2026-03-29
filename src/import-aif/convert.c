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

#include "aaru.h"
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
        case kMediaTagCdToc:
            return kCdTableOfContents;
        case kMediaTagSessionInfo:
            return kCdSessionInfo;
        case kMediaTagFullToc:
            return kCdFullTOC;
        case kMediaTagCdPma:
            return kCdPMA;
        case kMediaTagCdAtip:
            return kCdATIP;
        case kMediaTagCdText:
            return kCdTEXT;
        case kMediaTagCdMcn:
            return kCdMCN;
        case kMediaTagDvdPfi:
            return kDvdPFI;
        case kMediaTagDvdCmi:
            return kDvdCMI;
        case kMediaTagDvdDiscKey:
            return kDvdDiscKey;
        case kMediaTagDvdBca:
            return kDvdBCA;
        case kMediaTagDvdDmi:
            return kDvdDMI;
        case kMediaTagDvdMediaIdentifier:
            return kDvdMediaIdentifier;
        case kMediaTagDvdMkb:
            return kDvdMKB;
        case kMediaTagDvdRamDds:
            return kDvdRamDDS;
        case kMediaTagDvdRamMediumStatus:
            return kDvdRamMediumStatus;
        case kMediaTagDvdRamSpareArea:
            return kDvdRamSpareArea;
        case kMediaTagDvdrRmd:
            return kDvdRecordableRMD;
        case kMediaTagDvdrPreRecordedInfo:
            return kDvdRecordablePreRecordedInfo;
        case kMediaTagDvdrMediaIdentifier:
            return kDvdRecordableMediaIdentifier;
        case kMediaTagDvdrPfi:
            return kDvdRecordablePFI;
        case kMediaTagDvdAdip:
            return kDvdADIP;
        case kMediaTagHddvdCpi:
            return kHdDvdCPI;
        case kMediaTagHddvdMediumStatus:
            return kHdDvdMediumStatus;
        case kMediaTagDvddlLayerCapacity:
            return kDvdDlLayerCapacity;
        case kMediaTagDvddlMiddleZoneAddress:
            return kDvdDlMiddleZoneAddress;
        case kMediaTagDvddlJumpIntervalSize:
            return kDvdDlJumpIntervalSize;
        case kMediaTagDvddlManualLayerJumpLba:
            return kDvdDlManualLayerJumpLBA;
        case kMediaTagBlurayDi:
            return kBdDI;
        case kMediaTagBlurayBca:
            return kBdBCA;
        case kMediaTagBlurayDds:
            return kBdDDS;
        case kMediaTagBlurayCartridgeStatus:
            return kBdCartridgeStatus;
        case kMediaTagBluraySpareArea:
            return kBdSpareArea;
        case kMediaTagAacsVolumeIdentifier:
            return kAACS_VolumeIdentifier;
        case kMediaTagAacsSerialNumber:
            return kAACS_SerialNumber;
        case kMediaTagAacsMediaIdentifier:
            return kAACS_MediaIdentifier;
        case kMediaTagAacsMkb:
            return kAACS_MKB;
        case kMediaTagAacsDataKeys:
            return kAACS_DataKeys;
        case kMediaTagAacsLbaExtents:
            return kAACS_LBAExtents;
        case kMediaTagCprmMkb:
            return kAACS_CPRM_MKB;
        case kMediaTagHybridRecognizedLayers:
            return kHybrid_RecognizedLayers;
        case kMediaTagMmcWriteProtection:
            return kMMC_WriteProtection;
        case kMediaTagMmcDiscInformation:
            return kMMC_DiscInformation;
        case kMediaTagMmcTrackResourcesInformation:
            return kMMC_TrackResourcesInformation;
        case kMediaTagMmcPowResourcesInformation:
            return kMMC_POWResourcesInformation;
        case kMediaTagScsiInquiry:
            return kSCSI_INQUIRY;
        case kMediaTagScsiModePage2A:
            return kSCSI_MODEPAGE_2A;
        case kMediaTagAtaIdentify:
            return kATA_IDENTIFY;
        case kMediaTagAtapiIdentify:
            return kATAPI_IDENTIFY;
        case kMediaTagPcmciaCis:
            return kPCMCIA_CIS;
        case kMediaTagSdCid:
            return kSecureDigital_CID;
        case kMediaTagSdCsd:
            return kSecureDigital_CSD;
        case kMediaTagSdScr:
            return kSecureDigital_SCR;
        case kMediaTagSdOcr:
            return kSecureDigital_OCR;
        case kMediaTagMmcCid:
            return kMMC_CID;
        case kMediaTagMmcCsd:
            return kMMC_CSD;
        case kMediaTagMmcOcr:
            return kMMC_OCR;
        case kMediaTagExtendedCsd:
            return kMMC_ExtendedCSD;
        case kMediaTagXboxSecuritySector:
            return kXbox_SecuritySector;
        case kMediaTagFloppyLeadOut:
            return kFloppy_LeadOut;
        case kMediaTagDiscControlBlock:
            return kDiscControlBlock;
        case kMediaTagCdFirstTrackPregap:
            return kCD_FirstTrackPregap;
        case kMediaTagCdLeadOut:
            return kCD_LeadOut;
        case kMediaTagScsiModeSense6:
            return kSCSI_MODESENSE_6;
        case kMediaTagScsiModeSense10:
            return kSCSI_MODESENSE_10;
        case kMediaTagUsbDescriptors:
            return kUSB_Descriptors;
        case kMediaTagXboxDmi:
            return kXbox_DMI;
        case kMediaTagXboxPfi:
            return kXbox_PFI;
        case kMediaTagCdLeadIn:
            return -1; /* no OBMAFS3 equivalent */
        case kMediaTagMiniDiscType:
            return kMiniDiscType;
        case kMediaTagMiniDiscD5:
            return kMiniDiscD5;
        case kMediaTagMiniDiscUtoc:
            return kMiniDiscUTOC;
        case kMediaTagMiniDiscDtoc:
            return kMiniDiscDTOC;
        case kMediaTagDvdDiscKeyDecrypted:
            return kDVD_DiscKey_Decrypted;
        case kMediaTagDvdPfi2ndLayer:
            return kDVD_PFI_2ndLayer;
        case kMediaTagFloppyWriteProtect:
            return kFloppy_WriteProtect;
        case kMediaTagWiiUPartitionKeyMap:
            return kWiiUPartitionKeyMap;
        case kMediaTagWiiPartitionKeyMap:
            return kWiiPartitionKeyMap;
        case kMediaTagNgcwJunkMap:
            return kNgcwJunkMap;
        default:
            return -1;
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
        case 1: /* Data */
        case 2: /* CdMode1 */
            return kCdSectorMode1;
        case 3: /* CdMode2Formless */
            return kCdSectorMode2;
        case 4: /* CdMode2Form1 */
            return kCdSectorMode2Form1;
        case 5: /* CdMode2Form2 */
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
