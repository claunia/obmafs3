# Design of OBMAFS v3

OBMAFS v3 is a filesystem designed for the deduplicated storage of disk image files. It deduplicates data at the sector level of images stored inside it, while still allowing efficient random access to the data. It can also store accompanying metadata files alongside images and index metadata about the images themselves.

All multi-byte values are stored **little-endian**. All on-disk structures use `__attribute__((packed))` to eliminate padding.

---

## On-Disk Layout

| Block(s)           | Content                              | Magic             |
|---------------------|--------------------------------------|--------------------|
| 0                   | Superblock                           | `OBMAFS_3` (0x335F5346414D424F) |
| 1                   | Catalog B+Tree header                | `BTREEHDR` (0x5244484545525442) |
| 2                   | Catalog root node (root dir entry)   | `BTREENDE` (0x45444E4545525442) |
| 3                   | Inode B+Tree header                  | `BTREEHDR` |
| 4                   | Root directory inode                 | `BTREENDE` |
| 5                   | Overflow B+Tree header               | `BTREEHDR` |
| 6                   | Dedup tree list header               | `TREELIST` (0x5453494C45455254) |
| 7                   | Media Tag B+Tree header              | `BTREEHDR` |
| 8                   | CD Prefix B+Tree header              | `BTREEHDR` |
| 9                   | CD Suffix B+Tree header              | `BTREEHDR` |
| 10                  | CD Subchannel B+Tree header          | `BTREEHDR` |
| 11                  | Metadata B+Tree header               | `BTREEHDR` |
| 12                  | Metadata Index B+Tree header         | `BTREEHDR` |
| 13                  | Refcount B+Tree header               | `BTREEHDR` |
| 14 .. 14+N-1        | Allocation bitmap                    | `OBMABMAP` (0x50414D42414D424F) |
| 14+N ..             | Data blocks, B+Tree nodes, dedup data| `OBMABLCK` / `BTREENDE` |
| total_blocks − 1    | Backup superblock                    | `OBMAFS_3` (0x335F5346414D424F) |

The block size for regular data (catalog, inode, overflow, file data) defaults to **4096 bytes**.
The block size for deduplicated data defaults to **4 194 304 bytes** (4 MiB = 1024 standard blocks).

---

## Superblock

```c
struct obmafs3_sb {                          /* packed, all fields little-endian */
    uint64_t magic;              /* "OBMAFS_3" (0x335F5346414D424F) */
    uint8_t  guid[16];           /* Unique identifier for the filesystem instance */
    uint64_t block_size;         /* Size of each block for non-deduplicated data */
    uint64_t dedup_block_size;   /* Size of each block for deduplicated data */
    uint64_t total_bytes;        /* Total size of the filesystem in bytes */
    uint64_t catalog_lba;        /* LBA of the catalog B+Tree header (block 1) */
    uint64_t inode_lba;          /* LBA of the inode B+Tree header (block 3) */
    uint64_t overflow_lba;       /* LBA of the overflow B+Tree header (block 5) */
    uint64_t dedup_lba;          /* LBA of the dedup tree list header (block 6) */
    uint64_t metadata_lba;       /* LBA of the metadata B+Tree header (block 11) */
    uint64_t media_tag_lba;      /* LBA of the media tag B+Tree header (block 7) */
    uint64_t cd_prefix_lba;      /* LBA of the CD prefix B+Tree header (block 8) */
    uint64_t cd_suffix_lba;      /* LBA of the CD suffix B+Tree header (block 9) */
    uint64_t cd_subchannel_lba;  /* LBA of the CD subchannel B+Tree header (block 10) */
    uint64_t metadata_idx_lba;   /* LBA of the metadata reverse-index B+Tree header (block 12) */
    uint64_t refcount_lba;       /* LBA of the refcount B+Tree header (block 13) */
    uint16_t checksum_type;      /* Checksum algorithm (0 = XXH64) */
    uint64_t creation_time;      /* Unix timestamp of filesystem creation */
    uint64_t next_inode_id;      /* Next available inode ID */
    uint64_t bitmap_lba;         /* LBA of the first allocation bitmap block (block 14) */
    uint64_t bitmap_blocks;      /* Number of blocks used by the allocation bitmap */
    uint8_t  volume_label[256];  /* Volume label, UTF-8, NUL-terminated */
    uint8_t  checksum[32];       /* Checksum of the superblock (XXH64, 8 bytes used, 24 zeroed) */
};
```

The superblock identifies the filesystem, stores global parameters, and provides the LBAs for all top-level structures. The root inode ID is always 2 (`OBMAFS3_ROOT_INODE_ID`), and `next_inode_id` starts at 3 after creation.

A byte-identical **backup copy** of the superblock is stored at the last block of the filesystem (`LBA = total_blocks − 1`). The backup is written every time the primary superblock is updated. If the primary superblock is unreadable or has invalid magic, `obmafs3_open()` and `obmafsck` automatically fall back to the backup, probing 8 candidate block sizes (4096, 512, 1024, 2048, 8192, 16384, 32768, 65536) since the block size is stored inside the superblock itself. The backup block is marked as allocated in the allocation bitmap.

**Field descriptions:**

- `catalog_lba` — LBA of the Catalog Tree header, a B+Tree that maps (parent_id, name) pairs to inode IDs, providing the directory structure.
- `inode_lba` — LBA of the Inode Tree header, a B+Tree keyed by inode_id that stores file metadata (permissions, timestamps, size, extents).
- `overflow_lba` — LBA of the Overflow Tree header, a B+Tree that stores additional extents for files requiring more than 8 inline extent runs.
- `dedup_lba` — LBA of the Deduplication Tree List header, a list of per-sector-size B+Trees that map sector hashes to their physical locations.
- `metadata_lba` — LBA of the Metadata Tree header, a B+Tree that stores arbitrary key-value pairs about disk images, keyed by `(inode_id, key)`.
- `media_tag_lba` — LBA of the Media Tag Tree header, a B+Tree that stores binary media tags (TOC, PMA, BCA, etc.) keyed by `(inode_id, tag_type)`.
- `cd_prefix_lba` — LBA of the CD Prefix Tree header, a B+Tree that stores 16-byte CD sector prefixes keyed by XXH64 hash.
- `cd_suffix_lba` — LBA of the CD Suffix Tree header, a B+Tree that stores 288-byte CD sector suffixes (ECC/EDC) keyed by XXH64 hash.
- `cd_subchannel_lba` — LBA of the CD Subchannel Tree header, a B+Tree that stores 96-byte CD subchannel data keyed by XXH64 hash.
- `metadata_idx_lba` — LBA of the Metadata Index Tree header, a reverse-index B+Tree keyed by `(key, value, inode_id)` for metadata queries.
- `refcount_lba` — LBA of the Refcount Tree header, a B+Tree that tracks per-block reference counts for shared (cloned) data blocks.
- `bitmap_lba`, `bitmap_blocks` — Location and size of the allocation bitmap on disk.

---

## Allocation Bitmap

The allocation bitmap tracks which blocks are in use. Each bit corresponds to one standard block (defined by `block_size`). A bit value of 1 means allocated, 0 means free. Deduplication blocks that span multiple standard blocks (when `dedup_block_size` is a multiple of `block_size`, e.g. 1024 blocks for default sizes) occupy that many consecutive bits.

The first bitmap block begins with a header:

```c
struct bitmap_header {                       /* packed */
    uint64_t magic;          /* "OBMABMAP" (0x50414D42414D424F) */
    uint64_t total_blocks;   /* Total number of blocks tracked by the bitmap */
    uint64_t next_free_lba;  /* Allocation hint: next LBA to try for allocation */
    uint8_t  checksum[32];   /* Checksum of all bitmap data bytes */
};
```

