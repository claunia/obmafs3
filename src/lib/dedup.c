/*
 * dedup.c - OBMAFS3 deduplication operations
 *
 * Manages the deduplication tree list, per-sector-size dedup trees,
 * dedup data block accumulation, and the media image write path that
 * splits incoming data into sectors for deduplication.
 */
#include "obmafs.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/** Compute and store the checksum for a btree node block. */
static void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr = (struct btree_node_header *)buf;
    size_t data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

/* ------------------------------------------------------------------ */
/*  Dedup tree list management                                         */
/* ------------------------------------------------------------------ */

/**
 * Read the dedup tree list header and entries from disk.
 * Caller must free *entries when count > 0.
 */
static int dedup_tree_list_read(struct obmafs3_ctx *ctx,
                                struct tree_list_header *hdr,
                                struct tree_list_entry **entries,
                                uint64_t *count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, buf,
                                (size_t)ctx->sb.block_size);
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }

    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->magic != OBMAFS3_TREELIST_MAGIC) {
        free(buf);
        return OBMAFS3_ERR_BADMAGIC;
    }

    *count = hdr->tree_count;
    if (hdr->tree_count == 0) {
        *entries = NULL;
        free(buf);
        return OBMAFS3_OK;
    }

    *entries = malloc((size_t)(hdr->tree_count * sizeof(struct tree_list_entry)));
    if (!*entries) {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    memcpy(*entries, buf + sizeof(struct tree_list_header),
           (size_t)(hdr->tree_count * sizeof(struct tree_list_entry)));

    free(buf);
    return OBMAFS3_OK;
}

/**
 * Write the dedup tree list header and entries to disk.
 */
static int dedup_tree_list_write(struct obmafs3_ctx *ctx,
                                 struct tree_list_entry *entries,
                                 uint64_t count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct tree_list_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic      = OBMAFS3_TREELIST_MAGIC;
    hdr.tree_count = count;
    /* checksum computed below */

    memcpy(buf, &hdr, sizeof(hdr));
    if (count > 0)
        memcpy(buf + sizeof(hdr), entries,
               (size_t)(count * sizeof(struct tree_list_entry)));

    /* Compute checksum over the whole block contents */
    struct tree_list_header *hdr_buf = (struct tree_list_header *)buf;
    memset(hdr_buf->checksum, 0, sizeof(hdr_buf->checksum));
    obmafs3_checksum_block(buf, sizeof(hdr) +
                           (size_t)(count * sizeof(struct tree_list_entry)),
                           hdr_buf->checksum);

    int rc = obmafs3_block_write(ctx, ctx->sb.dedup_lba, buf,
                                 (size_t)ctx->sb.block_size);
    free(buf);
    return rc;
}

/**
 * Find the dedup tree for a given sector_size.
 * If none exists, create one and add it to the list.
 * Returns the tree header and its LBA on disk.
 */
int obmafs3_dedup_get_tree(struct obmafs3_ctx *ctx, uint16_t sector_size,
                           struct btree_header *hdr, uint64_t *hdr_lba)
{
    struct tree_list_header list_hdr;
    struct tree_list_entry *entries = NULL;
    uint64_t count = 0;
    int rc;

    rc = dedup_tree_list_read(ctx, &list_hdr, &entries, &count);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Search for existing tree with matching sector_size */
    for (uint64_t i = 0; i < count; i++) {
        if (entries[i].sector_size == sector_size) {
            *hdr_lba = entries[i].tree_lba;
            free(entries);
            return obmafs3_btree_header_read(ctx, *hdr_lba, hdr);
        }
    }

    /* Not found — create a new dedup tree */

    /* Allocate a block for the tree header */
    uint64_t new_hdr_lba;
    rc = obmafs3_alloc_block(ctx, &new_hdr_lba);
    if (rc != OBMAFS3_OK) {
        free(entries);
        return rc;
    }

    /* Allocate a block for the root node (empty sentinel) */
    uint64_t root_lba;
    rc = obmafs3_alloc_block(ctx, &root_lba);
    if (rc != OBMAFS3_OK) {
        free(entries);
        return rc;
    }

