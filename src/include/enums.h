// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : enums.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Common enumerations for OBMAFS3.
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

#ifndef OBMAFS3_ENUMS_H
#define OBMAFS3_ENUMS_H

/// Supported compression algorithms.
enum obmafs3_compression
{
    kCompressionNone = 0,
    kCompressionZstd = 1,
    kCompressionLzma = 2
};

/// Supported checksum algorithms.
enum obmafs3_checksum_type
{
    kChecksumTypeXXH64 = 0
};

/// Identifies which B+Tree a header belongs to.
enum obmafs3_btree_type
{
    kBtreeTypeCatalog       = 0,
    kBtreeTypeInode         = 1,
    kBtreeTypeOverflow      = 2,
    kBtreeTypeDeduplication = 3,
    kBtreeTypeMetadata      = 4,
    kBtreeTypeMediaTag      = 5,
    kBtreeTypeCdPrefix      = 6,
    kBtreeTypeCdSuffix      = 7,
    kBtreeTypeCdSubchannel  = 8,
    kBtreeTypeMetadataIndex = 9,
    kBtreeTypeRefcount      = 10,
    kBtreeTypeSectorTagData = 11,
    kBtreeTypeSectorTagRef  = 12,
    kBtreeTypeJunkMap       = 13,
    kBtreeTypeMetadataNumericIndex = 14  ///< Numeric metadata reverse-index
};

/// Identifies the record format stored in a B+Tree.
enum obmafs3_btree_data_type
{
    kBtreeDataTypeFilename           = 0,
    kBtreeDataTypeInode              = 1,
    kBtreeDataTypeExtent             = 2,
    kBtreeDataTypeDeduplicationEntry = 3,
    kBtreeDataTypeMetadataEntry      = 4,
    kBtreeDataTypeMediaTagEntry      = 5,
    kBtreeDataTypeCdPrefixEntry      = 6,
    kBtreeDataTypeCdSuffixEntry      = 7,
    kBtreeDataTypeCdSubchannelEntry  = 8,
    kBtreeDataTypeMetadataIndexEntry = 9,
    kBtreeDataTypeRefcountEntry      = 10,
    kBtreeDataTypeSectorTagDataEntry  = 11,
    kBtreeDataTypeSectorTagRefEntry   = 12,
    kBtreeDataTypeJunkMapEntry        = 13,
    kBtreeDataTypeMetadataNumericIndexEntry = 14  ///< Numeric metadata index entry
};

/// Inode file type stored in inode_record::file_type.
enum obmafs3_file_type
{
    kFileTypeRegular          = 0,
    kFileTypeDirectory        = 1,
    kFileTypeMediaImage       = 2,
    kFileTypeSymlink          = 3,
    kFileTypeCompactDiscImage = 4,
    kFileTypeSubchannelFile   = 5,
    kFileTypeNintendo         = 6,
    kFileTypePS3Image         = 7
};

/// CD sector encoding mode.
enum obmafs3_cd_sector_mode
{
    kCdSectorModeAudio  = 0,
    kCdSectorMode1      = 1,
    kCdSectorMode2      = 2,
    kCdSectorMode2Form1 = 3,
    kCdSectorMode2Form2 = 4,
};

/// Metadata query comparison operators (lexicographic).
enum obmafs3_query_op
{
    kQueryOpEqual      = 0,  ///< strcmp == 0
    kQueryOpNotEqual   = 1,  ///< strcmp != 0
    kQueryOpGreater    = 2,  ///< strcmp > 0
    kQueryOpLess       = 3,  ///< strcmp < 0
    kQueryOpGreaterEq  = 4,  ///< strcmp >= 0
    kQueryOpLessEq     = 5,  ///< strcmp <= 0
    kQueryOpContains   = 6,  ///< strstr != NULL
    kQueryOpStartsWith = 7,  ///< strncmp prefix == 0
    kQueryOpExists     = 8,  ///< key exists, value ignored

    /* Case-insensitive variants */
    kQueryOpIEqual      = 9,  ///< strcasecmp == 0
    kQueryOpINotEqual   = 10, ///< strcasecmp != 0
    kQueryOpIContains   = 11, ///< case-insensitive substring
    kQueryOpIStartsWith = 12, ///< case-insensitive prefix

    /* Numeric comparisons (values parsed as int64_t) */
    kQueryOpNumEqual    = 13, ///< atoll == atoll
    kQueryOpNumNotEqual = 14, ///< atoll != atoll
    kQueryOpNumGreater  = 15, ///< atoll > atoll
    kQueryOpNumLess     = 16, ///< atoll < atoll
    kQueryOpNumGreaterEq = 17, ///< atoll >= atoll
    kQueryOpNumLessEq   = 18, ///< atoll <= atoll

    /* Regular expression matching (POSIX extended regex) */
    kQueryOpRegex       = 19, ///< regexec match (case-sensitive)
    kQueryOpIRegex      = 20, ///< regexec match (case-insensitive)

    /* Suffix matching */
    kQueryOpEndsWith    = 21, ///< suffix match (case-sensitive)
    kQueryOpIEndsWith   = 22, ///< suffix match (case-insensitive)

    /* Set membership (comma-separated value list) */
    kQueryOpIn          = 23, ///< value in comma-separated list (case-sensitive)
    kQueryOpIIn         = 24, ///< value in comma-separated list (case-insensitive)

    /* Range matching (value = "low,high", inclusive) */
    kQueryOpBetween     = 25, ///< low <= strcmp <= high (lexicographic)
    kQueryOpNumBetween  = 26, ///< low <= atoll <= high (numeric)

    /* Glob/wildcard matching (fnmatch) */
    kQueryOpGlob        = 27, ///< fnmatch pattern (case-sensitive)
    kQueryOpIGlob       = 28  ///< fnmatch pattern (case-insensitive)
};

/// How to combine multiple query filters.
enum obmafs3_query_combine
{
    kQueryCombineAnd = 0,  ///< All filters must match
    kQueryCombineOr  = 1   ///< At least one filter must match
};

#endif /* OBMAFS3_ENUMS_H */