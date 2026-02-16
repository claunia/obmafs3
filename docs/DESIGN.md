# Design of OBMAFS v3

OBMAFS v3 is a filesystem designed for the deduplicated storage of disk image files. It is designed to deduplicate data at the sector level of images stored inside it, while still allowing for efficient random access to the data, and at the same time being able to store accompanying metadata files. It is also designed to be able to index metadata about the images themselves.

## The Superblock 

All OBMAFS start with the superblock structure:

```c
struct obmafs3_sb {
    uint64_t magic; //< "OBMAFS_3"
    uint8_t guid[16]; //< Unique identifier for the filesystem instance
    uint64_t block_size; //< Size of each block for non-deduplicated data
    uint64_t dedup_block_size; //< Size of each block for deduplicated data
    uint64_t total_bytes; //< Total size of the filesystem in bytes
    uint64_t catalog_lba; //< Logical block address of the catalog (btree) structure
    uint64_t inode_lba; //< Logical block address of the inode (btree) structure
    uint64_t overflow_lba; //< Logical block address of the overflow tree for large files
    uint64_t dedup_lba; //< Logical block address of the deduplication tree list header
    uint64_t metadata_lba; //< Logical block address of the metadata (btree)structure
    uint64_t media_tag_lba; //< Logical block address of the media tag (btree) structure
    uint16_t checksum_type; //< Type of checksum used for the filesystem
    uint64_t creation_time; //< Creation time of the filesystem
    uint8_t volume_label[256]; //< Volume label of the filesystem
};
```

The superblock contains a magic number to identify the filesystem type, a unique identifier (GUID) for the filesystem instance, block sizes for both non-deduplicated and deduplicated data, total size of the filesystem, logical block addresses for various structures (catalog, deduplication tree list header, metadata, media tag), checksum type used for the filesystem, creation time, and a volume label.

The `catalog_lba` point to the logical block address containing the header of the Catalog Tree, which is a B+Tree structure that stores the directory structure and file names of the file system. Each node in the Catalog Tree contains a list of entries, where each entry represents a file or directory. The entries contain the name of the file or directory, its type (file or directory), and a pointer to the corresponding data in the deduplication tree.

The `inode_lba` points to the logical block address containing the header of the Inode Tree, which is a B+Tree structure that stores metadata about the files in the filesystem. Each node in the Inode Tree contains a list of entries, where each entry represents a file. The entries contain metadata such as file size, creation time, modification time, and pointers to the file data.

The `overflow_lba` points to the logical block address containing the header of the Overflow Tree, which is a B+Tree structure that stores extra extents for files that have more data that can be stored in the extents contained in the inodes.

The `dedup_lba` points to the logical block address containing the header of the Deduplication Tree List, which is a simple list of B+Tree structures that store the deduplicated data blocks. Each B+Tree in the list corresponds to a specific disk image sector size (e.g., 512 bytes, 4096 bytes) and contains entries that map the hash of a deduplicated sector to its physical location in the filesystem.

The `metadata_lba` points to the logical block address containing the header of the Metadata Tree, which is a B+Tree structure that stores metadata about the disk images themselves. Each node in the Metadata Tree contains a list of arbitrary key-value pairs, where the keys are strings and the values can be of various types (e.g., string, integer, binary data). This allows for flexible storage of metadata about the disk images, such as their original size, format, creation time, etc.

The `media_tag_lba` points to the logical block address containing the header of the Media Tag Tree, which is a B+Tree structure that stores media tags for the disk images. Each node in the Media Tag Tree contains a list of entries, where each entry represents a media tag. The entries contain the name of the media tag and its value or a pointer to a block in the filesystem storing its value.

The `checksum_type` field in the superblock indicates the type of checksum used for the filesystem, which can be used to verify the integrity of the data stored in the filesystem. The `creation_time` field stores the time when the filesystem was created, and the `volume_label` field allows for a human-readable label to be assigned to the filesystem.

## The B+Tree Structures

All the B+Tree structures in OBMAFS v3 (Catalog Tree, Inode Tree, Overflow Tree, Deduplication Tree, Metadata Tree, Media Tag Tree) share a common header structure:

```c
struct btree_node_header {
    uint64_t magic; //< "BTREENDE"
    uint8_t record_type;
    uint64_t left_link;
    uint64_t right_link;
    uint64_t overflow_link;
    uint16_t node_keys;
    uint16_t keys_length;
    uint8_t checksum[32]; //< Checksum of the btree node block for integrity verification
};
```

The `magic` field is used to identify the block as a B+Tree node. The `record_type` field indicates the type of records stored in the node (e.g., catalog entries, inode entries, deduplication entries, metadata entries, media tag entries). The `left_link` and `right_link` fields are used to link sibling nodes in the B+Tree structure. The `overflow_link` field is used to link to an overflow node if the current node exceeds its capacity for storing records. The `node_keys` field indicates the number of keys currently stored in the node, and the `keys_length` field indicates the total length of the keys stored in the node. The `checksum` field contains a checksum of the entire B+Tree node block for integrity verification (with that field zeroed).

Each B+Tree structure will have its own specific record format for the entries it stores, but they will all follow the same general structure of having a header followed by a list of records. The records will be stored in sorted order based on their keys to allow for efficient searching and retrieval of data.

The Catalog Tree nodes use the following record format for their entries:

```c
struct btree_node_filename{
    btree_node_header header;
    uint64_t inode_id;
    uint64_t parent_id;
    uint8_t directory_flag; //< 1 if this entry is a directory, 0 if it's a file
    char name[256]; //< Name of the file or directory
};
```

Where `inode_id` is a unique identifier for the file or directory, `parent_id` is the identifier of the parent directory (or 2 for the root directory), `directory_flag` indicates whether the entry is a directory or a file, and `name` is the name of the file or directory in UTF-8 NUL-terminated format.

The Inode Tree nodes use the following record format for their entries:

```c
struct btree_node_inode {
    btree_node_header header;
    uint64_t inode_id;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t access_time;
    uint64_t file_size;
    extent_run extents[8]; //< Array of extent runs for the file data
    uint8_t file_type; //< Type of file (regular, media image, etc.)
};
```

Where `inode_id` is a unique identifier for the file, `uid` and `gid` are the user and group IDs of the file owner, `mode` is the file permissions, `creation_time`, `modification_time`, and `access_time` are timestamps for the file, `file_size` is the size of the file in bytes, `extents` is an array of extent runs that point to the data blocks for the file, and `file_type` indicates the type of file (e.g., regular file, media image file, etc.). In the case of media image files, the extents point to a list of `sector_map_entry` structures that map the sectors of the disk image to their corresponding deduplicated data blocks in the filesystem.

```c
struct sector_map_entry {
    int64_t sector;
    uint16_t sector_size; //< Size of the sector in bytes
    uint64_t hash; //< Hash of the data in the sector for deduplication purposes
}
```

Where `sector` is the logical sector number within the disk image, `sector_size` is the size of the sector in bytes (e.g., 512, 4096), and `hash` is a hash value computed from the data in the sector, which is used for deduplication purposes to identify identical sectors across different disk images.

The Deduplication Tree nodes use the following record format for their entries:

```c
struct dedup_entry {
    uint64_t hash; //< Hash of the deduplicated data block
    uint64_t block_lba; //< Logical block address where the deduplicated data block is stored
    uint64_t block_offset; //< Offset within the block where the deduplicated data starts
};
```

Where `hash` is the hash value of the deduplicated data block, `block_lba` is the logical block address where the deduplicated data block is stored in the filesystem, and `block_offset` is the offset within that block where the deduplicated data starts. This allows for efficient retrieval of deduplicated data blocks based on their hash values.

The format of the media tag tree and the format of the metadata tree will be defined in a future update, but they will follow a similar structure to the Catalog Tree and Inode Tree, with their own specific record formats for their entries.

## The Deduplication Tree List

The Deduplication Tree List is a simple list structure that contains headers for multiple B+Tree structures, each corresponding to a specific disk image sector size. The header for the Deduplication Tree List is as follows:

```c
struct tree_list_header {
    uint64_t magic; //< "TREELIST"
    uint64_t tree_count; //< Total number of btrees in the list
    uint8_t checksum[32]; //< Checksum of the tree list header block for integrity verification
    // Followed by an array of tree_list_entry structures, one for each btree in the list
};
```