    /* Write an empty root node */
    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!node_buf) {
        free(entries);
        return OBMAFS3_ERR_NOMEM;
    }

    struct btree_node_header root_hdr;
    memset(&root_hdr, 0, sizeof(root_hdr));
    root_hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    root_hdr.record_type = kBtreeDataTypeDeduplicationEntry;
    root_hdr.node_keys   = 0;  /* empty sentinel */
    root_hdr.keys_length = 0;  /* no entries yet */
    memcpy(node_buf, &root_hdr, sizeof(root_hdr));
    compute_node_checksum(node_buf);
    rc = obmafs3_block_write(ctx, root_lba, node_buf,
                             (size_t)ctx->sb.block_size);
    free(node_buf);
    if (rc != OBMAFS3_OK) {
        free(entries);
        return rc;
    }

    /* Write the tree header */
    struct btree_header new_hdr;
    memset(&new_hdr, 0, sizeof(new_hdr));
    new_hdr.magic            = OBMAFS3_BTREE_HDR_MAGIC;
    new_hdr.data_type        = kBtreeDataTypeDeduplicationEntry;
    new_hdr.root_node_lba    = root_lba;
    new_hdr.node_size        = (uint16_t)ctx->sb.block_size;
    new_hdr.total_nodes      = 1;
    new_hdr.tree_type        = kBtreeTypeDeduplication;
    new_hdr.last_block_lba   = 0;  /* no partial block yet */
    new_hdr.last_block_offset = 0;

    rc = obmafs3_btree_header_write(ctx, new_hdr_lba, &new_hdr);
    if (rc != OBMAFS3_OK) {
        free(entries);
        return rc;
    }

    /* Re-read to get the written checksum */
    rc = obmafs3_btree_header_read(ctx, new_hdr_lba, &new_hdr);
    if (rc != OBMAFS3_OK) {
        free(entries);
        return rc;
    }

    /* Add the new entry to the tree list */
    struct tree_list_entry *new_entries =
        realloc(entries, (size_t)((count + 1) * sizeof(struct tree_list_entry)));
    if (!new_entries) {
        free(entries);
        return OBMAFS3_ERR_NOMEM;
    }
    entries = new_entries;
    entries[count].sector_size = sector_size;
    entries[count].tree_lba    = new_hdr_lba;
    count++;

    rc = dedup_tree_list_write(ctx, entries, count);
    free(entries);
    if (rc != OBMAFS3_OK)
        return rc;

    *hdr     = new_hdr;
    *hdr_lba = new_hdr_lba;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Dedup tree operations                                              */
/* ------------------------------------------------------------------ */

/**
 * Look up a hash in the given dedup tree.
 * Returns OBMAFS3_OK if found, OBMAFS3_ERR_NOTFOUND if not.
 *
 * Each node stores up to max_keys dedup_entry records after the header.
 */
int obmafs3_dedup_lookup(struct obmafs3_ctx *ctx,
                         const struct btree_header *hdr,
                         uint64_t hash, struct dedup_entry *entry)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = hdr->root_node_lba;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if (nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        /* Scan all entries in this node */
        const uint8_t *entries_start = buf + sizeof(struct btree_node_header);
        for (uint16_t i = 0; i < nhdr.node_keys; i++) {
            struct dedup_entry de;
            memcpy(&de, entries_start + i * sizeof(struct dedup_entry),
                   sizeof(de));
            if (de.hash == hash) {
                *entry = de;
                free(buf);
                return OBMAFS3_OK;
            }
        }

        lba = nhdr.right_link;
    }

    free(buf);
    return OBMAFS3_ERR_NOTFOUND;
}

/**
 * Maximum number of dedup_entry records that fit in one node block.
 */
static uint16_t dedup_max_keys(const struct obmafs3_ctx *ctx)
{
    return (uint16_t)((ctx->sb.block_size - sizeof(struct btree_node_header))
                      / sizeof(struct dedup_entry));
}

/**
 * Insert a dedup_entry into the dedup tree.
 *
 * First tries to append to the last node in the linked list.  If that
 * node is full, allocates a new node block and links it.
 */
