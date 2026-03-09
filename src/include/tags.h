// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : tags.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 on-disk tags structure.
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

#ifndef OBMAFS3_TAGS_H
#define OBMAFS3_TAGS_H

typedef enum
{
    kCdTableOfContents             = 0,   ///< Standard CD Table Of Contents (lead-in, first session)
    kCdSessionInfo                 = 1,   ///< Per-session summary (start/end addresses, track count)
    kCdFullTOC                     = 2,   ///< Complete multi-session TOC including hidden tracks
    kCdPMA                         = 3,   ///< Program Memory Area (temporary track info before finalization)
    kCdATIP                        = 4,   ///< Absolute Time In Pregroove (writable media timing & power metadata)
    kCdTEXT                        = 5,   ///< CD-Text blocks (titles, performers, etc.)
    kCdMCN                         = 6,   ///< Media Catalogue Number (EAN/UPC style identifier)
    kDvdPFI                        = 7,   ///< Physical Format Information (layer geometry & book type)
    kDvdCMI                        = 8,   ///< Copyright Management Information (CSS/CPRM flags)
    kDvdDiscKey                    = 9,   ///< Encrypted disc key block (CSS)
    kDvdBCA                        = 10,  ///< Burst Cutting Area (etched manufacturer / AACS info)
    kDvdDMI                        = 11,  ///< Disc Manufacturer Information (lead-in descriptor)
    kDvdMediaIdentifier            = 12,  ///< Writable media dye / manufacturer ID
    kDvdMKB                        = 13,  ///< Media Key Block (AACS/DVD)
    kDvdRamDDS                     = 14,  ///< Defect Data Structure (DVD-RAM mapping)
    kDvdRamMediumStatus            = 15,  ///< Medium Status (allocated spare info)
    kDvdRamSpareArea               = 16,  ///< Spare area descriptors
    kDvdRecordableRMD              = 17,  ///< Recorded Media Data (RMD) last border-out
    kDvdRecordablePreRecordedInfo  = 18,  ///< Pre-recorded info area (lead-in)
    kDvdRecordableMediaIdentifier  = 19,  ///< DVD-R/-RW writable media identifier
    kDvdRecordablePFI              = 20,  ///< DVD-R physical format (layer data)
    kDvdADIP                       = 21,  ///< Address In Pregroove (DVD+ / wobble timing)
    kHdDvdCPI                      = 22,  ///< Content Protection Info (HD DVD)
    kHdDvdMediumStatus             = 23,  ///< HD DVD Medium status (spares/defects)
    kDvdDlLayerCapacity            = 24,  ///< Dual layer capacity & break info
    kDvdDlMiddleZoneAddress        = 25,  ///< Middle zone start LBA
    kDvdDlJumpIntervalSize         = 26,  ///< Jump interval size (opposite track path)
    kDvdDlManualLayerJumpLBA       = 27,  ///< Manual layer jump LBA (OTP)
    kBdDI                          = 28,  ///< Disc Information (BD)
    kBdBCA                         = 29,  ///< Blu-ray Burst Cutting Area
    kBdDDS                         = 30,  ///< Disc Definition Structure (recordable)
    kBdCartridgeStatus             = 31,  ///< Cartridge presence / write protect (BD-RE/BD-R in caddy)
    kBdSpareArea                   = 32,  ///< BD spare area allocation map
    kAACS_VolumeIdentifier         = 33,  ///< AACS Volume Identifier
    kAACS_SerialNumber             = 34,  ///< Pre-recorded media serial number (AACS)
    kAACS_MediaIdentifier          = 35,  ///< AACS Media Identifier (unique per disc)
    kAACS_MKB                      = 36,  ///< AACS Media Key Block
    kAACS_DataKeys                 = 37,  ///< Extracted AACS title/volume keys (when decrypted)
    kAACS_LBAExtents               = 38,  ///< LBA extents requiring bus encryption
    kAACS_CPRM_MKB                 = 39,  ///< CPRM Media Key Block
    kHybrid_RecognizedLayers       = 40,  ///< Hybrid disc recognized layer combinations (e.g. CD/DVD/BD)
    kMMC_WriteProtection           = 41,  ///< Write protection status (MMC GET CONFIG)
    kMMC_DiscInformation           = 42,  ///< Disc Information (recordable status, erasable, last session)
    kMMC_TrackResourcesInformation = 43,  ///< Track Resources (allocated/open track data)
    kMMC_POWResourcesInformation   = 44,  ///< Pseudo OverWrite resources (BD-R POW)
    kSCSI_INQUIRY                  = 45,  ///< SCSI INQUIRY standard data (SPC-*)
    kSCSI_MODEPAGE_2A              = 46,  ///< SCSI Mode Page 2Ah (CD/DVD capabilities)
    kATA_IDENTIFY                  = 47,  ///< ATA IDENTIFY DEVICE (512 bytes)
    kATAPI_IDENTIFY                = 48,  ///< ATA PACKET IDENTIFY DEVICE
    kPCMCIA_CIS                    = 49,  ///< PCMCIA/CardBus CIS tuple chain
    kSecureDigital_CID             = 50,  ///< SecureDigital Card ID register
    kSecureDigital_CSD             = 51,  ///< SecureDigital Card Specific Data
    kSecureDigital_SCR             = 52,  ///< SecureDigital Configuration Register
    kSecureDigital_OCR             = 53,  ///< SecureDigital Operation Conditions (voltage)
    kMMC_CID                       = 54,  ///< MMC Card ID
    kMMC_CSD                       = 55,  ///< MMC Card Specific Data
    kMMC_OCR                       = 56,  ///< MMC Operation Conditions
    kMMC_ExtendedCSD               = 57,  ///< MMC Extended CSD (512 bytes)
    kXbox_SecuritySector           = 58,  ///< Xbox/Xbox 360 Security Sector (SS.bin)
    kFloppy_LeadOut                = 59,  ///< Manufacturer / duplication cylinder (floppy special data)
    kDiscControlBlock              = 60,  ///< DVD Disc Control Blocks
    kCD_FirstTrackPregap           = 61,  ///< First track pregap (index 0)
    kCD_LeadOut                    = 62,  ///< Lead-out area contents
    kSCSI_MODESENSE_6              = 63,  ///< Raw MODE SENSE (6) data
    kSCSI_MODESENSE_10             = 64,  ///< Raw MODE SENSE (10) data
    kUSB_Descriptors               = 65,  ///< Concatenated USB descriptors (device/config/interface)
    kXbox_DMI                      = 66,  ///< Xbox Disc Manufacturing Info (DMI)
    kXbox_PFI                      = 67,  ///< Xbox Physical Format Information (PFI)
    kMiniDiscType                  = 69,  ///< 8 bytes response that seems to define type of MiniDisc
    kMiniDiscD5                    = 70,  ///< 4 bytes response to vendor command D5h
    kMiniDiscUTOC = 71,  ///< User TOC, contains fragments, track names, and can be from 1 to 3 sectors of 2336 bytes
    kMiniDiscDTOC = 72,  ///< Not entirely clear kind of TOC that only appears on MD-DATA discs
    kDVD_DiscKey_Decrypted = 73,  ///< Decrypted DVD disc key,
    kDVD_PFI_2ndLayer      = 74,  ///< DVD Physical Format Information for the second layer
    kFloppy_WriteProtect   = 75,  ///< Write protection status of the floppy disk
    kNintendoWiiUDiscKey   = 76,  ///< Nintendo Wii U disc key (16 bytes, from non-readable disc area)
    kPS3DiscKey            = 77,  ///< PS3 derived disc key (16 bytes)
    kPS3D1                 = 78,  ///< PS3 data1 key (16 bytes, from disc)
    kPS3D2                 = 79,  ///< PS3 data2 key (16 bytes, from disc)
    kPS3PIC                = 80,  ///< PS3 PIC data (115 bytes, from disc lead-in)
    kPS3EncryptionMap      = 81,  ///< PS3 encryption region map (serialized from sector 0)
    kMaxMediaTag           = kPS3EncryptionMap
} Obmafs3MediaTagType;

#endif /* OBMAFS3_TAGS_H */