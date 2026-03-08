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
| 14                  | Sector Tag Data B+Tree header        | `BTREEHDR` |
| 15                  | Sector Tag Ref B+Tree header         | `BTREEHDR` |
| 16                  | Junk Map B+Tree header               | `BTREEHDR` |
| 17 .. 17+N-1        | Allocation bitmap                    | `OBMABMAP` (0x50414D42414D424F) |
| 16+N ..             | Data blocks, B+Tree nodes, dedup data| `OBMABLCK` / `BTREENDE` |
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
    uint64_t keyset_lba;         /* LBA of the persisted dedup key set (0 = none) */
    uint64_t keyset_blocks;      /* Number of blocks used by the persisted key set */
    uint64_t pending_lba;        /* LBA of the persisted pending insert buffer (0 = none) */
    uint64_t pending_blocks;     /* Number of blocks used by the persisted pending buffer */
    uint32_t btree_clump_size;   /* Nodes to pre-allocate per growth for non-dedup trees (0 = default 64) */
    uint32_t dedup_clump_size;   /* Nodes to pre-allocate per growth for dedup trees (0 = default 1024) */
    uint32_t revision;           /* On-disk format revision (e.g. 20260224); newer → refuse mount */
    uint64_t compatible_flags;   /* Feature flags safe to ignore (unknown bits are harmless) */
    uint64_t rocompat_flags;     /* Feature flags requiring read-only mount if unknown */
    uint64_t incompatible_flags; /* Feature flags that must be understood to mount at all */
    uint8_t  volume_label[256];  /* Volume label, UTF-8, NUL-terminated */
    uint8_t  checksum[32];       /* Checksum of V1 portion (bytes 0..525, XXH64) */

    /* ---- Extension fields (bytes 526..4063) ---- */
    uint64_t sector_tag_data_lba;/* LBA of Sector Tag Data B+Tree header (block 14, 0 = none) */
    uint64_t sector_tag_ref_lba; /* LBA of Sector Tag Ref B+Tree header (block 15, 0 = none) */
    uint64_t junk_map_lba;       /* LBA of Junk Map B+Tree header (0 = none) */
    uint8_t  reserved[3498];     /* Zero-filled, reserved for future expansion */
    uint8_t  checksum2[32];      /* Checksum of extension area (bytes 526..4095, XXH64) */
};

#define OBMAFS3_SB_V1_SIZE 526   /* sizeof(V1 portion covered by checksum) */
```

The superblock occupies a full 4096-byte block. It is divided into two independently checksummed regions:

- **V1 portion** (bytes 0–525, `OBMAFS3_SB_V1_SIZE` = 526 bytes): Contains all original fields, terminated by `checksum`. The V1 checksum is computed over bytes 0–525 with the `checksum` field zeroed.
- **Extension area** (bytes 526–4095, 3570 bytes): Contains new fields (`sector_tag_data_lba`, `sector_tag_ref_lba`, etc.) followed by reserved space and `checksum2`. The extension checksum is computed over bytes 526–4095 with the `checksum2` field zeroed.

This two-checksum design ensures backward compatibility: older implementations read and validate only the V1 portion, see an unknown `rocompat_flags` bit, and mount read-only. Newer implementations validate both checksums. When the extension area is all-zeroes (no `checksum2`), the extension checksum is skipped — this handles filesystems created by older code.

The superblock identifies the filesystem, stores global parameters, and provides the LBAs for all top-level structures. The root inode ID is always 2 (`OBMAFS3_ROOT_INODE_ID`), and `next_inode_id` starts at 3 after creation.

A byte-identical **backup copy** of the full 4096-byte superblock is stored at the last block of the filesystem (`LBA = total_blocks − 1`). The backup is written every time the primary superblock is updated; both checksums (`checksum` and `checksum2`) are independently verified during recovery. If the primary superblock is unreadable or has invalid magic, `obmafs3_open()` and `obmafsck` automatically fall back to the backup, probing 8 candidate block sizes (4096, 512, 1024, 2048, 8192, 16384, 32768, 65536) since the block size is stored inside the superblock itself. The backup block is marked as allocated in the allocation bitmap.

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
- `keyset_lba`, `keyset_blocks` — Location and size of the persisted dedup key set extent. When zero, no key set has been persisted (see [Dedup Key Set](#dedup-key-set)).
- `pending_lba`, `pending_blocks` — Location and size of the persisted pending insert buffer extent. When zero, no pending buffer has been persisted (see [Pending Insert Buffer](#pending-insert-buffer)).
- `btree_clump_size` — Number of nodes to pre-allocate per growth for non-dedup B+Trees. 0 uses the default of 64 (see [Clump Allocation](#clump-allocation)).
- `dedup_clump_size` — Number of nodes to pre-allocate per growth for dedup B+Trees. 0 uses the default of 1024 (see [Clump Allocation](#clump-allocation)).
- `revision` — On-disk format revision (e.g. `20260224`). Set to `OBMAFS3_REVISION` at creation time. If a tool reads a revision higher than the one it was compiled with, mounting is refused (`OBMAFS3_ERR_REVISION`) to prevent corruption by older code that does not understand the newer layout.
- `compatible_flags` — Bitmask of optional feature flags that are safe to ignore. An implementation that does not recognise a set bit may mount the filesystem normally with full read-write access. No flags are currently defined.
- `rocompat_flags` — Bitmask of feature flags that require read-only mounting when unknown. If any bits outside `OBMAFS3_ROCOMPAT_FLAGS_KNOWN` are set, the implementation must mount read-only to avoid corrupting data that depends on the unknown feature. Currently defined: `OBMAFS3_ROCOMPAT_SECTOR_TAGS` (bit 0) — per-sector tag storage.
- `incompatible_flags` — Bitmask of feature flags that prevent mounting entirely when unknown. If any bits outside `OBMAFS3_INCOMPAT_FLAGS_KNOWN` are set, the implementation must refuse to mount (`OBMAFS3_ERR_INCOMPAT`). No flags are currently defined.
- `sector_tag_data_lba` — LBA of the Sector Tag Data B+Tree header. A hash-keyed dictionary of unique per-sector tag blobs. 0 if sector tags are not enabled.
- `sector_tag_ref_lba` — LBA of the Sector Tag Ref B+Tree header. A composite-keyed tree mapping `(inode_id, sector, tag_type)` to tag hashes in the data tree. 0 if sector tags are not enabled.
- `checksum2` — Checksum of the extension area (bytes 526–4095), computed with this field zeroed. All-zero when no extension fields are in use.

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
    kBtreeTypeRefcount      = 10,
    kBtreeTypeSectorTagData = 11,
    kBtreeTypeSectorTagRef  = 12,
    kBtreeTypeJunkMap       = 13
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
    kBtreeDataTypeRefcountEntry      = 10,
    kBtreeDataTypeSectorTagDataEntry  = 11,
    kBtreeDataTypeSectorTagRefEntry   = 12,
    kBtreeDataTypeJunkMapEntry        = 13
};

enum obmafs3_file_type {
    kFileTypeRegular          = 0,
    kFileTypeDirectory        = 1,
    kFileTypeMediaImage       = 2,
    kFileTypeSymlink          = 3,
    kFileTypeCompactDiscImage = 4,
    kFileTypeSubchannelFile   = 5,
    kFileTypeNintendo         = 6
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

enum obmafs3_query_op {
    kQueryOpEqual      = 0,   /* strcmp == 0 */
    kQueryOpNotEqual   = 1,   /* strcmp != 0 */
    kQueryOpGreater    = 2,   /* strcmp > 0 (lexicographic) */
    kQueryOpLess       = 3,   /* strcmp < 0 */
    kQueryOpGreaterEq  = 4,   /* strcmp >= 0 */
    kQueryOpLessEq     = 5,   /* strcmp <= 0 */
    kQueryOpContains   = 6,   /* strstr != NULL */
    kQueryOpStartsWith = 7,   /* strncmp prefix == 0 */
    kQueryOpExists     = 8    /* key exists, value ignored */
};

enum obmafs3_query_combine {
    kQueryCombineAnd = 0,     /* All filters must match */
    kQueryCombineOr  = 1      /* At least one filter must match */
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
    uint8_t  file_type;          /* 0=regular, 1=dir, 2=media image, 3=symlink, 4=CD image, 5=subchannel */
    uint64_t sector_count;       /* Media images: total number of sectors */
    uint64_t sector_map_size;    /* Media images: number of sector_map_entries written */
    uint32_t ref_count;          /* Number of hardlinks (catalog entries) pointing to this inode */
};
```

The `extents` array holds up to 8 inline extent runs. For regular files, these point to data blocks. For media image files, they point to blocks containing a flat array of `sector_map_entry` structures that map each sector to its deduplicated copy. The `sector_count` and `sector_map_size` fields are only used for media image files.

For Compact Disc images written via the `OBMAFS3_IOC_CD_WRITE_LONG` ioctl, `file_size` is set to `sector_count * CD_RAW_SECTOR_SIZE` (2352 bytes per sector). This virtual size reflects the full raw image extent regardless of the actual data sizes stored internally. For CD images written via POSIX `write()`, `file_size` reflects the actual bytes written (typically 2048 bytes per sector).

#### Subchannel Sidecar Files

When a CD sector is written via `OBMAFS3_IOC_CD_WRITE_LONG` with subchannel data (2448-byte buffer), a `.sub` sidecar file is automatically created alongside the CD image. The sidecar uses `file_type = kFileTypeSubchannelFile` and stores **no data of its own** — it shares the parent CD image's `cd_sector_map_entry` array. The inode fields are repurposed:

- `sector_count` — stores the **parent CD image's inode_id** (not a sector count)
- `file_size` — `parent_sector_count × CD_SUBCHANNEL_SIZE` (96 bytes per sector), kept in sync by the ioctl
- `sector_map_size` — unused (0)
- `extents` — unused (no data blocks)

The sidecar is **only created when at least one sector with subchannel data is written**. If no subchannel data is ever written, no sidecar file appears. On read, each 96-byte sector is fetched from the CD Subchannel B+Tree using the `subchannel_hash` from the parent's sector map. Sectors not present in the sector map (gaps between tracks) or sectors without subchannel data (`subchannel_hash == 0`) return a zero-filled 96-byte buffer. Reads never extend beyond the parent's `sector_count`.

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

The Overflow Tree is a B+Tree that stores additional extent runs for files that exceed the 8 inline extents available in the inode. Leaf nodes contain sorted `overflow_extent` records; index nodes use `overflow_index_entry` to route lookups by the composite key `(inode_id, logical_offset)`.

```c
struct overflow_extent {                     /* packed, 40 bytes */
    uint64_t inode_id;       /* Inode this extent belongs to */
    uint64_t logical_offset; /* First logical block covered (sort key) */
    uint64_t start_block;    /* Starting physical LBA of the extent run */
    uint64_t block_count;    /* Number of physical blocks in the extent run */
    uint64_t logical_count;  /* Number of logical blocks this extent covers */
};
```

Overflow entries are sorted by the composite key `(inode_id, logical_offset)`. Maximum records per leaf node with a 4096-byte block: (4096 − 70) / 40 = **100 entries**.