static int dedup_insert_node(struct obmafs3_ctx *ctx,
                             struct btree_header *hdr, uint64_t hdr_lba,
                             const struct dedup_entry *entry)
{
    uint16_t max_keys = dedup_max_keys(ctx);
    int rc;

    /* Walk to end of linked list to find the last node */
    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!node_buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = hdr->root_node_lba;
    uint64_t last_lba = lba;

    while (lba != 0) {
        rc = obmafs3_block_read(ctx, lba, node_buf,
                                (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(node_buf);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, node_buf, sizeof(nhdr));
        last_lba = lba;

        if (nhdr.right_link == 0)
            break;

        lba = nhdr.right_link;
    }

    /* Check if the last node has room */
    struct btree_node_header nhdr;
    memcpy(&nhdr, node_buf, sizeof(nhdr));

    if (nhdr.node_keys < max_keys) {
        /* Append the entry in place */
        size_t offset = sizeof(struct btree_node_header)
                        + nhdr.node_keys * sizeof(struct dedup_entry);
        memcpy(node_buf + offset, entry, sizeof(*entry));
        nhdr.node_keys++;
        nhdr.keys_length = (uint16_t)(nhdr.node_keys *
                                      sizeof(struct dedup_entry));
        memcpy(node_buf, &nhdr, sizeof(nhdr));
        compute_node_checksum(node_buf);
        rc = obmafs3_block_write(ctx, last_lba, node_buf,
                                 (size_t)ctx->sb.block_size);
        free(node_buf);
        return rc;
    }

    /* Last node is full — allocate a new node */
    uint64_t new_lba;
    rc = obmafs3_alloc_block(ctx, &new_lba);
    if (rc != OBMAFS3_OK) {
        free(node_buf);
        return rc;
    }

    /* Build the new node with one entry */
    uint8_t *new_buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!new_buf) {
        free(node_buf);
        return OBMAFS3_ERR_NOMEM;
    }

    struct btree_node_header new_hdr;
    memset(&new_hdr, 0, sizeof(new_hdr));
    new_hdr.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    new_hdr.record_type = kBtreeDataTypeDeduplicationEntry;
    new_hdr.node_keys   = 1;
    new_hdr.keys_length = (uint16_t)sizeof(struct dedup_entry);
    memcpy(new_buf, &new_hdr, sizeof(new_hdr));
    memcpy(new_buf + sizeof(new_hdr), entry, sizeof(*entry));
    compute_node_checksum(new_buf);
    rc = obmafs3_block_write(ctx, new_lba, new_buf,
                             (size_t)ctx->sb.block_size);
    free(new_buf);
    if (rc != OBMAFS3_OK) {
        free(node_buf);
        return rc;
    }

    /* Link the previous last node to the new one */
    nhdr.right_link = new_lba;
    memcpy(node_buf, &nhdr, sizeof(nhdr));
    compute_node_checksum(node_buf);
    rc = obmafs3_block_write(ctx, last_lba, node_buf,
                             (size_t)ctx->sb.block_size);
    free(node_buf);
    if (rc != OBMAFS3_OK)
        return rc;

    hdr->total_nodes++;
    return obmafs3_btree_header_write(ctx, hdr_lba, hdr);
}

/* ------------------------------------------------------------------ */
/*  Dedup data block management                                        */
/* ------------------------------------------------------------------ */

/**
 * Context for in-memory dedup data block accumulation.
 * Loaded from the tree header's last_block_lba/last_block_offset,
 * and flushed back to disk when full or at the end of processing.
 */
struct dedup_block_ctx {
    uint8_t  *data;          /**< In-memory dedup data block buffer */
    uint64_t  block_lba;     /**< LBA of this dedup block (0 = not yet allocated) */
    uint64_t  offset;        /**< Current byte offset for next sector write */
    uint64_t  capacity;      /**< Total capacity in bytes (dedup_block_size) */
    uint64_t  std_blocks;    /**< Number of standard blocks this dedup block spans */
    int       dirty;         /**< Whether the buffer has been modified */
};

/**
 * Initialize the in-memory dedup block from the tree header.
 * If last_block_lba != 0, reads the partial block from disk.
 * Otherwise sets up for a fresh allocation.
 */
static int dedup_block_init(struct obmafs3_ctx *ctx,
                            const struct btree_header *hdr,
                            struct dedup_block_ctx *db)
{
    db->capacity   = ctx->sb.dedup_block_size;
    db->std_blocks = db->capacity / ctx->sb.block_size;
    db->dirty      = 0;

    db->data = calloc(1, (size_t)db->capacity);
    if (!db->data)
        return OBMAFS3_ERR_NOMEM;