The bitmap data follows immediately after the header in the first block. If the bitmap exceeds a single block (after accounting for the header), it continues in subsequent blocks which contain only bitmap data with no additional headers.

| Block | Content |
|---|---|
| `bitmap_lba` | `bitmap_header` + bitmap data bytes |
| `bitmap_lba + 1` | bitmap data bytes (continuation) |
| ... | ... |
| `bitmap_lba + N-1` | bitmap data bytes (last block, may be partially filled) |

The checksum is recomputed and written every time the bitmap is persisted to disk, and verified when the bitmap is loaded at mount time. When the filesystem is created, blocks reserved for the superblock, tree headers, root nodes, and the bitmap itself are marked as allocated. All remaining blocks start as free.

---

## B+Tree Structures

All B+Tree structures (Catalog, Inode, Overflow, Deduplication, Metadata, Media Tag) share a common header and node format.

### B+Tree Header

```c
struct btree_header {                        /* packed */
    uint64_t magic;          /* "BTREEHDR" (0x5244484545525442) */
    uint32_t data_type;      /* Type of data stored (see btree_data_type enum) */
    uint64_t root_node_lba;  /* LBA of the root node */
    uint64_t free_node_lba;  /* LBA of the first free node (for reuse) */
    uint16_t node_size;      /* Size of each node in bytes */
    uint32_t total_nodes;    /* Total number of nodes in the tree */
    uint32_t free_nodes;     /* Number of free nodes available for reuse */
    uint32_t tree_type;      /* Purpose of tree (see btree_type enum) */
    uint64_t last_block_lba;    /* Dedup only: LBA of partially filled data block */
    uint64_t last_block_offset; /* Dedup only: byte offset for next write */
    uint8_t  checksum[32];   /* Checksum of this header block */
};
```

The `last_block_lba` / `last_block_offset` fields are only meaningful for deduplication trees, where they track the current partially filled dedup data block so that future imports with the same sector size can continue appending without wasting space.

### B+Tree Node Header

```c
struct btree_node_header {                   /* packed, 70 bytes */
    uint64_t magic;          /* "BTREENDE" (0x45444E4545525442) */
    uint8_t  record_type;    /* Type of records in this node */
    uint8_t  level;          /* 0 = leaf node, >0 = index node (B+Tree depth) */
    uint64_t left_link;      /* LBA of left sibling node */
    uint64_t right_link;     /* LBA of right sibling node */
    uint64_t overflow_link;  /* LBA of overflow node (when capacity exceeded) */
    uint16_t node_keys;      /* Number of keys currently stored */
    uint16_t keys_length;    /* Total byte length of all keys */
    uint8_t  checksum[32];   /* Checksum of this node block */
};
```

Records are stored in sorted order within each node. Sibling nodes are linked via `left_link` / `right_link` for ordered traversal. Overflow nodes handle the case where a node exceeds its key capacity.

### Enumerations

```c
enum obmafs3_btree_type {
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

enum obmafs3_btree_data_type {
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

enum obmafs3_file_type {
    kFileTypeRegular          = 0,
    kFileTypeDirectory        = 1,
    kFileTypeMediaImage       = 2,
    kFileTypeSymlink          = 3,
    kFileTypeCompactDiscImage = 4
};

enum obmafs3_compression {
    kCompressionNone = 0,
    kCompressionZstd = 1
};

enum obmafs3_checksum_type {
    kChecksumTypeXXH64 = 0
};

enum obmafs3_cd_sector_mode {
    kCdSectorModeAudio  = 0,
    kCdSectorMode1      = 1,
    kCdSectorMode2      = 2,
    kCdSectorMode2Form1 = 3,
    kCdSectorMode2Form2 = 4
};
```

### Catalog Tree (directory entries)

The catalog tree maps `(parent_id, name)` pairs to inode IDs. `btree_node_filename` wraps a single record for lookup results; leaf nodes store packed `catalog_record` entries. Index nodes use `catalog_index_entry` with the full composite key.

```c
struct btree_node_filename {                 /* packed */
    struct btree_node_header header;
    uint64_t inode_id;       /* Unique identifier for the file or directory */
    uint64_t parent_id;      /* Identifier of the parent directory (root = 2) */
    uint8_t  directory_flag; /* 1 if directory, 0 if file */
    char     name[256];      /* Name in UTF-8, NUL-terminated */
};

struct catalog_record {                      /* packed, leaf payload */
    uint64_t inode_id;       /* Unique identifier for the file or directory */
    uint64_t parent_id;      /* Identifier of the parent directory */
    uint8_t  directory_flag; /* 1 if directory, 0 if file */
    char     name[256];      /* Name in UTF-8, NUL-terminated */
};

struct catalog_index_entry {                 /* packed, index payload */
    uint64_t parent_id;  /* Smallest parent_id reachable through child */
    char     name[256];  /* Smallest name reachable through child */
    uint64_t child_lba;  /* LBA of the child node */
};
```

### Inode Tree (file metadata)

The inode tree is a proper B+Tree: leaf nodes (level 0) store packed `inode_record` entries sorted by `inode_id`, and index nodes (level > 0) store `btree_index_entry` entries pointing to child nodes. Each leaf node can hold up to `(block_size - sizeof(btree_node_header)) / sizeof(inode_record)` records (15 records for a 4096-byte block).

```c
struct inode_record {                        /* packed, 265 bytes */
    uint64_t inode_id;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;               /* POSIX file permissions */
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t access_time;
    uint64_t file_size;          /* File size in bytes */
    struct extent_run extents[8];/* Up to 8 inline extent runs */
    uint8_t  file_type;          /* 0=regular, 1=dir, 2=media image, 3=symlink, 4=CD image */
    uint64_t sector_count;       /* Media images: total number of sectors */
    uint64_t sector_map_size;    /* Media images: number of sector_map_entries written */
    uint32_t ref_count;          /* Number of hardlinks (catalog entries) pointing to this inode */
};
```

The `extents` array holds up to 8 inline extent runs. For regular files, these point to data blocks. For media image files, they point to blocks containing a flat array of `sector_map_entry` structures that map each sector to its deduplicated copy. The `sector_count` and `sector_map_size` fields are only used for media image files.

#### Hardlinks

Files support hardlinks: multiple catalog entries can point to the same inode. The `ref_count` field tracks the number of catalog entries referencing each inode. When a hardlink is created, a new catalog entry is added pointing to the existing inode and the reference count is incremented. When a file is unlinked, the catalog entry is removed and the reference count is decremented; the inode and its data blocks are only freed when the reference count reaches zero.

Directories do not support hardlinks. A directory's inode is deleted only when the directory is empty (no children in the catalog) and its single catalog entry is removed.

```c
struct extent_run {                          /* packed, 24 bytes */
    uint64_t start_block;    /* Starting physical LBA of the extent run */
    uint64_t block_count;    /* Number of physical blocks */
    uint64_t logical_blocks; /* Number of logical blocks this extent covers */
};
```