```c
struct overflow_index_entry {                /* packed, 24 bytes */
    uint64_t inode_id;       /* Smallest inode_id reachable through child */
    uint64_t logical_offset; /* Smallest logical_offset reachable through child */
    uint64_t child_lba;      /* LBA of the child node */
};
```

Maximum index entries per node: (4096 − 70) / 24 = **167 entries**.

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

#### Multi-filter Metadata Queries

The `OBMAFS3_IOC_QUERY_METADATA` ioctl accepts up to 4 filter conditions combined with AND or OR logic. Each filter specifies a key, a comparison operator (`enum obmafs3_query_op`), and a value. The query walks the reverse-index tree once per filter, collecting all inode IDs whose key matches and whose value satisfies the operator (lexicographic comparison for all ordered operators, `strstr` for contains, prefix `strncmp` for starts-with). Per-filter result sets are then intersected (AND) or unioned (OR) and resolved to filesystem paths.

```c
#define OBMAFS3_QUERY_MAX_FILTERS 4

struct obmafs3_query_filter {
    char    key[256];     /* Metadata key to match ("*" = any key) */
    char    value[1025];  /* Value operand (ignored for kQueryOpExists) */
    uint8_t op;           /* enum obmafs3_query_op */
};
```

#### Wildcard Key Queries

When the filter key is set to `"*"` (a single asterisk), the query matches across **all** metadata keys. Instead of navigating the reverse-index B+Tree to a specific key range, the engine descends to the leftmost leaf and scans the entire leaf chain, applying only the value operator against every record regardless of key. This enables cross-key searches such as "find all images where any metadata field contains 'maiden'". Wildcard key queries work with all operators but are most useful with `=` (equal), `CONTAINS`, and `STARTSWITH`.

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

### Sector Tag B+Trees (per-sector side data)

Two B+Trees store per-sector tag data (Apple Sony tags, DVD sector info, floppy address marks, etc.) for non-CD media images using a **hash-dedup** approach. This feature is protected by the `OBMAFS3_ROCOMPAT_SECTOR_TAGS` flag (bit 0 of `rocompat_flags`).

Most disk images have very few *unique* tag values — the same 12-byte Apple tag or 1-byte DVD sector info byte repeats across millions of sectors. The hash-dedup design separates the unique tag data from the per-sector references, achieving significant space savings.

#### Sector Tag Data Tree (hash-keyed dictionary)

A B+Tree keyed by `XXH64(tag_type ‖ data)` that stores unique tag blobs. Index nodes use standard `btree_index_entry`.

```c
#define SECTOR_TAG_DATA_MAX 64

struct sector_tag_data_record {              /* packed, 76 bytes */
    uint64_t hash;                           /* XXH64(tag_type ‖ data) */
    uint16_t tag_type;                       /* SectorTagType enum value */
    uint16_t data_length;                    /* Actual bytes of tag data */
    uint8_t  data[SECTOR_TAG_DATA_MAX];      /* Inline tag data (max 64 bytes) */
};
```

The 64-byte inline maximum comfortably covers all known per-sector tag types: Apple Sony (12B), Apple Profile (20B), Priam DataTower (24B), DVD sector info (1–5B), floppy address marks.

Maximum leaf records per 4096-byte block: `(4096 − 70) / 76` = **53 records**. Maximum index entries per node: `(4096 − 70) / 16` = **251 entries**.

#### Sector Tag Ref Tree (composite-keyed per-sector mapping)

A B+Tree keyed by `(inode_id, sector, tag_type)` that maps each sector's tag to a hash in the data tree.

```c
struct sector_tag_ref_record {               /* packed, 26 bytes */
    uint64_t inode_id;                       /* Image inode */
    int64_t  sector;                         /* Logical sector number */
    uint16_t tag_type;                       /* SectorTagType discriminator */
    uint64_t tag_hash;                       /* XXH64(tag_type ‖ data) → key into data tree */
};

struct sector_tag_ref_index_entry {          /* packed, 26 bytes */
    uint64_t inode_id;                       /* Smallest inode_id reachable through child */
    int64_t  sector;                         /* Smallest sector reachable through child */
    uint16_t tag_type;                       /* Smallest tag_type reachable through child */
    uint64_t child_lba;                      /* LBA of the child node */
};
```

Maximum leaf records per 4096-byte block: `(4096 − 70) / 26` = **154 records**. Maximum index entries per node: `(4096 − 70) / 26` = **154 entries**.

#### Read path

1. Look up `(inode_id, sector, tag_type)` in the Sector Tag Ref Tree → get `tag_hash`.
2. Look up `tag_hash` in the Sector Tag Data Tree → get `(tag_type, data_length, data[])`.
3. Return the tag data.

#### Write path

1. Compute `hash = XXH64(tag_type ‖ data)`.
2. Insert `(hash, tag_type, data_length, data)` into the Sector Tag Data Tree (idempotent — same hash = same data, dedup hit).
3. Insert `(inode_id, sector, tag_type, hash)` into the Sector Tag Ref Tree.

#### Deletion

When a media image file is deleted, all Sector Tag Ref Tree entries for that inode are removed. Data Tree entries are **not** removed since they may be shared by other inodes; orphaned data entries are harmless and can be cleaned up by `obmafsck`.

---

## Sector Map

Media image files store a `sector_map_header` followed by a flat array of `sector_map_entry` structures in their inode's data extents. The header is written once at offset 0 when the first entries are appended, and its checksum is finalized when the sector map cache is flushed.

### Sector Map Header

```c
#define OBMAFS3_SECTOR_MAP_MAGIC   0x504D524F54434553ULL  /* "SECTORMP" little-endian */
#define OBMAFS3_SECTOR_MAP_VERSION 1

enum sector_map_type {
    kSectorMapTypeNormal = 0,    /* Normal (non-CD) sector map */
    kSectorMapTypeCd     = 1     /* CD sector map */
};

struct sector_map_header {                   /* packed, 43 bytes */
    uint64_t magic;              /* OBMAFS3_SECTOR_MAP_MAGIC ("SECTORMP") */
    uint8_t  type;               /* Discriminator: kSectorMapTypeNormal or kSectorMapTypeCd */
    uint16_t version;            /* On-disk format version (OBMAFS3_SECTOR_MAP_VERSION) */
    uint8_t  checksum[32];       /* XXH64 of header (with this field zeroed) + all entries */
};
```

The checksum is computed over the entire header (with the checksum field zeroed) concatenated with all entry data, using the standard XXH64 algorithm. It is recomputed every time the sector map cache is flushed to disk. A zeroed checksum indicates the sector map has not yet been finalized.

### Sector Map Entry

There is one entry per sector in the disk image:

```c
struct sector_map_entry {                    /* packed, 34 bytes */
    int64_t  sector;              /* Logical sector number within the disk image */
    uint16_t sector_size;         /* Size of the sector in bytes (e.g. 512, 2048, 4096) */
    uint64_t hash;                /* XXH64 hash of the sector data */
    uint64_t dedup_sector_lba;    /* LBA of the dedup data block containing this sector */
    uint64_t dedup_sector_offset; /* Byte offset within the dedup data block */
};
```

The `dedup_sector_lba` and `dedup_sector_offset` fields cache the immutable location of the sector's data inside the dedup block. Since dedup blocks are append-only and never moved, these fields remain valid for the lifetime of the entry. When non-zero, the read path can skip the B+Tree traversal entirely and read the dedup data block directly. When zero (e.g., if the write path could not determine the location), the read path falls back to the standard dedup tree lookup, and `obmafsck --scrub` can populate the missing values.

#### SME Back-fill

When the read path encounters a `sector_map_entry` (or `cd_sector_map_entry`) with `dedup_sector_lba == 0`, it must perform a full B+Tree lookup. Once the lookup succeeds and the entry's `block_lba` / `block_offset` are known, the read path **writes the resolved position back into the on-disk sector map entry** so that all future reads skip the tree entirely.

Because `obmafs3_write_file_data()` performs a read-modify-write cycle on the underlying compression group, concurrent back-fills to overlapping groups would corrupt each other's updates. All back-fill writes are therefore serialised via `ctx->sme_backfill_lock` (`pthread_mutex_t`). Back-fill failures are non-fatal: they are logged but the read still succeeds using the just-resolved location. The same mechanism applies to both normal sector maps and CD sector maps.

To read a sector from the image, the system:
1. Reads the `sector_map_entry` from the inode's data extents at `sizeof(sector_map_header) + sector_num * sizeof(sector_map_entry)`.
2. If `dedup_sector_lba` is non-zero, uses the cached location directly. Otherwise, looks up the `hash` in the appropriate dedup tree to get the `dedup_entry` (and back-fills the sector map entry on success).
3. Reads the dedup data block at the determined LBA and extracts the sector data at the stored byte offset.

### CD Sector Map

CD (Compact Disc) images use the same `sector_map_header` (with `type = kSectorMapTypeCd`) followed by an array of extended `cd_sector_map_entry` structures that also track the sector's raw components — prefix, suffix, subchannel, and subheader — for lossless reconstruction of raw 2352/2448-byte sectors:

```c
struct cd_sector_map_entry {                 /* packed */
    int64_t  sector;                  /* Logical sector number within the CD image */
    uint16_t sector_size;             /* Size (e.g. 2048, 2336, 2352) */
    uint64_t hash;                    /* XXH64 hash of the CD data portion */
    uint8_t  generated_prefix;        /* 1 if prefix can be regenerated from LBA */
    uint64_t prefix_hash;             /* XXH64 hash of the 16-byte prefix */
    uint8_t  generated_suffix;        /* 1 if suffix (ECC/EDC) can be regenerated */
    uint64_t suffix_hash;             /* XXH64 hash of the 288-byte suffix */
    uint64_t subchannel_hash;         /* XXH64 hash of 96-byte subchannel (0 = not stored) */
    uint8_t  subheader[8];            /* Subheader for CD-ROM XA sectors (0 if N/A) */
    uint8_t  sector_mode;             /* Audio, Mode 1, Mode 2 Form 1/2, etc. */
    uint64_t dedup_sector_lba;        /* LBA of the dedup data block for this sector */
    uint64_t dedup_sector_offset;     /* Byte offset within the dedup data block */
    uint64_t dedup_subchannel_lba;    /* LBA of the subchannel B+Tree leaf containing this entry */
    uint64_t dedup_subchannel_offset; /* Byte offset within the leaf block */
};
```

The `dedup_sector_lba`/`dedup_sector_offset` fields cache the location of the sector's deduplicated data in the same way as in `sector_map_entry`. The `dedup_subchannel_lba`/`dedup_subchannel_offset` fields cache the position of the subchannel record inside the CD subchannel B+Tree leaf node. Because subchannel data is stored inline in B+Tree leaves (rather than in dedup blocks), these positions can change if the leaf is split; however, positions are stable once the image import is complete. The read path validates the cached subchannel location by comparing the hash stored in the leaf record with the expected `subchannel_hash`, falling back to a full tree traversal on mismatch.

