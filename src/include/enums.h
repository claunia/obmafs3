enum {
    kCompressionNone = 0,
    kCompressionZstd = 1
};

enum {
    kChecksumTypeXXH64 = 0
};

enum {
    kBtreeTypeCatalog = 0,
    kBtreeTypeInode = 1,
    kBtreeTypeOverflow = 2,
    kBtreeTypeDeduplication = 3,
    kBtreeTypeMetadata = 4,
    kBtreeTypeMediaTag = 5
};

enum {
    kBtreeDataTypeFilename = 0,
    kBtreeDataTypeInode = 1,
    kBtreeDataTypeExtent = 2,
    kBtreeDataTypeDeduplicationEntry = 3,
    kBtreeDataTypeMetadataEntry = 4,
    kBtreeDataTypeMediaTagEntry = 5
};

enum {
    kFileTypeRegular = 0,
    kFileTypeDirectory = 1,
    kFileTypeMediaImage = 2
};