When `logical_blocks == block_count` the data is stored uncompressed (one physical block per logical block). When `logical_blocks > block_count` the physical blocks contain a `block_header` followed by ZSTD-compressed data covering `logical_blocks × block_size` bytes.
```

If a file requires more than 8 extents, additional extents are stored in the Overflow Tree.

### Overflow Tree (extra extents)

The Overflow Tree is a B+Tree that stores additional extent runs for files that exceed the 8 inline extents available in the inode. Leaf nodes contain sorted `overflow_extent` records; index nodes use `btree_index_entry` to route lookups by `inode_id`.

```c
struct overflow_extent {                     /* packed, 40 bytes */
    uint64_t inode_id;       /* Inode this extent belongs to */
    uint64_t logical_offset; /* First logical block covered (sort key) */
    uint64_t start_block;    /* Starting physical LBA of the extent run */
    uint64_t block_count;    /* Number of physical blocks in the extent run */
    uint64_t logical_count;  /* Number of logical blocks this extent covers */
};
```

Overflow entries are sorted by the composite key `(inode_id, logical_offset)`. Maximum records per leaf node with a 4096-byte block: (4096 − 70) / 40 = **100 entries**. Maximum index entries per node: (4096 − 70) / 16 = **251 entries**.

When reading file data, the system first uses the 8 inline extents from the inode, then traverses the overflow B+Tree to find any additional extents for that inode. The `level` field in the node header distinguishes index nodes (`level > 0`) from leaf nodes (`level == 0`).

### Refcount Tree (block reference counts)

The Refcount Tree is a B+Tree keyed by block LBA that tracks the reference count for data blocks shared between files via `FICLONERANGE`. Only blocks with a reference count greater than 1 are stored; blocks absent from the tree have an implicit reference count of 1.

```c
struct refcount_record {                     /* packed, 12 bytes */
    uint64_t lba;            /* Block LBA */
    uint32_t ref_count;      /* Number of inodes sharing this block */
};
```

Entries are sorted by `lba`. Maximum records per leaf node with a 4096-byte block: (4096 − 70) / 12 = **335 entries**. Maximum index entries per node: (4096 − 70) / 16 = **251 entries**.

When a block range is cloned from one file to another, the refcount for each shared block is incremented (creating a new entry with refcount 2 if none existed). When a file is truncated or deleted, blocks with refcount > 1 have their refcount decremented instead of being freed; blocks whose refcount drops to 1 have their entry removed from the tree. The write path checks the refcount before modifying a shared block and performs copy-on-write (allocating a new block) when the refcount is greater than 1.

At runtime, the `obmafs3_ctx` maintains a **single-leaf refcount cache** (`rc_leaf_buf`, `rc_leaf_lba`, `rc_leaf_min`, `rc_leaf_max`, `rc_leaf_count`, `rc_leaf_valid`) that avoids repeated B+Tree traversals when incrementing or decrementing refcounts for blocks within the same leaf node. The cache is protected by the write lock.

### Deduplication Tree (sector hash lookup)

The deduplication tree is a B+Tree keyed by the XXH64 hash of sector data. Leaf nodes store sorted `dedup_entry` records; index nodes use the standard `btree_index_entry` to route lookups by hash key.

```c
struct dedup_entry {                         /* packed, 24 bytes */
    uint64_t hash;           /* XXH64 hash of the sector data */
    uint64_t block_lba;      /* LBA of the dedup data block containing this sector */
    uint64_t block_offset;   /* Byte offset within the block where sector data starts */
};
```

Entries are sorted by `hash` and stored in leaf nodes (`level == 0`). Maximum records per leaf node with a 4096-byte block: (4096 − 70) / 24 = **167 entries**. Maximum index entries per node: (4096 − 70) / 16 = **251 entries**.

Lookups traverse from root through index nodes (binary search on `hash` key) to the target leaf, then binary-search the leaf entries. Insertions follow the same path, performing sorted insertion in the leaf and splitting upward when full — identical to the inode and overflow trees.

### Media Tag Tree (binary media tags)

The Media Tag Tree is a B+Tree that stores media-specific binary tags associated with disk images (e.g., CD TOC, DVD PFI, Blu-ray disc info). Leaf nodes contain sorted `media_tag_record` entries; index nodes use `media_tag_index_entry` to route lookups by the composite key `(inode_id, tag_type)`.

```c
#define MEDIA_TAG_INLINE_MAX  512
#define MEDIA_TAG_FLAG_INLINE 0x01

struct media_tag_record {                    /* packed */
    uint64_t inode_id;                           /* Inode this tag belongs to */
    uint16_t tag_type;                           /* MediaTagType enum value */
    uint32_t data_length;                        /* Total length of tag data in bytes */
    uint8_t  flags;                              /* MEDIA_TAG_FLAG_INLINE if data is inline */
    uint64_t data_lba;                           /* LBA of external data blocks (0 if inline) */
    uint64_t data_blocks;                        /* Number of external blocks (0 if inline) */
    uint8_t  inline_data[MEDIA_TAG_INLINE_MAX];  /* Inline data storage (512 bytes) */
};

struct media_tag_index_entry {               /* packed, index payload */
    uint64_t inode_id;   /* Smallest inode_id reachable through child */
    uint16_t tag_type;   /* Smallest tag_type reachable through child */
    uint64_t child_lba;  /* LBA of the child node */
};
```

Tags up to `MEDIA_TAG_INLINE_MAX` (512) bytes are stored inline (indicated by `flags & MEDIA_TAG_FLAG_INLINE`). Larger tags are stored in separately allocated blocks referenced by `data_lba` and `data_blocks`.

Tag types include: CD TOC, CD session info, CD full TOC, CD PMA, CD ATIP, CD-TEXT, CD MCN, DVD PFI, DVD CMI, DVD disc key, DVD BCA, DVD DMI, and many others.

### Metadata B+Tree (image key-value pairs)

The Metadata B+Tree stores arbitrary key-value string pairs associated with disk images (e.g., dumper name, dump date, serial number). Uses 8-block nodes (`METADATA_NODE_BLOCKS = 8`) because records are large.

```c
#define METADATA_KEY_MAX     256  /* 255 chars + NUL */
#define METADATA_VALUE_MAX   1025 /* 1024 chars + NUL */
#define METADATA_NODE_BLOCKS 8    /* Blocks per metadata tree node */

struct metadata_record {                     /* packed */
    uint64_t inode_id;                   /* Inode this entry belongs to */
    char     key[256];                   /* Metadata key (NUL-terminated, max 255 chars) */
    char     value[1025];                /* Metadata value (NUL-terminated, max 1024 chars) */
};

struct metadata_index_entry {                /* packed, index payload */
    uint64_t inode_id;               /* Smallest inode_id reachable through child */
    char     key[256];               /* Smallest key reachable through child */
    uint64_t child_lba;              /* LBA of the child node */
};
```

Entries are sorted by the composite key `(inode_id, key)`. Index nodes use `metadata_index_entry` (inode_id + key + child_lba).

A companion **Metadata Index B+Tree** (reverse-index) enables queries such as "find all images dumped by a specific person". It is keyed by `(key, value, inode_id)`:

```c
struct metadata_idx_record {                 /* packed */
    char     key[256];                   /* Metadata key */
    char     value[1025];                /* Metadata value */
    uint64_t inode_id;                   /* Disk image inode */
};

struct metadata_idx_index_entry {            /* packed, index payload */
    char     key[256];               /* Smallest key reachable through child */
    char     value[1025];            /* Smallest value reachable through child */
    uint64_t inode_id;               /* Smallest inode_id reachable through child */
    uint64_t child_lba;              /* LBA of the child node */
};
```

Both metadata trees use 8-block nodes and support full CRUD operations plus paginated key listing and reverse queries.

### CD Prefix / Suffix / Subchannel B+Trees

Three B+Trees store deduplicated CD raw sector components. All three share identical logic: a `uint64_t` hash key with fixed-size inline data.

```c
#define CD_PREFIX_DATA_SIZE     16
#define CD_SUFFIX_DATA_SIZE     288
#define CD_SUBCHANNEL_DATA_SIZE 96

struct cd_prefix_record {                    /* packed, 24 bytes */
    uint64_t hash;           /* XXH64 hash of the 16-byte prefix */
    uint8_t  data[16];       /* CD sector prefix data */
};

struct cd_suffix_record {                    /* packed, 296 bytes */
    uint64_t hash;           /* XXH64 hash of the 288-byte suffix */
    uint8_t  data[288];      /* CD sector suffix (ECC/EDC) data */
};

