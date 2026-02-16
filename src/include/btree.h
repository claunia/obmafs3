struct btree_header {
    uint64_t magic; //< "BTREEHDR"
    uint32_t data_type; //< Type of data stored in the btree (string, integer, etc.)
    uint64_t root_node_lba; //< Logical block address of the root node of the btree
    uint64_t free_node_lba; //< Logical block address of the first free node in the btree
    uint16_t node_size; //< Size of each node in bytes
    uint32_t total_nodes; //< Total number of nodes in the btree
    uint32_t free_nodes; //< Number of free nodes in the btree
    uint32_t tree_type; //< Type of btree (catalog, deduplication, metadata, media tag)
    uint8_t checksum[32]; //< Checksum of the btree header block for integrity verification
};

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

struct btree_node_filename{
    btree_node_header header;
    uint64_t inode_id;
    uint64_t parent_id;
    uint8_t directory_flag; //< 1 if this entry is a directory, 0 if it's a file
    char name[256]; //< Name of the file or directory
};

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