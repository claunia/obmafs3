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
| 7 .. 7+N-1          | Allocation bitmap                    | `OBMABMAP` (0x50414D42414D424F) |
| 7+N ..              | Data blocks, B+Tree nodes, dedup data| `OBMABLCK` / `BTREENDE` |

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
    uint64_t metadata_lba;       /* LBA of the metadata B+Tree header (reserved, 0) */
    uint64_t media_tag_lba;      /* LBA of the media tag B+Tree header (reserved, 0) */
    uint16_t checksum_type;      /* Checksum algorithm (0 = XXH64) */
    uint64_t creation_time;      /* Unix timestamp of filesystem creation */
    uint64_t next_free_lba;      /* Hint: next LBA to try for allocation */
    uint64_t next_inode_id;      /* Next available inode ID */
    uint64_t bitmap_lba;         /* LBA of the first allocation bitmap block (block 7) */
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
- `metadata_lba` — Reserved for a future Metadata Tree (arbitrary key-value pairs about disk images). Currently 0.
- `media_tag_lba` — Reserved for a future Media Tag Tree (media-specific tags for disk images). Currently 0.
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
struct inode_record {                        /* packed, 197 bytes */
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
};
```

The `extents` array holds up to 8 inline extent runs. For regular files, these point to data blocks. For media image files, they point to blocks containing a flat array of `sector_map_entry` structures that map each sector to its deduplicated copy. The `sector_count` and `sector_map_size` fields are only used for media image files.

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

### Deduplication Tree (sector hash lookup)

Deduplication tree nodes store entries after the standard node header. Each entry maps a sector hash to its physical location:

```c
struct dedup_entry {                         /* packed, 24 bytes */
    uint64_t hash;           /* XXH64 hash of the sector data */
    uint64_t block_lba;      /* LBA of the dedup data block containing this sector */
    uint64_t block_offset;   /* Byte offset within the block where sector data starts */
};
```

Maximum entries per node with a 4096-byte block: (4096 - 69) / 24 = **167 entries**.

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

4. **Sector map caching**: During writes, `sector_map_entry` records are accumulated in an in-memory `sector_map_cache` and flushed to disk in batch on file close, reducing I/O overhead.

5. **Inode caching**: Per-file-handle inode caching avoids repeated B+Tree lookups during writes. The cached inode is written back once on file close if modified.

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

Writes: superblock, catalog tree (header + root node with root directory entry), inode tree (header + root inode), overflow tree (header, empty), dedup tree list (header, empty), and allocation bitmap.

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
| `release` | Flush sector map cache and dirty inode, free per-file context |
| `truncate`| Truncate file to new size (free excess blocks) |
| `unlink`  | Remove file: delete catalog entry, free blocks, delete inode |
| `utimens` | Update modification and access timestamps |

**Per-file-handle context** (`fuse_file_ctx`):
- `inode_id` — Cached inode ID
- `sector_size` — Detected sector size (0 for regular files)
- `inode` — Cached inode structure (write-back on release)
- `inode_dirty` — Flag indicating the cached inode needs write-back
- `sme_cache` — In-memory sector map entry cache (flushed on release)

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
- Dedup: `obmafs3_dedup_get_tree`, `obmafs3_dedup_lookup`, `obmafs3_write_media_image_data`, `obmafs3_read_media_image_data`
- Sector map cache: `obmafs3_flush_sector_map_cache`, `obmafs3_free_sector_map_cache`
- Checksum: `obmafs3_checksum_xxh64`, `obmafs3_checksum_block`
- Compression: `obmafs3_compress`, `obmafs3_decompress`
- Filesystem creation: `obmafs3_create`

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
| Dedup tree list + per-sector-size trees | Complete |
| Regular file read/write | Complete |
| Media image write (sector-level dedup) | Complete |
| Media image read (dedup lookup) | Complete |
| Dedup data block ZSTD compression | Complete |
| Dedup data block decompression on read | Complete |
| Regular data block ZSTD compression | Complete |
| Sector map caching (batched writes) | Complete |
| Inode caching (per file handle) | Complete |
| `mkobmafs` (create filesystem) | Complete |
| `mount.obmafs` (FUSE mount) | Complete |
| `obmafsck` (filesystem checker + scrub) | Complete |
| Metadata B+Tree | Not implemented |
| Media Tag B+Tree | Not implemented |
| Symlinks / hard links | Not implemented |
| Multi-level B+Tree (tree height > 1) | Not implemented |
| Filesystem repair in `obmafsck` | Not implemented (check-only) |