struct cd_subchannel_record {                /* packed, 104 bytes */
    uint64_t hash;           /* XXH64 hash of the 96-byte subchannel */
    uint8_t  data[96];       /* CD subchannel data */
};
```

Maximum leaf records per 4096-byte block: **167** (prefix), **13** (suffix), **38** (subchannel). All three trees support multi-level indexing with standard `btree_index_entry` nodes.

---

## Sector Map

Media image files store a flat array of `sector_map_entry` structures in their inode's data extents. There is one entry per sector in the disk image:

```c
struct sector_map_entry {                    /* packed, 18 bytes */
    int64_t  sector;         /* Logical sector number within the disk image */
    uint16_t sector_size;    /* Size of the sector in bytes (e.g. 512, 2048, 4096) */
    uint64_t hash;           /* XXH64 hash of the sector data */
};
```

To read a sector from the image, the system:
1. Reads the `sector_map_entry` from the inode's data extents at `sector_num * sizeof(sector_map_entry)`.
2. Looks up the `hash` in the appropriate dedup tree to get the `dedup_entry`.
3. Reads the dedup data block at `dedup_entry.block_lba` and extracts the sector data at `dedup_entry.block_offset`.

### CD Sector Map

CD (Compact Disc) images use an extended sector map entry that also tracks the sector's raw components — prefix, suffix, subchannel, and subheader — for lossless reconstruction of raw 2352/2448-byte sectors:

```c
struct cd_sector_map_entry {                 /* packed */
    int64_t  sector;             /* Logical sector number within the CD image */
    uint16_t sector_size;        /* Size (e.g. 2048, 2336, 2352) */
    uint64_t hash;               /* XXH64 hash of the CD data portion */
    uint8_t  generated_prefix;   /* 1 if prefix can be regenerated from LBA */
    uint64_t prefix_hash;        /* XXH64 hash of the 16-byte prefix */
    uint8_t  generated_suffix;   /* 1 if suffix (ECC/EDC) can be regenerated */
    uint64_t suffix_hash;        /* XXH64 hash of the 288-byte suffix */
    uint64_t subchannel_hash;    /* XXH64 hash of 96-byte subchannel (0 = not stored) */
    uint8_t  subheader[8];       /* Subheader for CD-ROM XA sectors (0 if N/A) */
    uint8_t  sector_mode;        /* Audio, Mode 1, Mode 2 Form 1/2, etc. */
};
```

When `generated_prefix` or `generated_suffix` is 1, the respective data is not stored in the CD prefix/suffix B+Trees but is instead regenerated from the sector's LBA and mode using the ECC/EDC engine (`ecc_cd_reconstruct`). This is a common case since most CD sectors have predictable sync/header bytes and valid ECC, avoiding storage overhead.

---

## CD Sector ECC/EDC Reconstruction

The `ecc_cd` module provides CD-ROM sector error correction code (EDC/ECC) computation and verification. It enables:

- **Suffix verification** (`ecc_cd_is_suffix_correct`) — checks whether a sector's 288-byte suffix (ECC+EDC) matches the data, determining if it can be regenerated rather than stored.
- **Prefix reconstruction** (`ecc_cd_reconstruct_prefix`) — regenerates the 16-byte sync/header prefix from a sector's LBA and mode bytes.
- **Full reconstruction** (`ecc_cd_reconstruct`) — regenerates the complete ECC/EDC suffix from the data portion.

This enables significant storage savings for CD images: only sectors with non-standard or corrupted ECC/EDC need their suffix stored in the CD Suffix B+Tree.

---

## Deduplication Tree List

The dedup tree list is stored at `dedup_lba` and contains headers for multiple per-sector-size B+Trees:

```c
struct tree_list_header {                    /* packed */
    uint64_t magic;          /* "TREELIST" (0x5453494C45455254) */
    uint64_t tree_count;     /* Number of B+Trees in the list */
    uint8_t  checksum[32];   /* Checksum of this header block */
    /* Followed by tree_count × tree_list_entry */
};

struct tree_list_entry {                     /* packed, 10 bytes */
    uint16_t sector_size;    /* Sector size this tree handles (e.g. 512, 2048) */
    uint64_t tree_lba;       /* LBA of the B+Tree header for this sector size */
};
```

When a new media image is imported with a sector size not yet in the list, a new B+Tree is created and a new entry is appended to the list.

---

## The Deduplication Process

When a file is written to the filesystem:

1. **Type detection**: If the file is detected as a disk image (by extension or explicit API flag), it is stored as a `kFileTypeMediaImage`. Otherwise it is stored as `kFileTypeRegular`.

2. **Regular files**: Data blocks are written with a `block_header`, optionally compressed with ZSTD. The data capacity per standard block is `block_size - sizeof(block_header)` = **4038 bytes** (for default 4096-byte blocks).

3. **Media image files**: The image is processed sector by sector:
   - An XXH64 hash is computed for each sector.
   - The dedup tree for the image's sector size is searched for a matching hash.
   - **If found (duplicate)**: The existing `dedup_entry` is referenced; no new data is written.
   - **If not found (unique)**: The sector data is appended to the current partially filled dedup data block. The tree header's `last_block_lba` and `last_block_offset` fields track the current write position.
   - When a dedup data block is full (reaches `dedup_block_size`), it is finalized with a `block_header` (see below for compression), and a new block is allocated.
   - A partially filled block persists across imports so that future files with the same sector size continue filling it.
   - A `sector_map_entry` is recorded for every sector (deduplicated or not), stored in the inode's data extents.

4. **Sector map caching**: During writes, `sector_map_entry` records are accumulated in an in-memory `sector_map_cache` and flushed to disk in batch on file close, reducing I/O overhead. CD images use a separate `cd_sector_map_cache`.

5. **Inode caching**: Per-file-handle inode caching avoids repeated B+Tree lookups during writes. The cached inode is written back once on file close if modified.

6. **Dedup block cache**: The 4 MiB dedup data block accumulator is kept in memory across writes (`dedup_block_cache`). This avoids a costly disk read + decompression on every write call. The cache is flushed and freed on file close.

7. **Shared compression pool**: A persistent thread pool handles compression for both regular file writes (block group batches) and dedup data blocks (async jobs). See the [Compression Pool](#compression-pool) section for full details.

8. **Dedup B+Tree node cache**: Frequently accessed B+Tree nodes are cached in memory during dedup writes, reducing disk reads during hash lookups and insertions.

9. **Dedup tree header caching**: The dedup B+Tree header is cached in the `dedup_block_cache` across writes, avoiding a tree-list scan and header read on each FUSE write call.

---

## Compression Pool

All compression — for both regular file block groups and dedup data blocks — is performed by a shared, persistent thread pool. The pool is created once at mount time and reused for the lifetime of the filesystem.

### Architecture

```
                ┌──────────────┐
                │  FUSE write  │
                └──────┬───────┘
                       │
          ┌────────────┼────────────┐
          │ Regular    │            │ Media image
          │ file       │            │ (dedup)
          ▼            │            ▼
  ┌───────────────┐    │    ┌───────────────┐
  │ Block-group   │    │    │ Async dedup   │
  │ batch         │    │    │ job           │
  └───────┬───────┘    │    └───────┬───────┘
          │            │            │
          ▼            ▼            ▼
  ┌─────────────────────────────────────┐
  │          compress_pool              │
  │  min(nproc, 32) persistent workers  │
  │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐   │
  │  │ W-0 │ │ W-1 │ │ W-2 │ │ ... │   │
  │  └─────┘ └─────┘ └─────┘ └─────┘   │
  └─────────────────────────────────────┘