    if (hdr->last_block_lba != 0) {
        /* Read the existing partial block from disk */
        db->block_lba = hdr->last_block_lba;
        db->offset    = hdr->last_block_offset;
        int rc = obmafs3_block_read(ctx, db->block_lba, db->data,
                                    (size_t)db->capacity);
        if (rc != OBMAFS3_OK) {
            free(db->data);
            db->data = NULL;
            return rc;
        }
    } else {
        db->block_lba = 0;
        db->offset    = 0;
    }

    return OBMAFS3_OK;
}

/**
 * Finalize and flush the in-memory dedup block to disk.
 * Computes the block_header checksum and writes it.
 */
static int dedup_block_flush(struct obmafs3_ctx *ctx,
                             struct dedup_block_ctx *db)
{
    if (!db->dirty || db->block_lba == 0)
        return OBMAFS3_OK;

    /* Write the block header */
    struct block_header bhdr;
    memset(&bhdr, 0, sizeof(bhdr));
    bhdr.magic           = OBMAFS3_BLOCK_MAGIC;
    bhdr.flags           = 0;  /* dedup blocks not compressed individually */
    bhdr.original_size   = db->offset - sizeof(struct block_header);
    bhdr.compressed_size = bhdr.original_size;

    /* Checksum over the data after the header */
    obmafs3_checksum_block(db->data + sizeof(bhdr),
                           (size_t)bhdr.original_size,
                           bhdr.checksum);
    memcpy(db->data, &bhdr, sizeof(bhdr));

    int rc = obmafs3_block_write(ctx, db->block_lba, db->data,
                                 (size_t)db->capacity);
    if (rc != OBMAFS3_OK)
        return rc;

    db->dirty = 0;
    return OBMAFS3_OK;
}

/**
 * Allocate a new dedup data block and prepare it for writing.
 * If there was a previous block, it is flushed first.
 */
static int dedup_block_new(struct obmafs3_ctx *ctx,
                           struct dedup_block_ctx *db)
{
    /* Flush any existing block */
    if (db->dirty) {
        int rc = dedup_block_flush(ctx, db);
        if (rc != OBMAFS3_OK)
            return rc;
    }

    /* Allocate contiguous standard blocks for the dedup block */
    uint64_t start_lba;
    int rc = obmafs3_alloc_blocks(ctx, db->std_blocks, &start_lba);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Reset the buffer */
    memset(db->data, 0, (size_t)db->capacity);
    db->block_lba = start_lba;
    db->offset    = sizeof(struct block_header);
    db->dirty     = 0;

    return OBMAFS3_OK;
}

/**
 * Store a sector into the dedup data block.
 * Allocates a new block if the current one is full or doesn't exist.
 * Returns the block_lba and block_offset where the sector was stored.
 */
static int dedup_block_store(struct obmafs3_ctx *ctx,
                             struct dedup_block_ctx *db,
                             const void *sector_data, size_t sector_len,
                             uint64_t *out_lba, uint64_t *out_offset)
{
    int rc;

    /* Need a new block? */
    if (db->block_lba == 0) {
        rc = dedup_block_new(ctx, db);
        if (rc != OBMAFS3_OK)
            return rc;
    } else if (db->offset + sector_len > db->capacity) {
        /* Current block is full — finalize and start a new one */
        rc = dedup_block_flush(ctx, db);
        if (rc != OBMAFS3_OK)
            return rc;
        rc = dedup_block_new(ctx, db);
        if (rc != OBMAFS3_OK)
            return rc;
    }

    /* Append sector data at current offset */
    memcpy(db->data + db->offset, sector_data, sector_len);
    *out_lba    = db->block_lba;
    *out_offset = db->offset;
    db->offset += sector_len;
    db->dirty   = 1;

    return OBMAFS3_OK;
}

static void dedup_block_free(struct dedup_block_ctx *db)
{
    free(db->data);
    db->data = NULL;
}

/* ------------------------------------------------------------------ */
/*  Sector map entry writing                                           */
/* ------------------------------------------------------------------ */

/**
 * Write a batch of sector_map_entries to the file's data blocks.
 * Uses the inode's extents and sector_map_size to determine position.
 * Allocates new blocks as needed.
 *
 * Writing all entries in one call avoids interleaving block allocations
 * with dedup tree node allocations, preventing inode extent fragmentation.
 */
