#ifndef OBMAFS3_ENUMS_H
#define OBMAFS3_ENUMS_H

/// Supported compression algorithms.
enum obmafs3_compression
{
    kCompressionNone = 0,
    kCompressionZstd = 1
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
    kBtreeTypeRefcount      = 10
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
    kBtreeDataTypeRefcountEntry      = 10
};

/// Inode file type stored in inode_record::file_type.
enum obmafs3_file_type
{
    kFileTypeRegular          = 0,
    kFileTypeDirectory        = 1,
    kFileTypeMediaImage       = 2,
    kFileTypeSymlink          = 3,
    kFileTypeCompactDiscImage = 4,
    kFileTypeSubchannelFile   = 5
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
    kQueryOpEqual      = 0, ///< strcmp == 0
    kQueryOpNotEqual   = 1, ///< strcmp != 0
    kQueryOpGreater    = 2, ///< strcmp > 0
    kQueryOpLess       = 3, ///< strcmp < 0
    kQueryOpGreaterEq  = 4, ///< strcmp >= 0
    kQueryOpLessEq     = 5, ///< strcmp <= 0
    kQueryOpContains   = 6, ///< strstr != NULL
    kQueryOpStartsWith = 7, ///< strncmp prefix == 0
    kQueryOpExists     = 8  ///< key exists, value ignored
};

/// How to combine multiple query filters.
enum obmafs3_query_combine
{
    kQueryCombineAnd = 0, ///< All filters must match
    kQueryCombineOr  = 1  ///< At least one filter must match
};

#endif /* OBMAFS3_ENUMS_H */