Where `magic` is used to identify the block as a Deduplication Tree List header, `tree_count` indicates the total number of B+Tree structures in the list, and `checksum` contains a checksum of the entire Deduplication Tree List header block for integrity verification (with that field zeroed).
Each entry in the Deduplication Tree List corresponds to a specific disk image sector size and has the following format:

```c
struct tree_list_entry {
    uint16_t sector_size; //< Size of each sector in bytes
    uint64_t tree_lba; //< Logical block address where the btree is stored
};
```

Where `sector_size` indicates the size of each sector in bytes (e.g., 512, 4096) that the corresponding B+Tree structure is designed to handle, and `tree_lba` is the logical block address where the header of that B+Tree structure is stored in the filesystem. This allows for efficient retrieval of the appropriate B+Tree structure based on the sector size of the disk image being accessed.

## The deduplication process

When a file is copied to the filesystem it is first analyzed to identify if it is a disk image file or not. If it is not a disk image file, it is stored as a regular file with its data blocks stored in the filesystem without deduplication. If it is identified as a disk image file, it is processed to identify the sector size, and then, if the corresponding B+Tree structure for that sector size does not exist in the Deduplication Tree List, a new B+Tree structure is created for that sector size and added to the list.

Then the disk image is processed sector by sector, where for each sector, a hash value is computed from the data in the sector. The deduplication tree corresponding to the sector size is then searched for an entry with a matching hash value. If a matching entry is found, it means that an identical sector has already been stored in the filesystem, and its contents are discarded. If no match is found, the sector data is stored in memory, until a full deduplicated block is formed (e.g., 4KB), at which point the block is written to the filesystem, and an entry is added to the corresponding deduplication tree with the hash value of the block and its location in the filesystem. This process allows for efficient storage of disk image files by eliminating duplicate sectors across different images, while still allowing for efficient random access to the data through the use of the B+Tree structures. In both cases the sector number, size, and hash value are stored in the `sector_map_entry` structures in the inodes for the disk image files, allowing for efficient mapping of the logical sectors of the disk images to their corresponding deduplicated data blocks in the filesystem.

In both types of files (regular and disk image files), the metadata about the file (e.g., size, timestamps, permissions) is stored in the Inode Tree, and the directory structure and file names are stored in the Catalog Tree, allowing for efficient organization and retrieval of files in the filesystem.

## Blocks

All data blocks in the filesystem, be it normal data blocks for regular files, or deduplicated data blocks for disk images, are prepended with a small header that contains information about the block, such as if it compressed, and the block checksum, allowing for block integrity verification and efficient storage of data. The format of the block header is as follows:

```c
struct block_header {
    uint64_t magic; //< "OBMABLCK"
    uint8_t flags; //< Flags indicating if the block is compressed, etc.
    uint8_t compression_type; //< Type of compression used for the block (if applicable)
    uint64_t original_size; //< Original size of the data before compression (if applicable)
    uint64_t compressed_size; //< Size of the data in the block (after compression if applicable)
    uint8_t checksum[32]; //< Checksum of the block data for integrity verification
};
```

Where `magic` is used to identify the block as an OBMAFS block, `flags` contains flags indicating if the block is compressed or has other special properties, `original_size` indicates the original size of the data before compression (if applicable), `compressed_size` indicates the size of the data in the block (after compression if applicable), and `checksum` contains a checksum of the block data for integrity verification (with that field zeroed). This allows for efficient storage of data blocks while also ensuring data integrity through the use of checksums.

## Checksums

Currently the only support checksum is XXHASH64 for all operations.

## Compression

Currently the only supported compression algorithm is ZSTD for all operations.

## Implementation

The filesystem will be implemented in C, with a user-space FUSE driver for mounting and accessing the filesystem on Linux. The implementation will include a tool to create volumes, `mkobmafs`, a tool to check and repair volumes, `obmafsck`, and a user application for mounting the filesystem, `mount.obmafs`. The implementation will also include a library, `libobmafs`, that provides an API for interacting with the filesystem, allowing for integration with other applications and tools. The library will provide functions for creating and managing volumes, reading and writing files, and accessing metadata about the files and disk images stored in the filesystem. The implementation will be designed to be efficient and scalable, allowing for the storage of large disk image files with high deduplication ratios, while still providing fast access to the data and metadata stored in the filesystem.