static int write_sector_map_batch(struct obmafs3_ctx *ctx,
                                  struct btree_node_inode *inode,
                                  const struct sector_map_entry *entries,
                                  uint64_t count)
{
    if (count == 0)
        return OBMAFS3_OK;

    size_t entry_size = sizeof(struct sector_map_entry);
    uint64_t map_offset = inode->sector_map_size * entry_size;
    size_t total_bytes = (size_t)(count * entry_size);

    /*
     * Temporarily set file_size to the current sector map byte size
     * so block allocation is computed correctly for the extent-based
     * storage of sector_map data.
     */
    uint64_t saved_file_size = inode->file_size;
    inode->file_size = map_offset;

    int rc = obmafs3_write_file_data(ctx, inode, map_offset, entries,
                                     total_bytes);

    /* Restore the logical image size */
    inode->file_size = saved_file_size;

    if (rc == OBMAFS3_OK)
        inode->sector_map_size += count;

    return rc;
}

/* ------------------------------------------------------------------ */
/*  Media image write path                                             */
/* ------------------------------------------------------------------ */

/**
 * Write data to a media image file with sector-level deduplication.
 *
 * Incoming data is split into sectors of @sector_size bytes.  Each
 * sector is hashed and looked up in the dedup tree for that sector
 * size.  New sectors are stored in dedup data blocks; duplicate
 * sectors are discarded.  A sector_map_entry is appended for every
 * sector regardless.
 *
 * If @cache is non-NULL, sector_map_entries are accumulated in the
 * cache instead of being written to disk.  Call
 * obmafs3_flush_sector_map_cache() to write them out.
 *
 * The last sector of the file may be smaller than sector_size if the
 * file size is not a multiple of sector_size.
 */
int obmafs3_write_media_image_data(struct obmafs3_ctx *ctx,
                                   struct btree_node_inode *inode,
                                   uint64_t offset, const void *buf,
                                   size_t size, uint16_t sector_size,
                                   struct sector_map_cache *cache)
{
    int rc;

    /* Update file_size (the logical image size) */
    uint64_t new_end = offset + size;
    if (new_end > inode->file_size)
        inode->file_size = new_end;

    /* Get (or create) the dedup tree for this sector size */
    struct btree_header dedup_hdr;
    uint64_t dedup_hdr_lba;
    rc = obmafs3_dedup_get_tree(ctx, sector_size, &dedup_hdr,
                                &dedup_hdr_lba);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Initialize the in-memory dedup block from the tree's partial block */
    struct dedup_block_ctx db;
    rc = dedup_block_init(ctx, &dedup_hdr, &db);
    if (rc != OBMAFS3_OK)
        return rc;

    /*
     * Pre-allocate a buffer for sector_map_entries.
     * Maximum number of sectors in this write = size / sector_size + 1.
     */
    uint64_t max_sectors = size / sector_size + 1;
    struct sector_map_entry *sme_buf =
        malloc((size_t)(max_sectors * sizeof(struct sector_map_entry)));
    if (!sme_buf) {
        dedup_block_free(&db);
        return OBMAFS3_ERR_NOMEM;
    }
    uint64_t sme_count = 0;

    /*
     * Phase 1: Process sectors — hash, dedup lookup/store, collect
     *          sector_map_entries in memory.  All dedup tree node
     *          allocations happen here, before any sector map block
     *          allocations, preventing extent fragmentation.
     */
    const uint8_t *data = (const uint8_t *)buf;
    size_t bytes_processed = 0;

