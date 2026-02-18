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
    uint64_t next_free_lba;      /* Hint: next LBA to try for allocation */
    uint64_t next_inode_id;      /* Next available inode ID */
    uint64_t bitmap_lba;         /* LBA of the first allocation bitmap block (block 14) */
    uint64_t bitmap_blocks;      /* Number of blocks used by the allocation bitmap */
    uint8_t  volume_label[256];  /* Volume label, UTF-8, NUL-terminated */
};
```

The superblock identifies the filesystem, stores global parameters, and provides the LBAs for all top-level structures. The root inode ID is always 2 (`OBMAFS3_ROOT_INODE_ID`), and `next_inode_id` starts at 3 after creation.

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
- `next_free_lba` — Allocation hint; tracks the highest allocated LBA to speed up sequential allocations.
- `bitmap_lba`, `bitmap_blocks` — Location and size of the allocation bitmap on disk.

---

## Allocation Bitmap

The allocation bitmap tracks which blocks are in use. Each bit corresponds to one standard block (defined by `block_size`). A bit value of 1 means allocated, 0 means free. Deduplication blocks that span multiple standard blocks (when `dedup_block_size` is a multiple of `block_size`, e.g. 1024 blocks for default sizes) occupy that many consecutive bits.

The first bitmap block begins with a header:

```c
struct bitmap_header {                       /* packed */
    uint64_t magic;          /* "OBMABMAP" (0x50414D42414D424F) */
    uint64_t total_blocks;   /* Total number of blocks tracked by the bitmap */
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
struct btree_node_header {                   /* packed, 69 bytes */
    uint64_t magic;          /* "BTREENDE" (0x45444E4545525442) */
    uint8_t  record_type;    /* Type of records in this node */
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
    kBtreeTypeRefcount      = 10
};

enum obmafs3_btree_data_type {
    kBtreeDataTypeFilename           = 0,
    kBtreeDataTypeInode              = 1,
    kBtreeDataTypeExtent             = 2,
    kBtreeDataTypeDeduplicationEntry = 3,
    kBtreeDataTypeMetadataEntry      = 4,
    kBtreeDataTypeMediaTagEntry      = 5,
    kBtreeDataTypeRefcountEntry      = 10
};

enum obmafs3_file_type {
    kFileTypeRegular    = 0,
    kFileTypeDirectory  = 1,
    kFileTypeMediaImage = 2
};
```

### Catalog Tree (directory entries)

```c
struct btree_node_filename {                 /* packed */
    struct btree_node_header header;
    uint64_t inode_id;       /* Unique identifier for the file or directory */
    uint64_t parent_id;      /* Identifier of the parent directory (root = 2) */
    uint8_t  directory_flag; /* 1 if directory, 0 if file */
    char     name[256];      /* Name in UTF-8, NUL-terminated */
};
```

### Inode Tree (file metadata)

The inode tree is a proper B+Tree: leaf nodes (level 0) store packed `inode_record` entries sorted by `inode_id`, and index nodes (level > 0) store `btree_index_entry` entries pointing to child nodes. Each leaf node can hold up to `(block_size - sizeof(btree_node_header)) / sizeof(inode_record)` records (20 records for a 4096-byte block).

```c
struct inode_record {                        /* packed, 201 bytes */
    uint64_t inode_id;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;               /* POSIX file permissions */
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t access_time;
    uint64_t file_size;          /* File size in bytes */
    struct extent_run extents[8];/* Up to 8 inline extent runs */
    uint8_t  file_type;          /* 0=regular, 1=directory, 2=media image */
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
struct extent_run {                          /* packed, 16 bytes */
    uint64_t start_block;    /* Starting block of the extent */
    uint64_t block_count;    /* Number of contiguous blocks */
};
```

If a file requires more than 8 extents, additional extents are stored in the Overflow Tree.

### Overflow Tree (extra extents)

The Overflow Tree is a B+Tree that stores additional extent runs for files that exceed the 8 inline extents available in the inode. Leaf nodes contain sorted `overflow_extent` records; index nodes use `btree_index_entry` to route lookups by `inode_id`.

```c
struct overflow_extent {                     /* packed, 24 bytes */
    uint64_t inode_id;       /* Inode this extent belongs to */
    uint64_t start_block;    /* Starting block of the extent run */
    uint64_t block_count;    /* Number of blocks in the extent run */
};
```

Overflow entries are sorted by the composite key `(inode_id, start_block)`. Maximum records per leaf node with a 4096-byte block: (4096 − 70) / 24 = **167 entries**. Maximum index entries per node: (4096 − 70) / 16 = **251 entries**.

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
struct media_tag_record {                    /* packed */
    uint64_t inode_id;       /* Inode this tag belongs to */
    uint16_t tag_type;       /* MediaTagType enum value */
    uint32_t data_length;    /* Length of tag data in bytes */
    uint64_t data_lba;       /* LBA of the first data block (for large tags) */
    uint16_t data_blocks;    /* Number of blocks used by data (0 = inline) */
    /* If data_blocks == 0, data follows inline after the header */
};
```

Small tags are stored inline within the leaf record. Large tags that exceed inline capacity are stored in separate data blocks referenced by `data_lba`.

Tag types include: CD TOC, CD session info, CD full TOC, CD PMA, CD ATIP, CD-TEXT, CD MCN, DVD PFI, DVD CMI, DVD disc key, DVD BCA, DVD DMI, and many others.

### Metadata B+Tree (image key-value pairs)

The Metadata B+Tree stores arbitrary key-value string pairs associated with disk images (e.g., dumper name, dump date, serial number). Uses 8-block nodes (`METADATA_NODE_BLOCKS = 8`) because records are large.

```c
struct metadata_record {                     /* packed */
    uint64_t inode_id;                   /* Inode this entry belongs to */
    char     key[256];                   /* Metadata key (NUL-terminated, max 255 chars) */
    char     value[1025];                /* Metadata value (NUL-terminated, max 1024 chars) */
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
```

Both metadata trees use 8-block nodes and support full CRUD operations plus paginated key listing and reverse queries.

### CD Prefix / Suffix / Subchannel B+Trees

Three B+Trees store deduplicated CD raw sector components. All three share identical logic: a `uint64_t` hash key with fixed-size inline data.

```c
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

7. **Background compression**: When enabled, a background worker thread compresses dedup data blocks in parallel with write I/O (`obmafs3_bg_compress_start`/`obmafs3_bg_compress_stop`). The writer fills the in-memory buffer; once full, the worker compresses and writes it to disk while the writer starts filling a new buffer.

8. **Dedup B+Tree node cache**: Frequently accessed B+Tree nodes are cached in memory during dedup writes, reducing disk reads during hash lookups and insertions.

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
- The superblock (not stored in-band; validated by magic and field consistency)
- B+Tree headers (`btree_header.checksum`)
- B+Tree nodes (`btree_node_header.checksum`)
- Data blocks (`block_header.checksum`)
- Bitmap data (`bitmap_header.checksum`)
- Tree list header (`tree_list_header.checksum`)

---

## Compression

The only supported compression algorithm is **ZSTD** (Zstandard). The compression level is configurable (default: 15). Compression is applied to:

- Regular file data blocks (when `--compress` is used on mount)
- Dedup data blocks (when compression is enabled)

Compression is transparent: the read path checks the `flags` field of each `block_header` and decompresses when needed.

---

## Tools

### `mkobmafs` — Create filesystem

Creates a new empty OBMAFS v3 filesystem.

Usage: `mkobmafs -s <size_bytes> -l <label> <path>`

Writes: superblock, catalog tree (header + root node with root directory entry), inode tree (header + root inode), overflow tree (header, empty), dedup tree list (header, empty), media tag tree (header, empty), CD prefix/suffix/subchannel trees (headers, empty), metadata tree (header, empty), metadata index tree (header, empty), refcount tree (header, empty), and allocation bitmap.

### `mount.obmafs` — FUSE mount

Mounts an OBMAFS v3 filesystem via FUSE 3.

Usage: `mount.obmafs --device=<path> <mountpoint> [options]`

Options:
- `--compress` — Enable ZSTD compression for writes
- `--zstd-level=N` — Set ZSTD compression level (default: 15)

**Supported FUSE operations:**
| Operation | Description |
|-----------|-------------|
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

**Per-file-handle context** (`fuse_file_ctx`):
- `inode_id` — Cached inode ID
- `sector_size` — Detected sector size (0 for regular files)
- `inode` — Cached inode structure (write-back on release)
- `inode_dirty` — Flag indicating the cached inode needs write-back
- `sme_cache` — In-memory sector map entry cache (flushed on release)
- `cd_sme_cache` — In-memory CD sector map entry cache (flushed on release)
- `dedup_block_cache` — Persistent dedup data block accumulator (avoids re-reading 4 MiB blocks)
- `bg_compress` — Background compression worker context (compresses dedup blocks in parallel)

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

**Media image detection**: Files with recognized disk image extensions (`.iso`, `.img`, `.bin`, `.raw`, etc.) are automatically created as `kFileTypeMediaImage`. The sector size is auto-detected from the file extension (e.g., 2048 for `.iso`, 512 for `.img`).

### `obmafsck` — Filesystem checker

Checks and verifies OBMAFS v3 filesystem integrity.

Usage: `obmafsck [-n] [-s] <path>`

Options:
- `-n` — No-fix mode (report errors only, do not modify)
- `-s` — Run data block scrub (verify all block checksums)

**Checks performed:**

| Check | Description |
|-------|-------------|
| Superblock validation | Magic, block sizes, LBA consistency |
| Allocation bitmap | Load and verify bitmap checksum |
| Catalog tree | Traverse all nodes, verify magic and checksums |
| Inode tree | Traverse all nodes, verify magic and checksums |
| Overflow tree | Traverse all nodes, verify magic and checksums |
| Refcount tree | Traverse all nodes, verify magic and checksums |
| Media tag tree | Traverse all nodes, verify magic and checksums |
| Metadata tree | Traverse all nodes, verify magic and checksums |
| Metadata index tree | Traverse all nodes, verify magic and checksums |
| CD prefix/suffix/subchannel trees | Traverse all nodes, verify magic and checksums |
| Dedup tree list | Verify list header, traverse all per-sector-size trees |
| Cross-reference | Verify all catalog entries have valid inodes |
| Block allocation | Verify all referenced blocks are marked allocated in bitmap |
| Data block scrub | Read every data block, verify magic and checksum (handles compressed blocks) |
| Dedup data block scrub | Read every unique dedup data block, verify magic and checksum (handles compressed blocks) |

The scrub functions correctly handle both compressed and uncompressed blocks by checking the `OBMAFS3_BLOCK_FLAG_COMPRESSED` flag to determine whether to checksum `compressed_size` or `original_size` bytes.

### `libobmafs` — Static library

Provides the C API for all filesystem operations. Used by all three tools above.

**API categories:**
- Context management: `obmafs3_open`, `obmafs3_open_flags`, `obmafs3_close`
- Superblock: `obmafs3_sb_read`, `obmafs3_sb_write`, `obmafs3_sb_validate`
- Block I/O: `obmafs3_block_read`, `obmafs3_block_write`
- B+Tree: header read/write, catalog lookup/list/insert/delete, inode get/put/delete
- Allocation: `obmafs3_alloc_block`, `obmafs3_alloc_blocks`, `obmafs3_free_block`, `obmafs3_free_blocks`, `obmafs3_alloc_inode_id`
- Bitmap: read/write/set/clear/is_set/find_free
- File data: `obmafs3_read_file_data`, `obmafs3_write_file_data`
- Clone/reflink: `obmafs3_clone_file_range`, `obmafs3_free_file_blocks`, `obmafs3_truncate_file_blocks`
- Refcount: `obmafs3_refcount_get`, `obmafs3_refcount_set`, `obmafs3_refcount_inc`, `obmafs3_refcount_dec`
- Dedup: `obmafs3_dedup_get_tree`, `obmafs3_dedup_lookup`, `obmafs3_write_media_image_data`, `obmafs3_read_media_image_data`
- Dedup block cache: `obmafs3_flush_dedup_block_cache`, `obmafs3_free_dedup_block_cache`, `obmafs3_bg_compress_start`, `obmafs3_bg_compress_stop`
- Sector map cache: `obmafs3_flush_sector_map_cache`, `obmafs3_free_sector_map_cache`
- CD sector map cache: `obmafs3_flush_cd_sector_map_cache`, `obmafs3_free_cd_sector_map_cache`
- Media tags: `obmafs3_media_tag_get`, `obmafs3_media_tag_put`, `obmafs3_media_tag_delete`, `obmafs3_media_tag_delete_all`, `obmafs3_media_tag_list`
- Image metadata: `obmafs3_metadata_get`, `obmafs3_metadata_put`, `obmafs3_metadata_delete`, `obmafs3_metadata_delete_all`, `obmafs3_metadata_list`, `obmafs3_metadata_query`
- CD B+Trees: `obmafs3_cd_prefix_get/put/delete`, `obmafs3_cd_suffix_get/put/delete`, `obmafs3_cd_subchannel_get/put/delete`
- ECC/EDC: `ecc_cd_init`, `ecc_cd_free`, `ecc_cd_is_suffix_correct`, `ecc_cd_reconstruct`
- Checksum: `obmafs3_checksum_xxh64`, `obmafs3_checksum_block`
- Compression: `obmafs3_compress`, `obmafs3_decompress`
- Filesystem creation: `obmafs3_create`
- Path resolution: `obmafs3_resolve_inode_path`

Open flags:
- `OBMAFS3_OPEN_SKIP_BITMAP` — Do not load/validate bitmap (for quick header-only checks)
- `OBMAFS3_OPEN_LENIENT` — Tolerate checksum errors (used by fsck)

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
| Background dedup compression | Complete |
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
| Filesystem repair in `obmafsck` | Not implemented (check-only) |
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
