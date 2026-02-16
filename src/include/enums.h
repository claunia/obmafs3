#ifndef OBMAFS3_ENUMS_H
#define OBMAFS3_ENUMS_H

enum obmafs3_compression {
    kCompressionNone = 0,
    kCompressionZstd = 1
};

enum obmafs3_checksum_type {
    kChecksumTypeXXH64 = 0
};

enum obmafs3_btree_type {
    kBtreeTypeCatalog       = 0,
    kBtreeTypeInode         = 1,
    kBtreeTypeOverflow      = 2,
    kBtreeTypeDeduplication = 3,
    kBtreeTypeMetadata      = 4,
    kBtreeTypeMediaTag      = 5
};

enum obmafs3_btree_data_type {
    kBtreeDataTypeFilename           = 0,
    kBtreeDataTypeInode              = 1,
    kBtreeDataTypeExtent             = 2,
    kBtreeDataTypeDeduplicationEntry = 3,
    kBtreeDataTypeMetadataEntry      = 4,
    kBtreeDataTypeMediaTagEntry      = 5
};

enum obmafs3_file_type {
    kFileTypeRegular    = 0,
    kFileTypeDirectory  = 1,
    kFileTypeMediaImage = 2
};

#endif /* OBMAFS3_ENUMS_H */