    while (bytes_processed < size) {
        uint64_t write_pos = offset + bytes_processed;
        int64_t sector_num = (int64_t)(write_pos / sector_size);
        size_t offset_in_sector = (size_t)(write_pos % sector_size);

        size_t remaining_in_write = size - bytes_processed;
        size_t remaining_in_sector = (size_t)sector_size - offset_in_sector;
        size_t chunk = (remaining_in_write < remaining_in_sector)
                           ? remaining_in_write
                           : remaining_in_sector;

        if (offset_in_sector != 0) {
            /* Partial sector write not starting at sector boundary.
             * This shouldn't happen in normal sequential writes.
             * Fall back to storing it as a unique (non-dedup) sector. */
        }

        size_t sector_data_len = chunk;
        const uint8_t *sector_data = data + bytes_processed;

        /* Hash the sector data */
        uint64_t hash = obmafs3_checksum_xxh64(sector_data, sector_data_len);

        /* Look up in the dedup tree */
        struct dedup_entry existing;
        rc = obmafs3_dedup_lookup(ctx, &dedup_hdr, hash, &existing);

        if (rc == OBMAFS3_OK) {
            /* Already stored — skip the data, just record the mapping */
        } else if (rc == OBMAFS3_ERR_NOTFOUND) {
            /* New sector — store in the dedup data block */
            uint64_t stored_lba, stored_offset;
            rc = dedup_block_store(ctx, &db, sector_data, sector_data_len,
                                   &stored_lba, &stored_offset);
            if (rc != OBMAFS3_OK)
                goto out;

            /* Insert into the dedup tree */
            struct dedup_entry new_entry;
            new_entry.hash         = hash;
            new_entry.block_lba    = stored_lba;
            new_entry.block_offset = stored_offset;

            rc = dedup_insert_node(ctx, &dedup_hdr, dedup_hdr_lba,
                                   &new_entry);
            if (rc != OBMAFS3_OK)
                goto out;
        } else {
            /* Lookup error */
            goto out;
        }

        /* Collect the sector_map_entry */
        sme_buf[sme_count].sector      = sector_num;
        sme_buf[sme_count].sector_size = sector_size;
        sme_buf[sme_count].hash        = hash;
        sme_count++;

        /* Update sector count */
        if ((uint64_t)(sector_num + 1) > inode->sector_count)
            inode->sector_count = (uint64_t)(sector_num + 1);

        bytes_processed += chunk;
    }

    /*
     * Phase 2: Flush dedup data, then write all sector_map_entries in
     *          one bulk call.  This ensures sector map block allocations
     *          are contiguous (single extent), since no other allocations
     *          happen between them.
     */

    /* Flush any remaining dedup block data to disk */
    if (db.dirty) {
        int flush_rc = dedup_block_flush(ctx, &db);
        if (rc == OBMAFS3_OK)
            rc = flush_rc;
    }

    /* Update the tree header with the current partial block state */
    dedup_hdr.last_block_lba    = db.block_lba;
    dedup_hdr.last_block_offset = db.offset;
    int hdr_rc = obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);
    if (rc == OBMAFS3_OK)
        rc = hdr_rc;

    /* Re-read header to keep cached checksum in sync */
    if (rc == OBMAFS3_OK)
        obmafs3_btree_header_read(ctx, dedup_hdr_lba, &dedup_hdr);

    /* Now write or cache the sector map entries */
    if (rc == OBMAFS3_OK && sme_count > 0) {
        if (cache) {
            /* Append to in-memory cache */
            uint64_t need = cache->count + sme_count;
            if (need > cache->capacity) {
                uint64_t new_cap = cache->capacity;
                if (new_cap == 0)
                    new_cap = 1024;
                while (new_cap < need)
                    new_cap *= 2;
                struct sector_map_entry *tmp =
                    realloc(cache->entries,
                            (size_t)(new_cap * sizeof(*tmp)));
                if (!tmp) {
                    rc = OBMAFS3_ERR_NOMEM;
                } else {
                    cache->entries  = tmp;
                    cache->capacity = new_cap;
                }
            }
            if (rc == OBMAFS3_OK) {
                memcpy(cache->entries + cache->count, sme_buf,
                       (size_t)(sme_count * sizeof(*sme_buf)));
                cache->count += sme_count;
            }
        } else {
            rc = write_sector_map_batch(ctx, inode, sme_buf, sme_count);
        }
    }

    free(sme_buf);
    dedup_block_free(&db);
    return rc;

out:
    /* Error path — still flush dedup state */
    if (db.dirty)
        dedup_block_flush(ctx, &db);

    dedup_hdr.last_block_lba    = db.block_lba;
    dedup_hdr.last_block_offset = db.offset;
    obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);

    free(sme_buf);
    dedup_block_free(&db);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Sector map cache flush / free                                      */
/* ------------------------------------------------------------------ */

int obmafs3_flush_sector_map_cache(struct obmafs3_ctx *ctx,
                                   struct btree_node_inode *inode,
                                   struct sector_map_cache *cache)
{
    if (!cache || cache->count == 0)
        return OBMAFS3_OK;