```

### Pool structure

The pool is defined privately in `block.c`:

```c
struct compress_pool {
    pthread_t      *threads;      /* worker thread array */
    int             num_threads;  /* number of workers */
    pthread_mutex_t mutex;        /* protects batch, shutdown, generation, async_queue */
    pthread_cond_t  work_avail;   /* signalled when work is ready */
    pthread_cond_t  batch_done;   /* signalled when all batch jobs complete */
    struct compress_batch *batch; /* current batch (NULL when idle) */
    int             shutdown;     /* non-zero → workers exit */
    unsigned int    generation;   /* incremented per batch submission */
    struct pool_async_job *async_queue; /* FIFO of pending async jobs */
};
```

### Worker threads

- **Count**: `min(sysconf(_SC_NPROCESSORS_ONLN), 32)` threads, minimum 1.
- **Persistent ZSTD contexts**: Each worker creates its own `ZSTD_CCtx` (full level) and `ZSTD_CCtx` (probe, level 1) on startup. Contexts are reused across all jobs, avoiding allocation overhead.
- **Priority**: Workers check the **async queue first** (dedup jobs), then process batch jobs. This ensures dedup compression — which is latency-sensitive because the write path is waiting for a fresh buffer — gets priority.

### Generation-based wake

Each worker tracks its own `my_gen` counter. Workers sleep via `pthread_cond_wait` on `work_avail` while `pool->generation == my_gen && !async_queue && !shutdown`. Each batch submission increments `pool->generation` and broadcasts `work_avail`. Workers update `my_gen` after waking, preventing double-processing of already-completed batches.

### Work distribution

Batch jobs use **lock-free atomic work claiming**:

```c
struct compress_batch {
    struct compress_job *jobs;
    int                  total;    /* number of jobs */
    atomic_int           next;     /* next job index to claim */
    atomic_int           done;     /* completed job count */
};
```

Each worker atomically claims the next job via `atomic_fetch_add(&batch->next, 1)`. When `done >= total`, the batch is signalled as complete.

### Submitter-participates

The submitting thread does not block idle while workers compress. After broadcasting `work_avail`, it creates temporary ZSTD contexts and calls the same `compress_worker_run` function as pool workers, claiming jobs from the batch via the same atomic counter. This ensures the batch completes even if all pool workers are busy with async jobs.

### Async dedup jobs

Dedup data blocks (4 MiB) are submitted as individual async jobs:

```c
struct pool_async_job {
    int (*fn)(void *arg, void *cctx); /* work function */
    void       *arg;          /* opaque context */
    int         result;       /* return value from fn */
    _Atomic int done;         /* set to 1 when complete */
    pthread_mutex_t mtx;      /* protects cond wait */
    pthread_cond_t  cond;     /* signalled on completion */
    struct pool_async_job *next; /* queue link */
};
```

Async jobs are enqueued on `pool->async_queue` (FIFO). Workers dequeue and execute them using their persistent ZSTD context. Completion is signalled via the job's own `mtx`/`cond` pair (independent of the batch mechanism). The `done` atomic is set under the job's mutex to prevent use-after-free races.

When the pool is NULL (no workers), async jobs run inline on the calling thread with a temporary ZSTD context.

### Dedup block lifecycle

When a 4 MiB dedup data block fills up during media image writes:
1. The full buffer is submitted to the pool as an async job (ownership of the data buffer transfers to the worker).
2. A fresh buffer is `calloc`'d and a new contiguous block range is allocated.
3. The writer continues accumulating sectors into the new buffer without blocking.
4. On the next submission (or file close), `dedup_bg_wait` is called — it waits for the async job, then frees any trailing unused standard blocks on the **main thread** (the allocation bitmap is not thread-safe).

**Partial block handling**: The last dedup data block of each tree is always stored with all `std_per_dedup` standard blocks allocated (trailing blocks are not freed). This allows the partial block to be resumed on the next mount without reallocation. The tree header's `last_block_lba` and `last_block_offset` fields track the resume point.

### Fork handling

`obmafs3_compress_pool_reinit` is called from the FUSE `init` callback. When FUSE daemonises (without `-f`), `fork()` is called and the pool worker threads from the parent do not survive into the child. The reinit function safely discards the stale pool struct (no `pthread_join` or mutex/cond destroy — those are unsafe on post-fork inconsistent state) and creates a fresh pool.

---

## Block Group Compression

Regular file data blocks are grouped into **compression groups** of 16 contiguous blocks (64 KiB at default 4096-byte block size).

```c
#define OBMAFS3_COMPRESS_GROUP_BLOCKS 16  /* 16 × 4096 = 64 KiB per group */
```

### Write pipeline

The regular file write path (`write_file_data_append`) uses a three-phase pipeline:

**Phase 1 — Assemble**: Write data is distributed into per-group buffers. Each group buffer is `16 × block_size` bytes, zero-filled for unused positions.

**Phase 2 — Parallel compression**: Full groups (exactly 16 blocks) are submitted as a batch to the compression pool. Each group becomes a `compress_job`:

```c
struct compress_job {
    const uint8_t *group_data;     /* assembled group data */
    size_t         grp_bytes;      /* input size */
    uint64_t       grp_count;      /* logical blocks in this group */
    int            level;          /* ZSTD compression level */
    uint64_t       block_size;     /* filesystem block size */
    uint8_t       *comp_buf;       /* output buffer (allocated by caller) */
    size_t         comp_buf_size;  /* capacity of comp_buf */
    int            use_compressed; /* result: non-zero if compression saved space */
    uint64_t       phys_needed;    /* result: physical blocks for compressed output */
    size_t         compressed_size;/* result: compressed data size */
};
```

**Phase 3 — Allocate and write**: For each group, if compression reduced the physical block count (`phys_needed < grp_count`), the compressed output is written. Otherwise uncompressed data is written. Block extents are recorded in the inode.

### Compressibility probe

For ZSTD levels > 3, the pool workers first compress at **level 1** using a separate probe ZSTD context (`zstd_probe_cctx`). If the probe does not reduce the data by at least 10%, the full-level compression is skipped. This avoids wasting CPU on incompressible data with expensive strategies (e.g., btopt at level 15).

Partial (tail) groups that have fewer than 16 blocks are always stored uncompressed.

---

## Thread-Local Storage

Each FUSE worker thread gets a lazily allocated `obmafs3_thread_bufs` structure via `pthread_key_t`. This avoids contention on shared buffers during concurrent reads and writes.

```c
struct obmafs3_thread_bufs {
    uint8_t            *hdr_buf;          /* B+Tree header I/O (block_size) */
    uint8_t            *node_buf;         /* B+Tree node traversal (block_size) */
    uint8_t            *io_buf;           /* Data block I/O (group_bytes) */
    uint8_t            *io_buf2;          /* Decompression work buffer (group_bytes) */
    uint8_t            *comp_buf;         /* Compression output buffer */
    size_t              comp_buf_size;    /* Size of comp_buf */
    struct ZSTD_CCtx_s *zstd_cctx;       /* Full-level ZSTD compression context */
    struct ZSTD_CCtx_s *zstd_probe_cctx; /* Level-1 probe context */
    struct ZSTD_DCtx_s *zstd_dctx;       /* ZSTD decompression context */
};
```

The `zstd_probe_cctx` is a separate ZSTD context dedicated to level-1 compressibility probes. It must be distinct from `zstd_cctx` because ZSTD's btopt match finder (used at levels ≥ 7) maintains internal window state that conflicts with interleaved level-1 probes on the same context.

Buffers are freed automatically when the thread exits (via the `pthread_key_t` destructor).

---

## Blocks

All data blocks (regular file data and dedup data) are prepended with a block header:

```c
struct block_header {                        /* packed, 58 bytes */
    uint64_t magic;            /* "OBMABLCK" (0x4B434C42414D424F) */
    uint8_t  flags;            /* 0x01 = COMPRESSED */
    uint8_t  compression_type; /* 0 = None, 1 = ZSTD */
    uint64_t original_size;    /* Original payload size before compression */
    uint64_t compressed_size;  /* On-disk payload size (after compression) */
    uint8_t  checksum[32];     /* Checksum of the payload data */
};
```

**Flags:**
- `OBMAFS3_BLOCK_FLAG_COMPRESSED` (0x01) — Payload is ZSTD-compressed.

**Checksum coverage:**
- For **compressed** blocks: the checksum covers the `compressed_size` bytes of compressed payload.
- For **uncompressed** blocks: the checksum covers the `original_size` bytes of raw payload.

The effective data capacity per standard 4096-byte block is `4096 - 58 = 4038` bytes.

### Dedup Block Compression

Dedup data blocks (4 MiB default) are compressed with ZSTD when the compressed output is smaller than the original. The compression process:

1. The raw dedup payload (`original_size` bytes after the header) is compressed with ZSTD at the configured compression level.
2. If the compressed size is smaller than the original:
   - `flags` is set to `OBMAFS3_BLOCK_FLAG_COMPRESSED`.
   - `compression_type` is set to `kCompressionZstd`.
   - `compressed_size` records the compressed payload size.
   - The checksum is computed over the compressed data.
   - The compressed payload replaces the raw data after the header; the remainder of the block is zero-filled.
3. If compression does not save space, the block is stored uncompressed with `flags = 0` and `compressed_size = original_size`.

On read, if a dedup block has the `COMPRESSED` flag set, the payload is decompressed into a temporary buffer before extracting individual sectors.

---

## Checksums

The only supported checksum algorithm is **XXH64** (xxHash, 64-bit). All checksums are stored in 32-byte fields (only the first 8 bytes are used; the remainder is zeroed). Checksums are computed with the checksum field itself zeroed out.

Checksums are applied to:
- The superblock (`obmafs3_sb.checksum`)
- B+Tree headers (`btree_header.checksum`)
- B+Tree nodes (`btree_node_header.checksum`)
- Data blocks (`block_header.checksum`)
- Bitmap data (`bitmap_header.checksum`)
- Tree list header (`tree_list_header.checksum`)

---

## Compression

The only supported compression algorithm is **ZSTD** (Zstandard). The compression level is configurable (default: 15). Compression is applied to:

- Regular file data block groups (when `--compression=1` is used on mount) — see [Block Group Compression](#block-group-compression)
- Dedup data blocks (when compression is enabled) — see [Compression Pool](#compression-pool)

Compression is transparent: the read path checks the `flags` field of each `block_header` and decompresses when needed.

Compression is performed by a shared, persistent thread pool rather than per-file background threads. See the [Compression Pool](#compression-pool) section for the architecture.

---

## Tools

### `mkobmafs` — Create filesystem

Creates a new empty OBMAFS v3 filesystem.

Usage: `mkobmafs [options] <device-or-file>`

Options:
- `-s, --size <bytes>` — Total filesystem size (default: file/device size, or 1 GiB)
- `-b, --block-size <bytes>` — Block size (default: 4096)
- `-d, --dedup-size <bytes>` — Dedup block size (default: 4194304)
- `-l, --label <name>` — Volume label (default: OBMAFS3)
- `-g, --guid <uuid>` — Filesystem GUID (default: random)

The target can be a regular file or a Linux block device (uses `BLKGETSIZE64` to determine size).

Writes: superblock + backup superblock, catalog tree (header + root node with root directory entry), inode tree (header + root inode), overflow tree (header, empty), dedup tree list (header, empty), media tag tree (header, empty), CD prefix/suffix/subchannel trees (headers, empty), metadata tree (header, empty), metadata index tree (header, empty), refcount tree (header, empty), and allocation bitmap.

### `mount.obmafs` — FUSE mount

Mounts an OBMAFS v3 filesystem via FUSE 3.

Usage: `mount.obmafs --device=<path> <mountpoint> [options]`

Options:
- `--compression=<0|1>` — Enable (1) or disable (0) ZSTD compression for writes (default: 1)
- `--zstd-level=<1-15>` — ZSTD compression level (default: 15)
- `--disk-images=<spec>` — Semicolon-separated `ext=sector_size` pairs for disk image detection (default: `dsk=512;iso=2048`)
- `-f` — Run in foreground (skip daemonisation/fork)

**Supported FUSE operations:**
| Operation | Description |
|-----------|-------------|
| `init`    | Reinit compression pool after fork; set `max_write` and `max_readahead` to 1 MiB; enable `FUSE_CAP_PARALLEL_DIROPS`; intentionally disable `FUSE_CAP_WRITEBACK_CACHE` to preserve sequential write optimisation |
| `getattr` | Return file/directory attributes from inode |
| `readdir` | List directory entries from catalog tree |
| `open`    | Allocate per-file context, detect sector size for media images |
| `read`    | Read regular file data or media image data (with dedup lookup) |
| `create`  | Create new file: catalog entry + inode, detect media image by extension |
| `write`   | Write regular file data or deduplicated media image data |
| `flush`   | Persist dirty inode to disk without closing the file |
| `release` | Flush sector map cache, dedup block cache and dirty inode, free per-file context |
| `truncate`| Truncate file to new size (free excess data and overflow blocks) |
| `unlink`  | Remove file: delete catalog entry, free blocks (inline + overflow), delete inode |
| `rename`  | Rename/move file or directory; supports `RENAME_NOREPLACE` and `RENAME_EXCHANGE` |
| `link`    | Create a hard link (new catalog entry, increment inode `ref_count`) |
| `symlink` | Create a symbolic link (stores target path as file data) |
| `readlink` | Read the target of a symbolic link |
| `mkdir`   | Create a new directory (catalog entry + inode) |
| `rmdir`   | Remove an empty directory |
| `utimens` | Update modification and access timestamps |
| `chmod`   | Change file/directory permissions |
| `chown`   | Change file/directory owner and group |
| `statfs`  | Return filesystem statistics (total/free blocks, inodes) |
| `statx`   | Return extended file attributes |
| `getxattr`  | Read extended attributes (media tags as `user.mediatag.*`, metadata as `user.metadata.*`) |
| `setxattr`  | Write extended attributes |
| `listxattr` | List extended attribute names |
| `removexattr` | Remove an extended attribute |
| `ioctl`   | Custom ioctls for media tags, CD image sectors, and image metadata |
| `copy_file_range` | Clone/reflink a range of blocks between files |

**Write serialisation**: All write-side FUSE callbacks are serialised by a `pthread_mutex_t write_lock` in `obmafs3_ctx`. This avoids data races on the shared allocation bitmap, B+Tree structures, and dedup block caches.

**Per-file-handle context** (`fuse_file_ctx`):
- `inode_id` — Cached inode ID
- `sector_size` — Detected sector size (0 for regular files)
- `inode` — Cached inode structure (write-back on release)
- `inode_dirty` — Flag indicating the cached inode needs write-back
- `sme_cache` — In-memory sector map entry cache (flushed on release)
- `db_cache` — Persistent dedup data block accumulator (also holds: pending async pool job slot, dedup B+Tree node cache, cached dedup tree header)
- `cd_sme_cache` — In-memory CD sector map entry cache (flushed on release)
- `ecc_ctx` — Lazy-initialised CD ECC/EDC context (for prefix/suffix reconstruction)
- `cd_next_sector` — Next expected CD sector LBA (for sequential write optimisation)

**Extended attributes (xattr):** Media tags and image metadata are exposed as extended attributes:
- `user.mediatag.<name>` — Binary media tags (e.g., `user.mediatag.cd_toc`, `user.mediatag.dvd_pfi`). Read returns binary data; write sets the tag.
- `user.metadata.<key>` — String key-value metadata (e.g., `user.metadata.dumper`, `user.metadata.serial`). Read returns UTF-8 value; write sets the key.
- `listxattr` enumerates all media tag and metadata xattr names for the file.
- `removexattr` deletes the corresponding media tag or metadata entry.

**Custom ioctls:**
| ioctl | Description |
|-------|-------------|
| `OBMAFS3_IOC_SET_MEDIA_TAG` | Write binary media tag data for an image |
| `OBMAFS3_IOC_GET_MEDIA_TAG` | Read binary media tag data for an image |
| `OBMAFS3_IOC_SET_CD_IMAGE` | Mark a file as a CD image (enables CD sector map mode) |
| `OBMAFS3_IOC_CD_WRITE_LONG` | Write a raw 2352/2448-byte CD sector (prefix/data/suffix/subchannel split) |
| `OBMAFS3_IOC_CD_READ_LONG` | Read a reconstructed 2352-byte raw CD sector |
| `OBMAFS3_IOC_CD_READ_LONG_SUB` | Read a reconstructed 2448-byte raw CD sector with subchannel |
| `OBMAFS3_IOC_SET_METADATA` | Set a key-value metadata pair for an image |
| `OBMAFS3_IOC_GET_METADATA` | Get a metadata value by key for an image |
| `OBMAFS3_IOC_DELETE_METADATA` | Delete a metadata entry by key |
| `OBMAFS3_IOC_LIST_METADATA` | List metadata keys for an image (paginated) |

**Media image detection**: The extension-to-sector-size mapping is configurable via `--disk-images`. Up to 32 mappings are supported (`OBMAFS3_MAX_DISK_IMAGE_MAPS`). Each mapping associates a file extension with a sector size. The default mapping (`dsk=512;iso=2048`) creates files with `.dsk` as `kFileTypeMediaImage` with 512-byte sectors, and `.iso` as `kFileTypeMediaImage` with 2048-byte sectors. CD images (`kFileTypeCompactDiscImage`) use the CD sector map format with prefix/suffix/subchannel splitting and ECC/EDC reconstruction.

### `obmafsck` — Filesystem checker

Checks and verifies OBMAFS v3 filesystem integrity.

Usage: `obmafsck [-y] [-n] [-s] [-d] [-v] <path>`

Options:
- `-y` — Assume 'yes' to all repair questions
- `-n` — Assume 'no' to all repair questions (report errors only, do not modify)
- `-s` — Run data block scrub (verify all block checksums)
- `-d` — Show deduplication and compression statistics
- `-v` — Verify dedup and CD hashes against stored data

**Checks performed:**

| Check | Description |
|-------|-------------|
| Superblock validation | Magic, checksum, block sizes, LBA consistency |
| Superblock field range checks | Validate block_size, dedup_block_size, total_bytes, checksum_type, next_inode_id, bitmap_lba/blocks, tree LBAs (bounds + uniqueness), volume_label NUL-termination, creation_time; offer to fix each invalid field |
| Backup superblock | Read backup at last block, verify magic/checksum/consistency against primary; restore primary from backup when primary is unreadable; overwrite backup from primary on mismatch |
| Allocation bitmap | Load and verify bitmap checksum |
| Catalog tree | Traverse all nodes, verify magic and checksums |
| Inode tree | Traverse all nodes, verify magic and checksums |
| Overflow tree | Traverse all nodes, verify magic and checksums |
| Refcount tree | Traverse all nodes, verify magic and checksums |
| Refcount validation | Walk all inode extents, count per-block references, compare against refcount tree entries, fix mismatches |
| Media tag tree | Traverse all nodes, verify magic and checksums |
| Metadata tree | Traverse all nodes, verify magic and checksums |
| Metadata index tree | Traverse all nodes, verify magic and checksums |
| Metadata bidirectional consistency | Walk both metadata and metadata index tree leaves, verify every entry in one tree has a matching entry in the other; re-insert missing entries |
| CD prefix/suffix/subchannel trees | Traverse all nodes, verify magic and checksums |
| Dedup tree list | Verify list header, traverse all per-sector-size trees |
| Cross-reference | Verify all catalog entries have valid inodes |
| Free node chain | Verify `free_node_lba` and `free_nodes` are zero in every B+Tree header (the runtime never uses the free node chain); reset to zero on mismatch |
| Block allocation | Reconstruct expected bitmap from all on-disk structures and compare against on-disk bitmap |
| Last-block handling | The last dedup data block of each tree marks all `std_per_dedup` blocks as expected (see [Partial block handling](#dedup-block-lifecycle)) |
| Data block scrub | Read every data block, verify magic and checksum (handles compressed blocks) |
| Dedup data block scrub | Read every unique dedup data block, verify magic and checksum (handles compressed blocks) |
| Dedup hash verification | Walk all dedup trees, read sector data from data blocks, recompute XXH64 hash, compare against stored hash (optional, `-v`) |
| CD hash verification | Walk CD prefix/suffix/subchannel trees, recompute XXH64 from inline data, compare against stored hash (optional, `-v`) |

The scrub functions correctly handle both compressed and uncompressed blocks by checking the `OBMAFS3_BLOCK_FLAG_COMPRESSED` flag to determine whether to checksum `compressed_size` or `original_size` bytes.

### `libobmafs` — Static library

Provides the C API for all filesystem operations. Used by all three tools above.

**API categories:**
- Context management: `obmafs3_open`, `obmafs3_open_flags`, `obmafs3_close`
- Superblock: `obmafs3_sb_read`, `obmafs3_sb_read_lenient`, `obmafs3_sb_write`, `obmafs3_sb_validate`, `obmafs3_sb_read_backup`, `obmafs3_sb_read_backup_lenient`
- Block I/O: `obmafs3_block_read`, `obmafs3_block_write`
- B+Tree: `obmafs3_btree_header_read`, `obmafs3_btree_header_read_lenient`, `obmafs3_btree_header_write`, catalog lookup/list/insert/delete, inode get/put/delete
- Allocation: `obmafs3_alloc_block`, `obmafs3_alloc_blocks`, `obmafs3_free_block`, `obmafs3_free_blocks`, `obmafs3_alloc_inode_id`
- Bitmap: `obmafs3_bitmap_read`, `obmafs3_bitmap_write`, `obmafs3_bitmap_set`, `obmafs3_bitmap_clear`, `obmafs3_bitmap_is_set`, `obmafs3_bitmap_find_free`
- Catalog: `obmafs3_catalog_lookup`, `obmafs3_catalog_list`, `obmafs3_catalog_list_free`, `obmafs3_catalog_insert`, `obmafs3_catalog_delete`
- Inode: `obmafs3_inode_get`, `obmafs3_inode_put`, `obmafs3_inode_delete`
- File data: `obmafs3_read_file_data`, `obmafs3_write_file_data`
- Clone/reflink: `obmafs3_clone_file_range`, `obmafs3_free_file_blocks`, `obmafs3_truncate_file_blocks`
- Refcount: `obmafs3_refcount_get`, `obmafs3_refcount_set`, `obmafs3_refcount_inc`, `obmafs3_refcount_dec`
- Dedup: `obmafs3_dedup_get_tree`, `obmafs3_dedup_lookup`, `obmafs3_write_media_image_data`, `obmafs3_read_media_image_data`
- Dedup block cache: `obmafs3_flush_dedup_block_cache`, `obmafs3_free_dedup_block_cache`
- Compression pool: `obmafs3_compress_pool_init`, `obmafs3_compress_pool_reinit`, `obmafs3_compress_pool_destroy`
- Async pool jobs: `obmafs3_pool_async_job_create`, `obmafs3_pool_async_job_free`, `obmafs3_pool_submit_async`, `obmafs3_pool_wait_async`
- Thread-local buffers: `obmafs3_get_thread_bufs`
- Sector map cache: `obmafs3_flush_sector_map_cache`, `obmafs3_free_sector_map_cache`
- CD sector map cache: `obmafs3_flush_cd_sector_map_cache`, `obmafs3_free_cd_sector_map_cache`
- Media tags: `obmafs3_media_tag_get`, `obmafs3_media_tag_data_free`, `obmafs3_media_tag_put`, `obmafs3_media_tag_delete`, `obmafs3_media_tag_delete_all`, `obmafs3_media_tag_list`, `obmafs3_media_tag_list_free`
- Image metadata: `obmafs3_metadata_get`, `obmafs3_metadata_put`, `obmafs3_metadata_delete`, `obmafs3_metadata_delete_all`, `obmafs3_metadata_list`, `obmafs3_metadata_list_free`, `obmafs3_metadata_query`, `obmafs3_metadata_query_free`
- CD B+Trees: `obmafs3_cd_prefix_get/put/delete`, `obmafs3_cd_suffix_get/put/delete`, `obmafs3_cd_subchannel_get/put/delete`
- ECC/EDC: `ecc_cd_init`, `ecc_cd_free`, `ecc_cd_is_suffix_correct`, `ecc_cd_is_suffix_correct_mode2`, `ecc_cd_reconstruct`, `ecc_cd_reconstruct_prefix`, `cd_lba_to_msf`
- Checksum: `obmafs3_checksum_xxh64`, `obmafs3_checksum_block`
- Compression: `obmafs3_compress`, `obmafs3_decompress`
- Filesystem creation: `obmafs3_create`
- Filesystem checking: `obmafs3_check`
- Path resolution: `obmafs3_resolve_inode_path`

Open flags:
- `OBMAFS3_OPEN_SKIP_BITMAP` — Do not load/validate bitmap (for quick header-only checks)
- `OBMAFS3_OPEN_LENIENT` — Tolerate checksum errors (used by fsck)

---

## Error Codes

All library functions return integer error codes:

```c
#define OBMAFS3_OK           0   /* Success */
#define OBMAFS3_ERR_IO       -1  /* I/O error (read/write/seek failed) */
#define OBMAFS3_ERR_NOMEM    -2  /* Out of memory */
#define OBMAFS3_ERR_BADMAGIC -3  /* Invalid magic number */
#define OBMAFS3_ERR_CHECKSUM -4  /* Checksum verification failed */
#define OBMAFS3_ERR_NOTFOUND -5  /* Entry not found */
#define OBMAFS3_ERR_INVAL    -6  /* Invalid argument */
#define OBMAFS3_ERR_EXISTS   -7  /* Entry already exists */
#define OBMAFS3_ERR_NOSPC    -8  /* No space left on device */
```

---

## Debug Logging

Debug output is controlled by the `OBMAFS3_DEBUG` environment variable. When set to any non-empty, non-`"0"` value, every error return site emits a line to stderr with file, line number, function name, and a brief message.

The following macros are provided in `debug.h`:

| Macro | Purpose |
|-------|---------|
| `OBMAFS3_DBG(fmt, ...)` | Debug message (only when debug is enabled) |
| `DBG_RETURN(err, fmt, ...)` | Log error and return `err` from library function |
| `DBG_RETURN_ERRNO(err, fmt, ...)` | Same as above, also captures and prints `errno` |
| `FUSE_RETURN(err, fmt, ...)` | Log error and return negative errno from FUSE callback |
| `DBG_PROPAGATE(rc)` | Propagate a non-OK return code, logging the call-site |

Initialised by calling `obmafs3_debug_init()` at startup (done automatically by `mount.obmafs` and `obmafsck`).

---

## Dependencies

| Library | Purpose |
|---------|---------|
| xxHash  | XXH64 checksums for all integrity verification |
| ZSTD    | Zstandard compression for data blocks |
| libfuse3| FUSE 3 user-space filesystem interface (mount.obmafs only) |

Both xxHash and ZSTD are fetched automatically via CMake `FetchContent` at build time.

---

## Current Implementation Status

| Feature | Status |
|---------|--------|
| Superblock read/write/validate | Complete |
| Allocation bitmap | Complete |
| Catalog B+Tree (directories) | Complete |
| Inode B+Tree (file metadata) | Complete |
| Overflow B+Tree (extra extents) | Complete |
| Multi-level B+Tree (tree height > 1) | Complete (all trees) |
| Dedup tree list + per-sector-size trees | Complete |
| Regular file read/write | Complete |
| Media image write (sector-level dedup) | Complete |
| Media image read (dedup lookup) | Complete |
| Dedup data block ZSTD compression | Complete |
| Dedup data block decompression on read | Complete |
| Regular data block ZSTD compression | Complete |
| Shared compression pool | Complete |
| Block group compression (regular files) | Complete |
| Async dedup compression (pool-based) | Complete |
| Dedup B+Tree node cache | Complete |
| Sector map caching (batched writes) | Complete |
| Inode caching (per file handle) | Complete |
| Media Tag B+Tree | Complete |
| Metadata B+Tree + reverse-index | Complete |
| CD prefix/suffix/subchannel B+Trees | Complete |
| CD sector ECC/EDC reconstruction | Complete |
| Symlinks | Complete |
| Hard links | Complete |
| Directories (mkdir/rmdir) | Complete |
| Permissions (chmod/chown) | Complete |
| Extended attributes (xattr) | Complete |
| Custom ioctls (media tags, CD, metadata) | Complete |
| Clone / reflink (`copy_file_range`) | Complete (inline + overflow) |
| Copy-on-Write for shared blocks | Complete (inline + overflow) |
| Refcount-aware block freeing | Complete (truncate + unlink) |
| `mkobmafs` (create filesystem) | Complete |
| `mount.obmafs` (FUSE mount) | Complete |
| `obmafsck` (filesystem checker + scrub) | Complete |
| Filesystem repair in `obmafsck` | Partial (superblock + backup, superblock fields, B+Tree ordering/siblings/checksums/free-node-chain, bitmap, refcounts, orphan inodes) |
| Rename / move | Complete |

---

## Clone / Reflink Support

OBMAFS v3 supports block-level file cloning through the FUSE3
`copy_file_range` callback.  When a file range is cloned, source and
destination inodes share physical data blocks; a per-block reference
count tracks sharing.

### Mechanism

Linux intercepts `FICLONERANGE` / `FICLONE` ioctls in `do_vfs_ioctl()`
before they reach FUSE.  FUSE3 instead provides the `copy_file_range`
callback, which the kernel also invokes as a fallback for
`cp --reflink=auto`.

### Shared Blocks and Refcounts

Each physical data block has an implicit refcount of 1.  When a clone
operation shares a block between two inodes, `obmafs3_refcount_inc()`
creates an explicit entry with refcount 2 in the refcount B+Tree.
Entries are removed when the refcount drops back to 1.

### Copy-on-Write

When writing to a block with refcount > 1:

**Inline extent path** (block mapped via inline extent slots):

1. A new block is allocated.
2. The inline extent is split to replace the old physical LBA with
   the new one (`cow_replace_block`).
3. The old block's refcount is decremented.
4. The write proceeds to the new block.

Adjacent contiguous extents are merged (`coalesce_inline_extents`)
before each split to reclaim inline extent slots.

**Overflow extent path** (block mapped via the overflow B+Tree):

1. A new block is allocated.
2. All overflow LBAs for the inode are collected into a flat array
   (`overflow_collect_lbas`).
3. The old physical LBA is replaced with the new one in the array.
4. Overflow entries for the inode are cleared (`overflow_clear_inode`).
5. The extent map (inline + overflow) is rebuilt from the flat array
   (`rebuild_extent_map`).
6. The old block's refcount is decremented.
7. The write proceeds to the new block.

### Refcount-Aware Freeing

`obmafs3_fuse_truncate` and `obmafs3_fuse_unlink` check each block's
refcount before freeing:

- **refcount > 1**: decremented (block still in use by other inodes).
- **refcount == 1**: freed to the allocation bitmap.

### Limitations

- Both offsets and the clone length must be aligned to the per-block
  data capacity (`block_size - sizeof(block_header)`).
- Same-inode clone is not supported.