The `obmafsck --scrub` phase verifies all cached dedup location fields by looking up each entry's hash in the appropriate dedup or subchannel tree and comparing the result with the cached LBA and offset. Mismatches are reported and can be repaired interactively (or automatically with `-y`).

The CD sector map is **sparse**: only sectors that belong to a track are stored. Gaps between tracks (e.g., lead-in/lead-out regions) have no entries. The `sector_count` field in the inode records the highest sector LBA + 1, while `sector_map_size` records the actual number of `cd_sector_map_entry` structures written. When reading, entries are located by **binary search** on the `sector` field rather than positional indexing, since the entry index does not necessarily equal the sector number.

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

8. **Dedup B+Tree node cache**: Frequently accessed B+Tree nodes are cached in memory during dedup writes, reducing disk reads during hash lookups and insertions. The cache uses open-addressing with Fibonacci hashing and a write-back policy — dirty nodes are accumulated in memory and flushed to disk via coalesced `pwritev()` calls. See [Node Cache Memory Management](#node-cache-memory-management) for details on the configurable memory cap and eviction strategy.

9. **Dedup tree header caching**: The dedup B+Tree header is cached in the `dedup_block_cache` across writes, avoiding a tree-list scan and header read on each FUSE write call.

---

## Clump Allocation

B+Tree node allocation uses an HFS+-style **clump allocation** strategy to improve on-disk contiguity and reduce allocation overhead. Instead of allocating one node at a time, the allocator pre-allocates a contiguous batch ("clump") of nodes and links the extras into the tree's free-node chain for subsequent use.

```c
#define OBMAFS3_DEFAULT_CLUMP_SIZE 64   /* Non-dedup trees */
#define OBMAFS3_DEDUP_CLUMP_SIZE   1024 /* Dedup trees */
```

### Node Allocation (`obmafs3_btree_alloc_node`)

1. **Pop from free list**: If the tree header's `free_node_lba` is non-zero and `free_nodes > 0`, the first free node is popped. The node's first 8 bytes store the LBA of the next free node in the chain (a singly-linked list).

2. **Clump growth**: When the free list is empty, a contiguous clump of nodes is allocated:
   - The clump size is determined by `btree_clump_size` (non-dedup) or `dedup_clump_size` (dedup) from the superblock. A value of 0 uses the compile-time default (64 or 1024).
   - `obmafs3_alloc_blocks()` is called for `clump × blocks_per_node` contiguous blocks.
   - If contiguous allocation fails, the request is halved repeatedly until it succeeds (minimum: 1 node).
   - The first node is returned to the caller.
   - Remaining nodes are linked into a singly-linked free chain: each node's first 8 bytes store the LBA of the next free node.
   - `hdr->free_node_lba` and `hdr->free_nodes` are updated in memory.

3. For multi-block nodes (e.g., metadata trees with 8-block nodes), each "node" occupies `node_size / block_size` contiguous blocks.

### Node Freeing (`obmafs3_btree_free_node`)

Freed nodes are pushed onto the head of the tree's free-node list. The node's first 8 bytes are overwritten with the current `free_node_lba`, then `free_node_lba` is updated to point to the freed node and `free_nodes` is incremented.

### Free Node Chain Format

The free node chain is a singly-linked list. Each free node's on-disk content starts with a `uint64_t` containing the LBA of the next free node (0 = end of chain). The rest of the block is zero-filled.

---

## Node Cache Memory Management

The dedup B+Tree node cache (`dedup_node_cache`) is an open-addressing hash table that caches B+Tree node blocks in memory. Each cached entry holds a full `block_size` buffer (typically 4096 bytes) plus slot metadata.

### Structure

```c
struct dedup_node_cache {
    struct dedup_cache_slot *slots;
    uint32_t  capacity;          /* Current hash table size (always power of 2) */
    uint32_t  count;             /* Number of occupied slots */
    uint32_t  max_capacity;      /* Hard cap on slot count (0 = unlimited) */
    size_t    block_size;        /* Filesystem block size */
    uint32_t *dirty_list;        /* Indices of dirty slots (for flush) */
    uint32_t  dirty_count;
    uint32_t  dirty_cap;
    uint32_t  writes_since_flush;
};
```

### Memory Budget

The cache enforces a configurable RAM ceiling via `max_capacity`. The ceiling is derived from a byte budget (default: **8 GiB**, set via `DEDUP_NC_DEFAULT_BYTES` or the `--cache-limit` mount option). At creation time the byte budget is converted to a maximum slot count:

```
max_entries = max_bytes / (block_size + sizeof(dedup_cache_slot))
```

The result is rounded down to the nearest power of two. For a 4096-byte block size, the 8 GiB default yields approximately 1.9 million cached nodes.

### Growth and Eviction

The hash table doubles its capacity when occupancy reaches 75%. Before doubling, the cache checks `max_capacity`:

- **Below cap**: The table is doubled and all entries are rehashed into the new slot array (standard `cache_grow`).
- **At cap**: Instead of growing, the cache performs **clean-entry eviction** (`cache_evict_clean`):
  1. Scan the slot array and free all non-dirty (clean) entry buffers until occupancy drops below 75%.
  2. Collect surviving entries, clear the slot array, and rehash them in place to repair open-addressing probe chains.
  3. Rebuild the dirty list from scratch.

Dirty entries (nodes with pending writes) are **never evicted**. If all remaining entries are dirty, the eviction returns `OBMAFS3_ERR_NOMEM`, prompting callers to flush dirty entries to disk first.

This design ensures that:
- **Writes are always safe** — dirty nodes remain in cache until flushed.
- **Reads degrade gracefully** — evicted clean nodes are simply re-read from disk on the next access.
- **Memory usage is bounded** — the cache cannot grow beyond the configured limit.

### Mount Option

The memory budget is configured via the `--cache-limit` mount option (see [mount.obmafs](#mountobmafs--fuse-mount)). The value is stored in `obmafs3_ctx.cache_limit` and passed to `dedup_cache_create()` at cache initialization time. A value of 0 uses the 8 GiB default.

---

## Dedup Key Set

The **dedup key set** is a fixed-capacity in-memory hash table with LRU eviction that stores dedup hash keys seen in the B+Tree. This enables O(1) existence checks ("has this sector hash been seen before?") without any disk I/O.

### Structure

```c
struct ks_slot {
    uint64_t key;        /* The hash key stored here (0 = unused) */
    uint32_t chain_next; /* Next slot in same hash bucket (DEDUP_KS_NIL = end) */
    uint32_t lru_prev;   /* Previous in LRU list (DEDUP_KS_NIL = head) */
    uint32_t lru_next;   /* Next in LRU list (DEDUP_KS_NIL = tail) */
};

struct dedup_key_set {
    struct ks_slot *slots;        /* Pre-allocated node pool [capacity] */
    uint32_t       *buckets;      /* Hash-table bucket heads [bucket_count] */
    uint32_t        capacity;     /* Total number of slots */
    uint32_t        bucket_count; /* Number of hash buckets (2 × capacity) */
    uint32_t        count;        /* Number of occupied slots */
    uint32_t        free_head;    /* Head of the free-slot singly-linked list */
    uint32_t        lru_head;     /* Most-recently used slot */
    uint32_t        lru_tail;     /* Least-recently used slot (eviction candidate) */
};
```

- **Hashing**: Fibonacci hashing (`key × 0x9E3779B97F4A7C15 >> 32`) maps keys to bucket indices.
- **Collision resolution**: Separate chaining via `chain_next` pointers.
- **Fixed capacity**: The slot count is computed from a configurable RAM budget (default: **4 GiB**, set via `DEDUP_KS_DEFAULT_BYTES` or the `--keyset-limit` mount option). The formula: `capacity = max_bytes / (sizeof(ks_slot) + 2 × sizeof(uint32_t))`. The bucket count is `2 × capacity` for ~50% average chain length.
- **LRU eviction**: When the table is full, inserting a new key evicts the least-recently-used entry. Lookups and insertions of existing keys promote the entry to the MRU position.
- **Sentinel**: Hash value 0 is reserved as "empty slot" and cannot be stored.
- **Free list**: Unoccupied slots are threaded into a singly-linked free list via `chain_next`.

### Memory Budget

The capacity is configured via the `--keyset-limit` mount option (see [mount.obmafs](#mountobmafs--fuse-mount)). The value is stored in `obmafs3_ctx.keyset_limit` and passed to `keyset_create()` at warmup time. A value of 0 uses the 4 GiB default. For the default budget, the key set holds approximately 143 million keys.

### Population

The key set is populated during the [warmup phase](#warmup-thread):
1. **Fast path**: If a persisted key set exists on disk (`keyset_lba ≠ 0`), it is loaded directly.
2. **Slow path**: All dedup tree leaves are scanned via BFS, and each leaf's hash keys are ingested into the set.

During normal operation, newly inserted keys are added to the set immediately. If the set is at capacity, the LRU entry is evicted to make room.

### Persistence

The key set is persisted to disk at unmount time so the next mount can use the fast-path load.

```c
#define KEYSET_PERSIST_MAGIC 0x53594B44444E4F4DULL /* "MONDKEYS" LE */

struct keyset_persist_header {       /* packed */
    uint64_t magic;      /* KEYSET_PERSIST_MAGIC */
    uint64_t count;      /* Number of uint64_t keys following this header */
    uint64_t checksum;   /* XXH64 of the packed key array (count × 8 bytes) */
};
```

All non-empty keys are packed into a flat `uint64_t` array, prepended with the header, and written to a contiguously allocated extent. The superblock's `keyset_lba` / `keyset_blocks` fields record the location. On load, the header magic and XXH64 checksum are validated; if either check fails, the key set falls back to a full tree scan.

---

## Dedup Lookup Cache

The **dedup lookup cache** (`dedup_lookup_cache`) is a global, thread-safe LRU hash table that caches `hash → dedup_entry` mappings. It provides O(1) read-path dedup lookups for recently and frequently accessed sectors, avoiding B+Tree traversals entirely on cache hits.

### Structure

```c
struct dedup_lc_node {
    uint64_t hash;          /* Hash key */
    uint64_t tree_lba;      /* Distinguishes different dedup trees */
    uint64_t block_lba;     /* Dedup block LBA */
    uint64_t block_offset;  /* Offset within the dedup block */
    uint32_t lru_prev;      /* Previous node in LRU list */
    uint32_t lru_next;      /* Next node in LRU list */
    uint32_t chain_next;    /* Next node in hash bucket chain */
};

struct dedup_lookup_cache {
    struct dedup_lc_node *nodes;      /* Node pool [0 .. capacity-1] */
    uint32_t             *buckets;    /* Hash bucket heads [0 .. DEDUP_LC_BUCKETS-1] */
    uint32_t              capacity;   /* Total node pool size */
    uint32_t              count;      /* Currently occupied nodes */
    uint32_t              lru_head;   /* Most recently used */
    uint32_t              lru_tail;   /* Least recently used (eviction candidate) */
    uint32_t              free_head;  /* Head of free-list (singly-linked via chain_next) */
    pthread_mutex_t       lock;       /* Protects all fields */
};
```

- **Capacity**: Fixed at 8 388 608 entries (`DEDUP_LC_CAPACITY`, 2²³). Bucket count equals the capacity.
- **Memory**: ~416 MiB total (32 MiB for bucket array + 384 MiB for node pool).
- **Key**: Composite `(hash, tree_lba)` — the `tree_lba` distinguishes entries from different per-sector-size dedup trees.
- **Hashing**: Fibonacci hashing on `hash`, same as the key set.
- **Collision resolution**: Separate chaining.
- **Eviction**: When full, the LRU tail entry is evicted to make room.
- **Thread safety**: All operations are protected by `lock` (`pthread_mutex_t`).

### Usage

- **Read path**: Before traversing the dedup B+Tree, the read path checks the lookup cache via `dedup_lc_get()`. On a hit, the entry is promoted to MRU and the B+Tree lookup is skipped entirely.
- **Write path**: When a new dedup entry is inserted (via the pending buffer or direct B+Tree insert), it is also placed into the lookup cache via `dedup_lc_put()` so that subsequent reads see it immediately.
- **Housekeeping thread**: When draining the pending buffer, successfully inserted entries are added to the lookup cache.
- **Warmup thread**: The lookup cache is populated via `dlc_warmup()` during the warmup phase, pre-loading entries from the dedup tree leaves.

---

## Pending Insert Buffer

The **pending insert buffer** is an in-memory open-addressing hash table that defers B+Tree insertions for new dedup entries. Instead of immediately inserting each new sector hash into the B+Tree (which requires expensive random disk reads to find the correct leaf), new entries are buffered here and flushed in bulk by the [housekeeping thread](#housekeeping-thread).

### Structure

```c
struct dedup_pending_buf {
    struct dedup_entry *slots;     /* Open-addressing table */
    uint32_t            capacity;  /* Always a power of 2 */
    uint32_t            count;     /* Number of occupied slots */
    uint16_t            sector_size; /* Sector size of the dedup tree */
};
```

- Uses the same Fibonacci hashing and linear probing as the key set.
- Stores full `dedup_entry` records (hash + block_lba + block_offset).
- The write path checks here before the B+Tree: if a hash is found in the pending buffer, the existing entry is reused (deduplication hit).

### Write Path Integration

1. Check the **key set** — if the hash exists, it's a known duplicate; look up the B+Tree.
2. If the key set says "not found", check the **pending buffer** — if found, reuse the pending entry.
3. If neither has the hash, it's a genuinely new sector: write the data, create a `dedup_entry`, insert into the pending buffer, and add the hash to the key set.

This avoids B+Tree traversals entirely for new sectors during the hot write path.

### Sector Size Change Handoff

Each pending buffer is bound to a single `sector_size` (and therefore a single dedup B+Tree). When consecutive files use different sector sizes (e.g., a 512-byte `.img` followed by a 2048-byte `.iso`), the stale pending entries must be drained before the write path can buffer entries for the new sector size.

Rather than flushing the stale buffer synchronously on the FUSE write thread (which would block all filesystem operations for the duration of a potentially large batch insert), the write path **hands the buffer off to the housekeeping thread**:

1. If the draining slot is free (`dedup_pending_draining == NULL`), housekeeping is running, and shutdown has not been requested:
   - Move the current pending buffer to `dedup_pending_draining`.
   - Allocate a fresh empty pending buffer for the write path (with `sector_size = 0`, to be set on the first insert).
   - Signal `housekeeping_cond` to wake the housekeeping thread immediately.
   - The write path continues without waiting — zero synchronous tree I/O.

2. If the draining slot is occupied (housekeeping is actively draining a previous buffer), housekeeping is not running, or the fresh-buffer allocation fails:
   - Fall back to the original synchronous `pending_flush()` path.

After the handoff the housekeeping thread drains the stale buffer exactly as it would for a normal periodic swap — using the batched prefetch/insert algorithm described in the [Housekeeping Thread](#housekeeping-thread) section. The stale buffer's `sector_size` field tells the housekeeping thread which dedup tree to target.

### Persistence

The pending buffer is persisted to disk at unmount time so un-drained entries survive across mounts.

```c
#define PENDING_PERSIST_MAGIC 0x474E49444E455055ULL /* "UPENDING" LE */

struct pending_persist_header {      /* packed */
    uint64_t magic;         /* PENDING_PERSIST_MAGIC */
    uint64_t count;         /* Number of dedup_entry records */
    uint16_t sector_size;   /* Sector size of the pending buffer */
    uint8_t  _pad[6];       /* Alignment padding */
    uint64_t checksum;      /* XXH64 of the packed entry array */
};
```

Entries from both the active pending buffer and any in-progress draining buffer are merged and written contiguously. The superblock's `pending_lba` / `pending_blocks` fields record the location.

### Restore Optimisations

When restoring a persisted pending buffer at mount time, two optimisations avoid O(N log N) CPU work that would otherwise stall the warmup thread:

1. **Pre-sized allocation** — `pending_create_presized(hdr.count)` allocates the hash table at the final capacity (next power-of-two ≥ `count / 0.75`), eliminating all `pending_grow()` doublings and rehashes. Without pre-sizing, restoring *N* entries from the default 4 096-slot table triggers ~log₂(N / 4096) growth steps, each rehashing every entry.

2. **Conditional keyset insertion** — When the keyset was loaded from its persisted file, the pending hashes are already present (they were inserted during the previous mount and saved with the keyset). The per-entry `keyset_insert()` loop is skipped, avoiding millions of probes into an already-dense hash table. When the keyset had to be rebuilt via a full tree scan, the loop runs as before because the scan only covers entries that were drained into the B+Tree.

---

## Housekeeping Thread

A background **housekeeping thread** drains the pending insert buffer into the B+Tree in small batches, running concurrently with the FUSE write path.

### Lifecycle

1. Started after the warmup thread completes (via `obmafs3_housekeeping_start()`).
2. Waits for the warmup thread to finish before doing any drain work.
3. Runs continuously until `obmafs3_housekeeping_stop()` is called at unmount.

### Drain Algorithm

1. **Swap**: Under `tree_lock`, the active pending buffer is moved to a "draining" pointer and a fresh empty buffer is created for the write path. The swap can also be initiated by the write path itself during a [sector size change](#sector-size-change-handoff), in which case the housekeeping thread is signalled to wake immediately.
2. **Extract and sort**: All entries are extracted from the draining buffer and sorted by hash for sequential B+Tree leaf access.
3. **Batch processing**: Entries are processed in batches of 32 (`HOUSEKEEPING_BATCH_SIZE`):
   - **Phase A — Snapshot**: Briefly acquire `tree_lock` to read the current root LBA.
   - **Phase B — Prefetch** (without lock): Use direct `pread()` to traverse the B+Tree and find target leaf LBAs, then issue `posix_fadvise(WILLNEED)` and pre-read them into the kernel page cache.
   - **Phase C — Insert** (under lock): Perform the actual B+Tree insertions using the node cache. Because the leaves are already in the page cache, disk reads are served from RAM.
   - **Yield**: Sleep 50ms (`HOUSEKEEPING_YIELD_US`) between batches to yield I/O bandwidth to the write path.
4. **Idle sleep**: When no work is available, the thread sleeps for 5 seconds (`HOUSEKEEPING_IDLE_SEC`) before checking again.
5. **Shutdown**: If shutdown is requested mid-drain, the incomplete draining buffer is preserved so `obmafs3_dedup_pending_save()` can persist the remaining entries for the next mount.

### Thread Safety

- The draining buffer is only accessed by the housekeeping thread (no lock needed for extraction/sorting).
- B+Tree inserts are serialised via `tree_lock`.
- Re-draining already-inserted entries on the next mount is safe because `dedup_upsert_find` skips duplicates.

---

## Warmup Thread

A background **warmup thread** populates the dedup key set and node cache at mount time, running concurrently with early FUSE operations.

### Sequence

1. Creates the dedup node cache if not already present.
2. **Fast path**: Tries to load the persisted key set from disk (`obmafs3_dedup_keyset_load`). If successful, skips the tree scan and sets `loaded = 1`.
3. **Slow path**: If no persisted key set exists or validation fails, creates a fresh key set and scans all dedup tree leaves via BFS to populate it (`loaded = 0`).
4. Loads the persisted pending buffer from disk (if `pending_lba ≠ 0`) via `obmafs3_dedup_pending_load(ctx, loaded)`. The `loaded` flag controls [restore optimisations](#restore-optimisations): when the keyset came from disk, redundant keyset insertions are skipped; the pending buffer is always pre-sized to its final capacity to avoid rehash churn.
5. Creates a fresh pending buffer if none was loaded.
6. Populates the [dedup lookup cache](#dedup-lookup-cache) via `dlc_warmup()`, pre-loading hash→dedup_entry mappings from the dedup tree leaves so the read path has a warm cache from the start.
7. Signals completion via `warmup_cond` broadcast.

The write path calls `obmafs3_dedup_warmup_wait()` which blocks until the warmup thread signals completion. This ensures the key set and pending buffer are ready before any dedup writes occur.

---

## Tree Defragmentation

The `obmafsck` tool supports optional B+Tree defragmentation via the `-f` / `--defrag` flag. This relocates fragmented tree nodes into a contiguous extent, improving sequential access performance.

### Algorithm

For each tree:

1. **BFS collection**: Traverse the tree via breadth-first search to collect all node LBAs in logical order.
2. **Contiguity check**: If the nodes are already contiguous on disk, the tree is skipped.
3. **Free region search**: Find a contiguous free region large enough for all nodes.
4. **User prompt**: Unless `-y` is specified, ask the user to confirm the relocation.
5. **Relocation map**: Build a mapping from old LBAs to new LBAs.
6. **Read, remap, and write**: Read each node, update all internal LBA references (child pointers, sibling links, overflow links) using the relocation map, and write to the new location.
7. **Free old blocks**: Release the old node blocks back to the allocation bitmap.
8. **Update header**: Update the tree header's `root_node_lba` and reset the free-node chain.
9. **Persist bitmap**: Write the updated allocation bitmap to disk.

### Scope

Defragmentation is applied to all B+Trees: catalog, inode, overflow, media tag, CD prefix/suffix/subchannel, refcount, metadata, metadata index, and all per-sector-size dedup sub-trees.

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
- Sector map header (`sector_map_header.checksum`) — covers the header plus all sector map entries

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
- `--cache-limit=<size>` — Maximum RAM budget for the dedup B+Tree node cache (default: `8G`). Accepts K, M, or G suffixes (e.g. `2G`, `512M`). See [Node Cache Memory Management](#node-cache-memory-management).
- `--keyset-limit=<size>` — Maximum RAM budget for the dedup key set (default: `4G`). Accepts K, M, or G suffixes (e.g. `2G`, `512M`). See [Dedup Key Set — Memory Budget](#memory-budget).
- `--disk-images=<spec>` — Semicolon-separated `ext=sector_size` pairs for disk image detection (default: `dsk=512;iso=2048;img=512;IMA=512;adf=512;xdf=512;usb=512`)
- `-f` — Run in foreground (skip daemonisation/fork)

**Mount-time enforcement:**

The following checks are performed by `obmafs3_open_flags()` before the filesystem is fully opened. Both checks are skipped in lenient mode (`OBMAFS3_OPEN_LENIENT`), which is used by `obmafsck`.

- **Revision check**: If the on-disk `revision` exceeds the compiled `OBMAFS3_REVISION`, mounting is refused with `OBMAFS3_ERR_REVISION`. This prevents older code from silently corrupting newer on-disk layouts.
- **Incompatible flags**: If any bits in `incompatible_flags` are not in `OBMAFS3_INCOMPAT_FLAGS_KNOWN`, mounting is refused with `OBMAFS3_ERR_INCOMPAT`.
- **Read-only compatible flags**: If any bits in `rocompat_flags` are not in `OBMAFS3_ROCOMPAT_FLAGS_KNOWN`, the `read_only` flag is set in the context and the mount tool injects `-o ro` into the FUSE arguments, forcing a read-only mount.
- **Compatible flags**: Unknown bits in `compatible_flags` are silently ignored; the filesystem is mounted with full read-write access.

**Supported FUSE operations:**
| Operation | Description |
|-----------|-------------|
| `init`    | Reinit compression pool after fork; set `max_write` and `max_readahead` to 1 MiB; enable `FUSE_CAP_PARALLEL_DIROPS`; intentionally disable `FUSE_CAP_WRITEBACK_CACHE` to preserve sequential write optimisation |
| `getattr` | Return file/directory attributes from inode |
| `readdir` | List directory entries from catalog tree |
| `open`    | Allocate per-file context, detect sector size for media images |
| `read`    | Read regular file data or media image data (with dedup lookup). For CD images, POSIX reads always return 2352-byte raw sectors without subchannel data. Sectors not present in the sector map (gaps between tracks) are zero-filled. Reads are clamped to `sector_count × 2352` and never extend beyond the last sector. |
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
| `statfs`  | Return filesystem statistics (total/free blocks, inodes); sets `f_fsid` to `OBMAFS3_SB_MAGIC` |
| `statx`   | Return extended file attributes |
| `getxattr`  | Read extended attributes (media tags as `user.mediatag.*`, metadata as `user.metadata.*`, filesystem identity as `system.obmafs3.fstype` on root) |
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
- `sub_inode_id` — Inode ID of the `.sub` subchannel sidecar (0 if none created yet)
- `sub_inode` — Cached sidecar inode (written back on flush/release)
- `sub_inode_dirty` — Flag indicating the sidecar inode needs write-back

**Extended attributes (xattr):** Media tags and image metadata are exposed as extended attributes:
- `system.obmafs3.fstype` — Filesystem identity xattr, returned only on the root directory (`/`). Value: `"obmafs3"`. Used by tools like `obmafs-query` to confirm the mount is OBMAFS3.
- `user.mediatag.<name>` — Binary media tags (e.g., `user.mediatag.cd_toc`, `user.mediatag.dvd_pfi`). Read returns binary data; write sets the tag.
- `user.metadata.<key>` — String key-value metadata (e.g., `user.metadata.dumper`, `user.metadata.serial`). Read returns UTF-8 value; write sets the key.
- `listxattr` enumerates `system.obmafs3.fstype` on the root directory, and all media tag and metadata xattr names on image files.
- `removexattr` deletes the corresponding media tag or metadata entry.

**Custom ioctls:**
| ioctl | Description |
|-------|-------------|
| `OBMAFS3_IOC_SET_MEDIA_TAG` | Write binary media tag data for an image |
| `OBMAFS3_IOC_GET_MEDIA_TAG` | Read binary media tag data for an image |
| `OBMAFS3_IOC_SET_CD_IMAGE` | Mark a file as a CD image (enables CD sector map mode) |
| `OBMAFS3_IOC_CD_WRITE_LONG` | Write a raw 2352/2448-byte CD sector at a given LBA (prefix/data/suffix/subchannel split). Updates `sector_count` to `max(sector_count, LBA + 1)` and sets `file_size = sector_count × CD_RAW_SECTOR_SIZE` (2352). When the buffer is 2448 bytes (includes subchannel), a `.sub` sidecar file (`kFileTypeSubchannelFile`) is created on the first such write, and its `file_size` is kept in sync. |
| `OBMAFS3_IOC_CD_READ_LONG` | Read a reconstructed 2352-byte raw CD sector |
| `OBMAFS3_IOC_CD_READ_LONG_SUB` | Read a reconstructed 2448-byte raw CD sector with subchannel |
| `OBMAFS3_IOC_SET_METADATA` | Set a key-value metadata pair for an image |
| `OBMAFS3_IOC_GET_METADATA` | Get a metadata value by key for an image |
| `OBMAFS3_IOC_DELETE_METADATA` | Delete a metadata entry by key |
| `OBMAFS3_IOC_LIST_METADATA` | List metadata keys for an image (paginated) |
| `OBMAFS3_IOC_QUERY_METADATA` | Multi-filter metadata query with AND/OR combination (up to 4 filters, paginated paths). Supports operators: equal, not-equal, greater, less, greater-or-equal, less-or-equal, contains, starts-with, and exists. All comparisons are lexicographic. Setting the key to `"*"` performs a wildcard query across all metadata keys. |
| `OBMAFS3_IOC_SET_MEDIA_IMAGE` | Convert an empty regular file to a MediaImage with a given sector size |
| `OBMAFS3_IOC_SET_SECTOR_TAG` | Write a per-sector tag to a media image (stores in hash-dedup sector tag trees) |
| `OBMAFS3_IOC_GET_SECTOR_TAG` | Read a per-sector tag from a media image |

**Media image detection**: The extension-to-sector-size mapping is configurable via `--disk-images`. Up to 32 mappings are supported (`OBMAFS3_MAX_DISK_IMAGE_MAPS`). Each mapping associates a file extension with a sector size. The default mapping is `dsk=512;iso=2048;img=512;IMA=512;adf=512;xdf=512;usb=512`. Extension matching is **case-insensitive** (`strcasecmp`), so `.DSK`, `.Dsk`, and `.dsk` all match the same mapping. CD images (`kFileTypeCompactDiscImage`) use the CD sector map format with prefix/suffix/subchannel splitting and ECC/EDC reconstruction.

### `import-aif` — Aaru Image Format importer

Imports an Aaru Image Format (.aif) disk image file into a mounted OBMAFS3 filesystem via standard POSIX I/O and ioctls. This tool does not link against `libobmafs`; it communicates with the mounted filesystem entirely through `open()`, `write()`, and `ioctl()` system calls.

Usage: `import-aif [options] <aif-file> <output-path>`

Arguments:
- `<aif-file>` — Path to the source `.aif` file
- `<output-path>` — Full path for the image within the mounted filesystem (e.g. `/mnt/obmafs/images/myimage`)

Options:
- `-h, --help` — Show help

**Import pipeline:**

1. Opens the AIF file via `libaaruformat` (`aaruf_open`).
2. Reads `ImageInfo` (sector count, sector size, media type).
3. Determines if the image is a Compact Disc using `is_compact_disc_media()` (39 known CD media types).
4. Creates parent directories on the mounted filesystem (`mkdir -p` style).
5. Creates the output file and converts it to the appropriate type:
   - **Compact Disc**: `OBMAFS3_IOC_SET_CD_IMAGE` ioctl.
   - **Other media**: `OBMAFS3_IOC_SET_MEDIA_IMAGE` ioctl with the image's sector size.
6. Imports sector data:
   - **Flat images** (`import_flat_image`): Reads each sector via `aaruf_read_sector`, handles variable sector sizes via `AARUF_ERROR_BUFFER_TOO_SMALL` + realloc, writes sequentially.
   - **CD images** (`import_cd_image`): Reads tracks via `aaruf_get_tracks`, then iterates track by track — for each track, sectors from `start - pregap` to `end` are read. Only sectors belonging to a track are imported; gaps between tracks are skipped (the `CD_WRITE_LONG` ioctl carries the sector LBA, so non-contiguous sectors are placed correctly). For each sector, the tool first attempts a raw read via `aaruf_read_sector_long` (2352 bytes); if that fails, it falls back to a cooked read via `aaruf_read_sector` and reconstructs the full raw sector using `aaruf_ecc_cd_reconstruct_prefix` and `aaruf_ecc_cd_reconstruct`. Subchannel data (96 bytes) is appended when available (`aaruf_read_sector_tag`). Each sector is written via the `OBMAFS3_IOC_CD_WRITE_LONG` ioctl with its LBA.
7. Imports all media tags (`import_media_tags`): Enumerates available tags via `aaruf_get_readable_media_tags`, reads each tag, stores via `OBMAFS3_IOC_SET_MEDIA_TAG` ioctl.
8. For non-CD images, imports per-sector tags (`import_sector_tags`): Queries `aaruf_get_readable_sector_tags` for available non-CD tag types (Apple Sony 12B, DVD CMI, floppy address marks, DVD sector title key/info/number/IED/EDC, Apple Profile 20B, Priam DataTower 24B), iterates all sectors, and stores each tag via `OBMAFS3_IOC_SET_SECTOR_TAG` ioctl. CD-specific tags (sync, header, subchannel, etc.) are skipped since they are handled by the CD import path.
9. Imports image metadata (`import_metadata`): Stores application name/version, media type string, disk geometry, media sequence, and 12 UTF-16LE metadata fields (converted to UTF-8 via `iconv`) including: creator, comments, media title, manufacturer, model, serial number, barcode, part number, drive manufacturer/model/serial/firmware.
10. Generates a CDRWin-format cue sheet (`.cue`) for Compact Disc images (`write_cue_file`): Includes `REM ORIGINAL MEDIA-TYPE`, `REM METADATA AARU MEDIA-TYPE`, ripping tool info, `CATALOG` (MCN), per-session markers, and per-track `TRACK`/`FLAGS`/`ISRC`/`INDEX` entries.
11. Exports sidecar files:
    - CICM XML metadata (`.metadata.xml`)
    - Aaru JSON metadata (`.metadata.json`)
    - Dump hardware JSON (`.dumphw.json`) — parsed from binary format (18-byte header, 36-byte per-entry records with strings and extent arrays)

**Source files:**

| File | Contents |
|------|----------|
| `import_aif.h` | Shared header with all includes and function declarations |
| `main.c` | Entry point, argument parsing, `mkdirs()`, orchestration |
| `convert.c` | Tag/type mapping helpers (`aaruf_tag_to_obmafs`, `aaruf_track_type_to_cd_mode`, `cd_mode_sector_size`), `is_compact_disc_media()` |
| `cuesheet.c` | CDRWin cue sheet generation (`write_cue_file`) |
| `metadata.c` | Media tag import (`import_media_tags`), metadata import with UTF-16LE→UTF-8 conversion (`import_metadata`) |
| `import.c` | Sector data import for flat images (`import_flat_image`), CD images (`import_cd_image`), and per-sector tag import (`import_sector_tags`) |
| `sidecar.c` | CICM XML, Aaru JSON, and dump hardware JSON export (`export_sidecar_files`) |

**Dependencies:** Links only against `libaaruformat` (shared library). Includes OBMAFS3 headers (`enums.h`, `obmafs3_ioctl.h`, `tags.h`) but does not link `libobmafs`.

### `obmafs-query` — Interactive metadata query tool

Interactive command-line tool for querying image metadata on a live OBMAFS3 mount. Communicates entirely through `getxattr()` (for mount validation) and `ioctl()` (for queries). Does not link `libobmafs`.

Usage: `obmafs-query <mountpoint>`

The tool validates the mount point by: (1) checking `statfs` reports `FUSE_SUPER_MAGIC`, and (2) reading the `system.obmafs3.fstype` xattr on the root directory. If either check fails, it exits with an error.

Once connected, it presents an interactive `>` prompt. Commands:
- `help` — Show available commands and query syntax
- `quit` / `exit` — Exit the tool (Ctrl-D also works)

**Query syntax:**

| Syntax | Description |
|--------|-------------|
| `<key> = "<value>"` | Exact match |
| `<key> != "<value>"` | Not equal |
| `<key> > "<value>"` | Greater than (lexicographic) |
| `<key> < "<value>"` | Less than (lexicographic) |
| `<key> >= "<value>"` | Greater or equal |
| `<key> <= "<value>"` | Less or equal |
| `<key> CONTAINS "<value>"` | Substring match |
| `<key> STARTSWITH "<value>"` | Prefix match |
| `<key> EXISTS` | Key exists (any value) |
| `* = "<value>"` | Any key equals value |
| `* CONTAINS "<value>"` | Any key's value contains substring |
| `* STARTSWITH "<value>"` | Any key's value starts with prefix |

Multiple conditions can be joined with `AND` or `OR` (up to 4 filters, cannot mix `AND` and `OR` in a single query):

```
artist = "Iron Maiden" AND year > "1985"
genre = "Rock" OR genre = "Metal"
* CONTAINS "maiden"
```

Values may be quoted (`"..."`) or unquoted single words. Use `\"` for literal quotes inside quoted strings.

**Result export:** After each query that returns results, the tool offers to export:
- `txt <path>` — One path per line, plain text
- `json <path>` — JSON object with `count` and `results` array (properly escaped)
- Press Enter to skip

**Implementation notes:** The tool creates a temporary sentinel file on the mount (immediately unlinked) to obtain a FUSE file descriptor for ioctl calls. Query parsing translates the human-friendly syntax into `obmafs3_ioctl_metadata_query_arg` structs, and results are paginated automatically (8 paths per ioctl call) until all matches are collected.

### `obmafsck` — Filesystem checker

Checks and verifies OBMAFS v3 filesystem integrity.

Usage: `obmafsck [-y] [-n] [-s] [-d] [-D] [-v] [-f] <path>`

Options:
- `-y` — Assume 'yes' to all repair questions
- `-n` — Assume 'no' to all repair questions (report errors only, do not modify)
- `-s, --scrub` — Run data block scrub (verify all block checksums)
- `-d, --dedup-stats` — Show deduplication and compression statistics
- `-D, --dedup-stats-only` — Show dedup statistics and exit (skip all checks)
- `-v, --verify-hashes` — Verify dedup and CD hashes against stored data
- `-f, --defrag` — Defragment B+Tree nodes (relocate to contiguous extents)

**Checks performed:**

| Check | Description |
|-------|-------------|
| Superblock validation | Magic, checksum, block sizes, LBA consistency |
| Revision display | Display on-disk revision; mark OK (✔) if ≤ compiled `OBMAFS3_REVISION`, or bad (✗) if newer |
| Feature flags display | Display all three feature flag fields; count unknown incompatible flags as errors; note if unknown ro-compat flags would force read-only mount |
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
| Key ordering | Verify records within each leaf node are in ascending key order; verify keys across sibling nodes are monotonically increasing |
| Sibling link verification | Verify `left_link` / `right_link` bidirectional consistency between adjacent nodes at each tree level |
| Extent validation | Verify all inline and overflow extents reference valid, in-bounds LBAs; check for overlapping extent ranges |
| File size check | Verify that the total logical extent coverage matches `file_size` for regular files |
| Orphan inode detection | Detect inodes with no catalog entry (`ref_count` mismatch) and offer to delete them |
| next_inode_id validation | Verify `next_inode_id` is greater than all existing inode IDs; offer to fix if stale |
| Pending dedup data blocks | Include data blocks referenced by the persisted pending insert buffer in the expected allocation bitmap |
| Free node chain | Verify `free_node_lba` chain is well-formed: walk the singly-linked list, check that `free_nodes` matches the actual chain length, and that all free-chain blocks are within bounds and marked allocated |
| Block allocation | Reconstruct expected bitmap from all on-disk structures (including free node chain blocks and persisted keyset/pending extents) and compare against on-disk bitmap |
| Last-block handling | The last dedup data block of each tree marks all `std_per_dedup` blocks as expected (see [Partial block handling](#dedup-block-lifecycle)) |
| Data block scrub | Read every data block, verify magic and checksum (handles compressed blocks) |
| Dedup data block scrub | Read every unique dedup data block, verify magic and checksum (handles compressed blocks) |
| Dedup hash verification | Walk all dedup trees, read sector data from data blocks, recompute XXH64 hash, compare against stored hash (optional, `-v`) |
| CD hash verification | Walk CD prefix/suffix/subchannel trees, recompute XXH64 from inline data, compare against stored hash (optional, `-v`) |
| Sector map dedup field scrub | Walk all sector maps, verify cached `dedup_sector_lba`/`dedup_sector_offset` match the dedup tree lookup; for CD sector maps also verify `dedup_subchannel_lba`/`dedup_subchannel_offset` against the subchannel B+Tree; offer to fix mismatches (scrub phase, `-s`) |
| Tree defragmentation | Relocate fragmented B+Tree nodes to contiguous extents (optional, `-f`); see [Tree Defragmentation](#tree-defragmentation) |

The scrub functions correctly handle both compressed and uncompressed blocks by checking the `OBMAFS3_BLOCK_FLAG_COMPRESSED` flag to determine whether to checksum `compressed_size` or `original_size` bytes.

### `defrag` — Interactive defragmenter (TUI)

An ncurses-based text user interface for interactive filesystem defragmentation.

Usage: `defrag [options] <device-or-image>`

Options:
- `-h, --help` — Show help message and exit

**User interface layout:**

| Region | Position | Content |
|--------|----------|---------|
| Menu bar | Top row | **F**ile, **A**ction, **H**elp menus (hot-key letters highlighted in yellow) |
| Block map | Middle area (fills remaining space) | Visual map of filesystem blocks coloured by state: used (white), free (dot on blue), tree nodes (magenta), dedup data (green), metadata/superblock/bitmap (cyan) |
| Status bar | Bottom row | Progress percentage, current phase label, and progress bar during analysis; key hints when idle |

**Colour scheme:** Classic DOS / Norton Utilities palette — blue desktop background, white-on-black menu bar, black-on-cyan status bar, black-on-white dialogs with white-on-green buttons.

**Startup dialog:** On launch, a modal dialog warns "You should do an fsck before starting" with two buttons:
- **OK** — Dismisses the dialog and enters the normal event loop.
- **Exit** — Terminates the application immediately.

**Key bindings:**
- `Q` / `q` — Quit the application
- `A` / `a` — Start fragmentation analysis
- `S` / `s` — Show analysis summary (after analysis completes)
- Left / Right arrows, Tab — Navigate dialog buttons
- Enter — Activate selected button
- Terminal resize is handled automatically.

**Analysis action:**

Pressing `A` launches a background analysis thread that classifies every filesystem block into one of five categories:

| Block type | Colour | Description |
|------------|--------|-------------|
| Free | White dot on blue | Not allocated in the bitmap |
| Used | White on white (solid) | Standard data block (inode data, file data) |
| Tree | Magenta on blue | B+Tree node (any tree: catalog, inode, overflow, dedup, etc.) |
| Dedup | Green on blue | Deduplicated data block |
| Meta | Cyan on black | Superblock, backup superblock, bitmap, tree headers, keyset, pending buffer |

The analysis proceeds in five phases:
1. **Bitmap** — Loads the allocation bitmap and marks metadata blocks (superblock, backup, bitmap, keyset, pending buffer).
2. **Trees** — BFS-walks every B+Tree (catalog, inode, overflow, metadata, metadata index, media tag, CD prefix/suffix/subchannel, refcount, and each per-sector-size dedup tree) plus free-node chains. Marks all nodes as Tree blocks and records per-tree node counts.
3. **Dedup** — Walks dedup tree leaves to discover dedup data block LBAs. Each dedup block spans `dedup_block_size / block_size` standard blocks, all marked as Dedup.
4. **Classify** — Scans the rest of the bitmap: any allocated block not already classified is marked as Used.
5. **Statistics** — Computes per-type block counts, free-space fragmentation, per-tree fragmentation, and dedup data fragmentation.

During analysis, the TUI refreshes every 150 ms:
- The **block map** updates live — each character cell represents a group of blocks and is coloured according to the dominant block type in that group.
- The **status bar** shows the current phase label, a percentage, and a visual progress bar.

When analysis completes, a **summary dialog** is presented showing:
- Total, free, used, tree, dedup, and metadata block counts with percentages.
- Free-space fragmentation percentage (based on contiguous free runs).
- Per-tree fragmentation (name, percentage, node count) for every B+Tree.
- Dedup data fragmentation percentage.

*Fragmentation formula*: `frag% = (runs − 1) / (total_items − 1) × 100`. A single contiguous extent = 0 %; every block isolated = 100 %.

**Compaction action (press `C` after analysis):**

Compaction moves all allocated blocks to eliminate free-space fragmentation and arrange data for optimal sequential access. It runs in five phases:

| Phase | Description | What moves where |
|-------|-------------|------------------|
| 1 — Trees | All B+Tree nodes | → end of disk, each tree contiguous, with 64-block clump gaps |
| 2 — Data blocks | Inode data extents (inline + overflow) | → beginning of disk, moved as complete extent units |
| 3 — Dedup blocks | Deduplicated data blocks | → immediately after data blocks |
| 4 — References | Dedup tree entries, SME caches, subchannel LBAs | Updated in place using relocation maps |
| 5 — Flush | Bitmap + superblock | Final write to disk |

**Phase 1 — Tree relocation:** Trees are relocated FIRST so their nodes are out of the way when data and dedup blocks are compacted. Each tree's nodes are read entirely into memory, all internal `child_lba`, `left_link`/`right_link` sibling pointers, and free-chain `next_free` pointers are rewritten using an O(1) hash map, then all nodes are written to a contiguous range at the disk end. Tree headers are updated via `obmafs3_btree_header_write()` which auto-computes checksums. Node checksums are computed over `sizeof(btree_node_header) + keys_length` (not the full block), matching the read-path verification. A tree node relocation map is recorded for subchannel LBA fixup in CD SMEs.

**Phase 2 — Inode-aware data compaction:** Data blocks are moved as **complete extent units** — entire `extent_run` ranges are copied atomically via bulk `pread`/`pwrite`, preserving the compressed data inside. The `start_block` field is updated directly in the inode tree leaf node (or overflow tree leaf for overflow extents). Overflow extents are collected in a two-pass approach: (1) BFS-collect all overflow extents with their node LBA and record index, (2) sort by `start_block` for monotonic cursor advancement, (3) process in LBA order, (4) batch-write dirty leaf nodes. When dedup blocks obstruct the target range, the ENTIRE dedup data block is evicted as one unit — the block_header at the base is read to determine actual physical size, and all physical blocks are moved together. Evicted dedup blocks are recorded in a relocation map.

**Phase 3 — Dedup block compaction:** Dedup data blocks are packed contiguously after the data region. Each dedup block's physical size is determined by reading its `block_header.compressed_size`, not by contiguous `BT_DEDUP` runs. Per-block relocation entries are recorded for every moved block.

**Phase 4 — Reference updates:** Three types of references are updated using relocation maps:
1. **Dedup tree entries** (`dedup_entry.block_lba`) — BFS walks all dedup tree leaves, updates `block_lba` using the dedup reloc map, recomputes node checksums.
2. **Sector map entry caches** (`sector_map_entry.dedup_sector_lba` / `cd_sector_map_entry.dedup_sector_lba`) — The ENTIRE sector map for each media-image inode is loaded into a contiguous memory buffer (reading all extents — inline + overflow — decompressing as needed), all SME entries are updated in a single pass, then the buffer is recompressed and written back. This avoids alignment issues from 34-byte packed SME entries spanning block/extent boundaries.
3. **CD subchannel LBAs** (`cd_sector_map_entry.dedup_subchannel_lba`) — Updated using the tree relocation map recorded during Phase 1.

Relocation chains (A→B during eviction, B→C during compaction) are resolved to direct A→C mappings before building the final lookup maps.

**Crash safety:** Block moves follow a write-first, update-references, sync, update-bitmap, sync sequence. Bitmap writes and syncs are batched (every 256 inode tree leaves or 512 overflow extents) to minimise `fdatasync` calls. Safe cancellation via ESC flushes all pending work (including reference updates) before stopping.

**Performance optimisations:**
- Bulk `pread`/`pwrite` with a reusable 16 MB I/O buffer and retry on short reads/writes
- `posix_fadvise(FADV_WILLNEED)` prefetch in batches of 64
- Level-order BFS with sorted LBAs for sequential I/O in all tree traversals
- Eviction destination cached via decreasing cursor (amortised O(1))
- Bitmap flush + `fdatasync` batched every 256 leaves / 512 extents
- Extents already in optimal position are skipped without I/O
- Whole dedup data block eviction (block_header-based sizing, not BT_DEDUP run counting)

**Status bar during compaction:** Shows the current phase label, completion percentage, current block move (`src→dst`), elapsed time, and estimated remaining time. A `Status` panel in the bottom-left shows detailed operation info. The block map shows `r` (reading) and `W` (writing) position markers during compaction.

**Key bindings:**
- `A` / `a` — Start fragmentation analysis
- `S` / `s` — Show analysis summary (after analysis completes)
- `C` / `c` — Start compaction (after analysis completes)
- `ESC` — Safely stop compaction (flushes all pending work including reference updates)
- `Q` / `q` — Quit (safely stops compaction first if running)
- Left / Right arrows, Tab — Navigate dialog buttons
- Enter — Activate selected button
- Terminal resize is handled automatically.

**Implementation notes:** Built with ncursesw (wide-character ncurses). Both the analysis and compaction engines run in detached `pthread`s and communicate with the TUI via lock-free `_Atomic` counters. The block map view dynamically adapts to terminal size — each cell aggregates `ceil(total_blocks / usable_cells)` blocks and displays the dominant type using solid colored blocks (Norton Speed Disk style, with U+2592 MEDIUM SHADE for free space). The device is opened with raw `open(O_RDWR)` for exclusive offline access — no node cache, keyset, DLC, or housekeeping threads are started.

### `libobmafs` — Static library

Provides the C API for all filesystem operations. Used by all three tools above.

**API categories:**
- Context management: `obmafs3_open`, `obmafs3_open_flags`, `obmafs3_close`
- Superblock: `obmafs3_sb_read`, `obmafs3_sb_read_lenient`, `obmafs3_sb_write`, `obmafs3_sb_validate`, `obmafs3_sb_read_backup`, `obmafs3_sb_read_backup_lenient`
- Block I/O: `obmafs3_block_read`, `obmafs3_block_write`
- B+Tree: `obmafs3_btree_header_read`, `obmafs3_btree_header_read_lenient`, `obmafs3_btree_header_write`, catalog lookup/list/insert/delete, inode get/put/delete
- Allocation: `obmafs3_alloc_block`, `obmafs3_alloc_blocks`, `obmafs3_free_block`, `obmafs3_free_blocks`, `obmafs3_alloc_inode_id`
- B+Tree node allocation: `obmafs3_btree_alloc_node`, `obmafs3_btree_free_node`
- Bitmap: `obmafs3_bitmap_read`, `obmafs3_bitmap_write`, `obmafs3_bitmap_set`, `obmafs3_bitmap_clear`, `obmafs3_bitmap_is_set`, `obmafs3_bitmap_find_free`
- Catalog: `obmafs3_catalog_lookup`, `obmafs3_catalog_list`, `obmafs3_catalog_list_free`, `obmafs3_catalog_insert`, `obmafs3_catalog_delete`
- Inode: `obmafs3_inode_get`, `obmafs3_inode_put`, `obmafs3_inode_delete`
- File data: `obmafs3_read_file_data`, `obmafs3_write_file_data`
- Clone/reflink: `obmafs3_clone_file_range`, `obmafs3_free_file_blocks`, `obmafs3_truncate_file_blocks`
- Refcount: `obmafs3_refcount_get`, `obmafs3_refcount_set`, `obmafs3_refcount_inc`, `obmafs3_refcount_dec`
- Dedup: `obmafs3_dedup_get_tree`, `obmafs3_dedup_lookup`, `obmafs3_write_media_image_data`, `obmafs3_read_media_image_data`, `obmafs3_read_cd_image_data`, `obmafs3_read_subchannel_data`
- Dedup block cache: `obmafs3_flush_dedup_block_cache`, `obmafs3_free_dedup_block_cache`, `obmafs3_dedup_node_cache_init`, `obmafs3_dedup_node_cache_free`, `obmafs3_dedup_key_set_free`
- Media read caches: `obmafs3_alloc_media_leaf_cache`, `obmafs3_free_media_leaf_cache`, `obmafs3_alloc_media_dedup_cache`, `obmafs3_free_media_dedup_cache`

### Dedup Leaf Cache

The **dedup leaf cache** (`struct dedup_leaf_cache`) is a stack-allocated per-call cache that stores the last accessed B+Tree leaf node. It is used by the media image read path to avoid redundant B+Tree traversals when reading sequential sectors whose hashes fall in the same leaf.

```c
struct dedup_leaf_cache {
    uint8_t *leaf_buf;    /* Cached leaf node data (block_size bytes) */
    uint16_t num_keys;    /* Number of keys in the cached leaf */
    uint64_t min_key;     /* Minimum hash key in the cached leaf */
    uint64_t max_key;     /* Maximum hash key in the cached leaf */
};
```

When `dedup_lookup_cached()` is called:
1. If the target hash falls within `[min_key, max_key]`, a binary search is performed directly on the cached leaf buffer, avoiding any I/O.
2. On a miss, the standard B+Tree traversal runs and the leaf cache is re-populated with the new leaf via `dedup_leaf_cache_populate()`.

The companion `dedup_readahead_next()` function pre-fetches the next leaf when the current lookup lands near the boundary of the cached leaf, so the next sequential lookup is nearly free. Combined with the global [dedup lookup cache](#dedup-lookup-cache), this gives the read path three levels of caching: lookup cache (global, thread-safe) → leaf cache (per-call, zero-copy) → B+Tree traversal (cold path).
- Dedup key set: `obmafs3_dedup_keyset_save`, `obmafs3_dedup_keyset_load`
- Dedup pending buffer: `obmafs3_dedup_pending_save`, `obmafs3_dedup_pending_load`, `obmafs3_dedup_pending_flush_and_free`
- Dedup warmup: `obmafs3_dedup_warmup_start`, `obmafs3_dedup_warmup_wait`
- Housekeeping: `obmafs3_housekeeping_start`, `obmafs3_housekeeping_stop`
- Compression pool: `obmafs3_compress_pool_init`, `obmafs3_compress_pool_reinit`, `obmafs3_compress_pool_destroy`
- Async pool jobs: `obmafs3_pool_async_job_create`, `obmafs3_pool_async_job_free`, `obmafs3_pool_submit_async`, `obmafs3_pool_wait_async`
- Thread-local buffers: `obmafs3_get_thread_bufs`
- Sector map cache: `obmafs3_flush_sector_map_cache`, `obmafs3_free_sector_map_cache`
- CD sector map cache: `obmafs3_flush_cd_sector_map_cache`, `obmafs3_free_cd_sector_map_cache`
- Media tags: `obmafs3_media_tag_get`, `obmafs3_media_tag_data_free`, `obmafs3_media_tag_put`, `obmafs3_media_tag_delete`, `obmafs3_media_tag_delete_all`, `obmafs3_media_tag_list`, `obmafs3_media_tag_list_free`
- Image metadata: `obmafs3_metadata_get`, `obmafs3_metadata_put`, `obmafs3_metadata_delete`, `obmafs3_metadata_delete_all`, `obmafs3_metadata_list`, `obmafs3_metadata_list_free`, `obmafs3_metadata_query`, `obmafs3_metadata_query_filtered`, `obmafs3_metadata_query_free`
- CD B+Trees: `obmafs3_cd_prefix_get/put/delete`, `obmafs3_cd_suffix_get/put/delete`, `obmafs3_cd_subchannel_get/get_location/put/delete`
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
#define OBMAFS3_ERR_REVISION -9  /* Filesystem revision is newer than supported */
#define OBMAFS3_ERR_INCOMPAT -10 /* Incompatible feature flags set */
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

| Library | Purpose | Used by |
|---------|---------|--------|
| xxHash  | XXH64 checksums for all integrity verification | libobmafs |
| ZSTD    | Zstandard compression for data blocks | libobmafs |
| libfuse3| FUSE 3 user-space filesystem interface | mount.obmafs only |
| libaaruformat | Read Aaru Image Format (.aif) disk image files | import-aif only |

xxHash, ZSTD, and libaaruformat are fetched automatically via CMake `FetchContent` at build time. libfuse3 is a system dependency located via `pkg-config`.

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
| Node cache memory cap and eviction | Complete |
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
| `import-aif` (Aaru Image Format importer) | Complete |
| Filesystem repair in `obmafsck` | Partial (superblock + backup, superblock fields, B+Tree ordering/siblings/checksums/free-node-chain, bitmap, refcounts, orphan inodes, metadata bidirectional consistency) |
| B+Tree defragmentation in `obmafsck` | Complete |
| Clump allocation (HFS+-style B+Tree growth) | Complete |
| Dedup key set (O(1) existence check) | Complete |
| Pending insert buffer (deferred B+Tree inserts) | Complete |
| Background housekeeping thread | Complete |
| Background warmup thread | Complete |
| Persisted key set / pending buffer | Complete |
| Rename / move | Complete |
| `import-aif` CD cue sheet generation | Complete |
| `import-aif` sidecar file export (CICM XML, Aaru JSON, dump hardware) | Complete |
| `defrag` (interactive TUI defragmenter) | Analysis complete, compaction engine implemented |

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

---

## Nintendo GameCube / Wii Disc Image Support

OBMAFS3 supports importing plain Nintendo GameCube and Wii ISO disc images with LFG (Lagged Fibonacci Generator) junk detection and removal, and — for Wii — AES-128-CBC decryption on import with re-encryption on read, producing byte-identical output.

### Overview

Nintendo GameCube and Wii discs fill unused sectors with deterministic pseudo-random data generated by a Lagged Fibonacci Generator (LFG, K=521, J=32, XOR). This "junk" data is ~86–92% of a typical disc. By detecting and removing it during import, OBMAFS3 stores only the actual game data plus small per-block LFG seeds, dramatically reducing storage.

Wii discs additionally encrypt partition data with AES-128-CBC. The import tool decrypts on ingest and the FUSE read path re-encrypts on read, so the file presented via FUSE is byte-identical to the original ISO.

### On-Disk Additions

**New enumeration values:**

```c
kBtreeTypeJunkMap       = 13   /* Junk Map B+Tree */
kBtreeDataTypeJunkMapEntry = 13
kFileTypeNintendo       = 6    /* Nintendo GC/Wii disc image */
kSectorMapTypeNintendo  = 2    /* Nintendo disc sector map */
```

**New superblock field** (in the extension area):

```c
uint64_t junk_map_lba;   /* LBA of the Junk Map B+Tree header (0 = none) */
```

**New rocompat flag:**

```c
OBMAFS3_ROCOMPAT_NINTENDO  (1 << 1)   /* Nintendo GC/Wii disc image support */
```

### Junk Map B+Tree (`kBtreeTypeJunkMap = 13`)

Stores the LFG seeds needed to regenerate junk sectors. Keyed by `(inode_id, offset)`.

**Leaf record** (94 bytes):

```c
struct junk_map_record {
    uint64_t inode_id;                     /* Image inode */
    uint64_t offset;                       /* Disc offset (bytes) */
    uint64_t length;                       /* Junk region length in bytes */
    uint16_t partition_index;              /* Partition index (0xFFFF = GC / outside partitions) */
    uint32_t seed[NGC_LFG_SEED_SIZE];     /* 17-word LFG seed (big-endian u32) */
};
```

**Index entry** (24 bytes):

```c
struct junk_map_index_entry {
    uint64_t inode_id;
    uint64_t offset;
    uint64_t child_lba;
};
```

Lookup is range-based: given `(inode_id, offset)`, the B+Tree returns the record whose `[offset, offset+length)` range covers the query.

### AES-128-CBC for Wii Partitions

Each Wii encrypted block (0x8000 bytes) consists of a 0x400-byte hash block followed by 0x7C00 bytes of user data.

**Decryption** (`ngc_wii_decrypt_group`):
1. Hash block: AES-CBC decrypt with IV = 16 zero bytes.
2. User data: AES-CBC decrypt with IV = bytes 0x3D0–0x3DF of the **encrypted input** (not the decrypted hash block). This matches Dolphin's `DecryptBlockData`.

**Encryption** (`ngc_wii_encrypt_group`):
1. Hash block: AES-CBC encrypt with IV = 16 zero bytes.
2. User data: AES-CBC encrypt with IV = bytes 0x3D0–0x3DF of the **encrypted output** (just written by step 1).

Title keys are derived from the partition ticket using the Wii common key via standard AES-128-CBC decryption.

### LFG Junk Detection (Import)

The LFG generates a disc-wide pseudo-random stream seeded by the disc ID. `ngc_lfg_get_seed()` extracts a per-block seed by reversing the LFG state. A "block" in LFG terms is a 0x8000-byte aligned chunk of the continuous decrypted user data stream (matching Dolphin's `BLOCK_TOTAL_SIZE`). The `data_offset` parameter is `stream_position % 0x8000`, keeping backward/forward cycles bounded to ≤16.

**Per-group detection (Wii):**

Each 0x7C00-byte decrypted group may span two 0x8000-aligned LFG blocks. The import extracts up to two seeds (one per block) from the first free sector in each block. Each free sector is then individually verified by regenerating the expected LFG bytes at `(logical_offset + sector_offset) % 0x8000` and comparing via `memcmp`. Sectors that match are zeroed and recorded in the Junk Map B+Tree; sectors that don't match are stored verbatim.

**Per-block detection (GC):**

GameCube images are processed in 32 KB blocks. The entire block is tested with `ngc_lfg_get_seed(data, 0x8000, 0, seed)`. If it matches, each non-data sector (classified by FST) is zeroed and a junk entry recorded.

**FST-based classification:**

The partition's File System Table (FST) is parsed to build a sorted data map of file regions. Sectors within the system area (boot header through FST) or overlapping a file region are classified as data; all others are candidates for junk detection. For Wii, FST offsets are word-shifted (`<< 2`); the FST size is also word-shifted.

### Read Path (Junk Reconstruction)

**GameCube:** `read_gc()` reads stored data via dedup, then for each sector with a junk map entry, generates LFG bytes at position `sector_offset - jrec.offset` (entry-relative advance) and overwrites the stored zeros.

**Wii:** `reconstruct_group()`:
1. Reads the full decrypted group (hash block + user data) from dedup storage.
2. For each sector with a junk map entry, generates LFG bytes at position `(group_idx * WII_GROUP_DATA_SIZE + off) % WII_GROUP_SIZE` and fills in the user data.
3. Re-encrypts the full group via `ngc_wii_encrypt_group()`, producing the original encrypted block.

The read path for Wii also handles inter-partition unencrypted regions (disc header, partition table, inter-partition gaps) which are read directly from dedup.

### Partition Metadata

Partition layout is stored as key-value metadata entries on the inode:

| Key | Value |
|-----|-------|
| `__ngc_disc_type__` | `0` = GameCube, `1` = Wii |
| `__ngc_disc_size__` | Total disc size in bytes |
| `__ngc_part_count__` | Number of Wii partitions |
| `__ngc_part_N_data_offset__` | Partition N data area disc offset |
| `__ngc_part_N_data_size__` | Partition N data area size |
| `__ngc_part_N_title_key__` | Partition N AES title key (32 hex chars) |

### Embedded Cryptography

Two standalone implementations are included to avoid external crypto dependencies:

- **AES-128** (`src/lib/aes128.c`): FIPS 197 AES-128 encrypt/decrypt with CBC mode. Used for Wii partition decryption/re-encryption and title key derivation.
- **SHA-1** (`src/lib/sha1.c`): Used for potential hash verification (Wii H0/H1/H2 hash trees).

### `import-ngcw` — Nintendo GameCube/Wii disc importer

Imports a plain GameCube or Wii ISO disc image into a mounted OBMAFS3 filesystem. Communicates via `open()`, `write()`, and `ioctl()` — does not link `libobmafs`.

Usage: `import-ngcw <input.iso> <output-path-on-obmafs>`

**Import pipeline:**

1. **Phase 1:** Opens the ISO file, reads the disc header (0x440 bytes), detects disc type (GC magic `0xC2339F3D` at offset 0x1C, Wii magic `0x5D1C9EA3` at offset 0x18).
2. **Phase 2 (Wii only):** Reads the partition table, parses ticket/TMD for each partition, derives title keys.
3. **Phase 3:** Creates the output file, sets it as `kFileTypeNintendo` via `OBMAFS3_IOC_SET_NINTENDO_IMAGE` ioctl (carries disc type, disc size, partition count, per-partition data offset/size/title key).
4. **Phase 4:** Linear disc walk — writes sector data:
   - **Outside partitions:** 2048-byte sectors written verbatim (inter-partition junk detected via the outer-disc LFG).
   - **Inside Wii partition data:** 0x8000-byte groups decrypted, junk detected per-sector with two-seed block approach, junk sectors zeroed, hash block stored verbatim, written as 0x8000 bytes.
   - **GC sectors:** 32 KB blocks, per-sector junk classification via FST data map.
5. **Phase 5:** Stores accumulated junk entries in the Junk Map B+Tree via `OBMAFS3_IOC_ADD_JUNK_ENTRY` ioctl.
6. **Phase 6:** Imports BCA (Burst Cutting Area) sidecar file if present (`.bca` extension, 64 bytes), stored as a media tag.
7. **Phase 7:** Stores partition metadata as key-value pairs.

**Source files:**

| File | Contents |
|------|----------|
| `import_ngcw.h` | Shared header with all includes and declarations |
| `main.c` | Entry point, argument parsing, phase orchestration, ANSI-colored TUI |
| `disc.c` | ISO opening, disc type detection, disc info printing |
| `partition.c` | Wii partition table parsing, ticket/TMD reading, title key derivation |
| `import.c` | Sector import for GC (`ngcw_import_gc`) and Wii (`ngcw_import_wii`) with two-seed LFG junk detection |
| `metadata.c` | Partition metadata storage |
| `junk_map.c` | Junk collector (in-memory accumulator with merge) and B+Tree storage via ioctl |

**Dependencies:** Links only against `libobmafs` (for `nintendo.h` / LFG / AES). Includes OBMAFS3 headers for ioctl definitions.

### LFG Implementation (`src/lib/nintendo.c`)

The Lagged Fibonacci Generator is a port of Dolphin Emulator's implementation (CC0-licensed):

- **Parameters:** K=521, J=32, XOR operation
- **Seed size:** 17 u32 words (NGC_LFG_SEED_SIZE)
- **Key functions:**
  - `ngc_lfg_get_seed(data, size, data_offset, seed_out)` — Extract seed from data at a known stream position. Returns number of matched bytes.
  - `ngc_lfg_set_seed(ctx, seed)` — Initialize LFG from a saved seed.
  - `ngc_lfg_get_bytes(ctx, out, count)` — Generate output bytes.
- **Sanity check:** Before attempting full seed recovery, checks that the first 521 u32 words satisfy `(x & 0x00C00000) == (x >> 2 & 0x00C00000)` — a bit pattern invariant from the Initialize shift-by-18-instead-of-16 quirk.

### Integration with fsck and defrag

- **obmafsck:** Recognizes `kBtreeTypeJunkMap` (type 13). Validates the B+Tree structure, collects free-chain blocks, and includes junk map blocks in the expected bitmap.
- **defrag:** Analysis phase scans junk map B+Tree blocks. Compaction phase handles junk map tree relocation. The junk map tree uses single-block nodes (JUNK_MAP_NODE_BLOCKS = 1).