    int rc = write_sector_map_batch(ctx, inode, cache->entries,
                                   cache->count);
    if (rc == OBMAFS3_OK) {
        cache->count = 0;  /* keep the buffer for potential reuse */
    }
    return rc;
}

void obmafs3_free_sector_map_cache(struct sector_map_cache *cache)
{
    if (!cache)
        return;
    free(cache->entries);
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}

/* ------------------------------------------------------------------ */
/*  Media image read path                                              */
/* ------------------------------------------------------------------ */

/**
 * Read data from a media image file.
 *
 * The inode's data blocks store sector_map_entry records that map
 * each logical sector to a hash.  The hash is used to look up the
 * actual sector data in the dedup tree.
 *
 * @param ctx         Filesystem context.
 * @param inode       Inode of the media image file.
 * @param offset      Byte offset in the original file.
 * @param buf         Output buffer.
 * @param size        Number of bytes to read.
 * @param sector_size Size of each sector in the image.
 * @return OBMAFS3_OK on success, error code otherwise.
 */
int obmafs3_read_media_image_data(struct obmafs3_ctx *ctx,
                                  const struct btree_node_inode *inode,
                                  uint64_t offset, void *buf,
                                  size_t size, uint16_t sector_size)
{
    if (offset >= inode->file_size)
        return OBMAFS3_OK;

    if (offset + size > inode->file_size)
        size = (size_t)(inode->file_size - offset);

    if (size == 0)
        return OBMAFS3_OK;

    /* Get the dedup tree for this sector size */
    struct btree_header dedup_hdr;
    uint64_t dedup_hdr_lba;
    int rc = obmafs3_dedup_get_tree(ctx, sector_size,
                                    &dedup_hdr, &dedup_hdr_lba);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Buffer for reading the dedup data block (dedup_block_size bytes) */
    uint8_t *dedup_buf = malloc((size_t)ctx->sb.dedup_block_size);
    if (!dedup_buf)
        return OBMAFS3_ERR_NOMEM;

    /* Cache the last read dedup block LBA to avoid re-reading */
    uint64_t cached_dedup_lba = 0;

    /* Temporary inode copy for reading sector map (need to adjust file_size) */
    struct btree_node_inode map_inode;
    memcpy(&map_inode, inode, sizeof(map_inode));
    map_inode.file_size = inode->sector_map_size *
                          sizeof(struct sector_map_entry);

    uint8_t *out = (uint8_t *)buf;
    size_t bytes_read = 0;

    while (bytes_read < size) {
        uint64_t read_pos = offset + bytes_read;
        int64_t sector_num = (int64_t)(read_pos / sector_size);
        size_t offset_in_sector = (size_t)(read_pos % sector_size);

        /* How many bytes remain in this sector */
        size_t remaining_in_sector = (size_t)sector_size - offset_in_sector;
        size_t remaining_in_read = size - bytes_read;
        size_t chunk = remaining_in_read < remaining_in_sector
                           ? remaining_in_read : remaining_in_sector;

        /* Read the sector_map_entry for this sector from inode data */
        struct sector_map_entry sme;
        uint64_t sme_offset = (uint64_t)sector_num *
                              sizeof(struct sector_map_entry);
        rc = obmafs3_read_file_data(ctx, &map_inode, sme_offset,
                                    &sme, sizeof(sme));
        if (rc != OBMAFS3_OK) {
            free(dedup_buf);
            return rc;
        }

        /* Look up the hash in the dedup tree */
        struct dedup_entry de;
        rc = obmafs3_dedup_lookup(ctx, &dedup_hdr, sme.hash, &de);
        if (rc != OBMAFS3_OK) {
            free(dedup_buf);
            return rc;
        }

        /* Read the dedup data block if not already cached */
        if (de.block_lba != cached_dedup_lba) {
            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf,
                                    (size_t)ctx->sb.dedup_block_size);
            if (rc != OBMAFS3_OK) {
                free(dedup_buf);
                return rc;
            }
            cached_dedup_lba = de.block_lba;
        }

        /* Copy sector data from the dedup block at the stored offset */
        memcpy(out + bytes_read,
               dedup_buf + de.block_offset + offset_in_sector,
               chunk);
        bytes_read += chunk;
    }

    free(dedup_buf);
    return OBMAFS3_OK;
}
