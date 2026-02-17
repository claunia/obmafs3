/*
 * dedup.c - OBMAFS3 deduplication operations
 *
 * Manages the deduplication tree list, per-sector-size dedup trees,
 * dedup data block accumulation, and the media image write path that
 * splits incoming data into sectors for deduplication.
 */
#include "obmafs.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

/* ------------------------------------------------------------------ */
/*  Background compression context                                     */
/* ------------------------------------------------------------------ */

/**
 * Persistent worker thread that compresses and writes full dedup blocks
 * in the background while the main thread continues accumulating data
 * into a fresh buffer.
 */
struct bg_compress_ctx {
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond_work;     /**< Main -> worker: new job available */
    pthread_cond_t  cond_done;     /**< Worker -> main: job complete */
    int             busy;          /**< Worker is processing a job */
    int             shutdown;      /**< Signal the worker to exit */
    int             result;        /**< Result of the last job */

    /* Job parameters — owned by the worker while busy */
    uint8_t        *data;          /**< Buffer to compress and write */
    uint64_t        block_lba;     /**< Destination LBA */
    uint64_t        offset;        /**< Byte offset (end of payload) */
    uint64_t        capacity;      /**< Full dedup block size */

    /* Read-only references (safe for concurrent access) */
    struct obmafs3_ctx *ctx;
};

/**
 * Compress and write a full dedup data block to disk.
 * Called by the background worker thread (or synchronously as a helper).
 * The caller retains ownership of @data — it is NOT freed here.
 */
static int bg_do_compress_and_write(struct obmafs3_ctx *ctx,
                                    uint8_t *data, uint64_t block_lba,
                                    uint64_t offset, uint64_t capacity)
{
    struct block_header bhdr;
    memset(&bhdr, 0, sizeof(bhdr));
    bhdr.magic         = OBMAFS3_BLOCK_MAGIC;
    bhdr.original_size = offset - sizeof(struct block_header);

    uint8_t *disk_buf = NULL;
    int compressed = 0;

    if (ctx->compression && bhdr.original_size > 0) {
        size_t comp_bound = ZSTD_compressBound((size_t)bhdr.original_size);
        uint8_t *comp_buf = malloc(comp_bound);
        if (comp_buf) {
            size_t comp_size = comp_bound;
            int crc = obmafs3_compress(
                data + sizeof(bhdr),
                (size_t)bhdr.original_size,
                comp_buf, &comp_size, ctx->zstd_level);
            if (crc == OBMAFS3_OK &&
                comp_size < bhdr.original_size) {
                bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                bhdr.compression_type = kCompressionZstd;
                bhdr.compressed_size  = comp_size;
                obmafs3_checksum_block(comp_buf, comp_size,
                                       bhdr.checksum);
                disk_buf = calloc(1, (size_t)capacity);
                if (disk_buf) {
                    memcpy(disk_buf, &bhdr, sizeof(bhdr));
                    memcpy(disk_buf + sizeof(bhdr), comp_buf, comp_size);
                    compressed = 1;
                }
            }
            free(comp_buf);
        }
    }

    if (!compressed) {
        bhdr.flags           = 0;
        bhdr.compressed_size = bhdr.original_size;
        obmafs3_checksum_block(data + sizeof(bhdr),
                               (size_t)bhdr.original_size,
                               bhdr.checksum);
        memcpy(data, &bhdr, sizeof(bhdr));
    }

    const void *write_src = compressed ? disk_buf : data;
    int rc = obmafs3_block_write(ctx, block_lba, write_src,
                                 (size_t)capacity);
    free(disk_buf);
    return rc;
}

/** Background worker thread entry point. */
static void *bg_compress_worker(void *arg)
{
    struct bg_compress_ctx *bc = (struct bg_compress_ctx *)arg;

    pthread_mutex_lock(&bc->mutex);
    while (!bc->shutdown) {
        /* Wait for a job or shutdown signal */
        while (!bc->busy && !bc->shutdown)
            pthread_cond_wait(&bc->cond_work, &bc->mutex);

        if (bc->shutdown)
            break;

        /* Snapshot job parameters under the lock */
        uint8_t  *data      = bc->data;
        uint64_t  block_lba = bc->block_lba;
        uint64_t  offset    = bc->offset;
        uint64_t  capacity  = bc->capacity;
        struct obmafs3_ctx *ctx = bc->ctx;

        /* Release the lock while doing heavy compression + I/O */
        pthread_mutex_unlock(&bc->mutex);

        int result = bg_do_compress_and_write(ctx, data, block_lba,
                                             offset, capacity);
        free(data);   /* we own this buffer */

        pthread_mutex_lock(&bc->mutex);
        bc->result = result;
        bc->data   = NULL;
        bc->busy   = 0;
        pthread_cond_signal(&bc->cond_done);
    }
    pthread_mutex_unlock(&bc->mutex);
    return NULL;
}

/** Wait for any pending background compression to finish. */
static int bg_compress_wait(struct bg_compress_ctx *bc)
{
    if (!bc)
        return OBMAFS3_OK;

    pthread_mutex_lock(&bc->mutex);
    while (bc->busy)
        pthread_cond_wait(&bc->cond_done, &bc->mutex);
    int result = bc->result;
    pthread_mutex_unlock(&bc->mutex);
    return result;
}

/**
 * Submit a buffer for background compression + write.
 * Ownership of @data transfers to the bg worker — the caller must NOT
 * free or reuse the buffer after this call.
 */
static void bg_compress_submit(struct bg_compress_ctx *bc,
                               uint8_t *data, uint64_t block_lba,
                               uint64_t offset, uint64_t capacity)
{
    pthread_mutex_lock(&bc->mutex);
    bc->data      = data;
    bc->block_lba = block_lba;
    bc->offset    = offset;
    bc->capacity  = capacity;
    bc->result    = OBMAFS3_OK;
    bc->busy      = 1;
    pthread_cond_signal(&bc->cond_work);
    pthread_mutex_unlock(&bc->mutex);
}

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

        /* If the block was compressed on a previous flush, decompress
         * it back into raw form so we can continue appending data at
         * db->offset.  The dedup entries already stored reference
         * uncompressed offsets, so this preserves correctness. */
        struct block_header bhdr;
        memcpy(&bhdr, db->data, sizeof(bhdr));
        if ((bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) &&
            bhdr.original_size > 0) {
            uint8_t *temp = malloc((size_t)bhdr.original_size);
            if (!temp) {
                free(db->data);
                db->data = NULL;
                return OBMAFS3_ERR_NOMEM;
            }
            rc = obmafs3_decompress(
                db->data + sizeof(bhdr),
                (size_t)bhdr.compressed_size,
                temp, (size_t)bhdr.original_size);
            if (rc != OBMAFS3_OK) {
                free(temp);
                free(db->data);
                db->data = NULL;
                return rc;
            }
            /* Rebuild uncompressed layout: [header][raw payload] */
            memset(db->data + sizeof(bhdr), 0,
                   (size_t)db->capacity - sizeof(bhdr));
            memcpy(db->data + sizeof(bhdr), temp,
                   (size_t)bhdr.original_size);
            free(temp);
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
    bhdr.magic         = OBMAFS3_BLOCK_MAGIC;
    bhdr.original_size = db->offset - sizeof(struct block_header);

    /* Try ZSTD compression.
     *
     * IMPORTANT: We must NOT overwrite db->data with the compressed
     * payload.  db->data holds the uncompressed accumulator and may
     * still be appended to if this is a partial (not-yet-full) block.
     * Instead, build the on-disk image in a separate buffer.  */
    uint8_t *disk_buf = NULL;
    int compressed = 0;

    if (ctx->compression && bhdr.original_size > 0) {
        size_t comp_bound = ZSTD_compressBound((size_t)bhdr.original_size);
        uint8_t *comp_buf = malloc(comp_bound);
        if (comp_buf) {
            size_t comp_size = comp_bound;
            int crc = obmafs3_compress(
                db->data + sizeof(bhdr),
                (size_t)bhdr.original_size,
                comp_buf, &comp_size, ctx->zstd_level);
            if (crc == OBMAFS3_OK &&
                comp_size < bhdr.original_size) {
                bhdr.flags            = OBMAFS3_BLOCK_FLAG_COMPRESSED;
                bhdr.compression_type = kCompressionZstd;
                bhdr.compressed_size  = comp_size;
                obmafs3_checksum_block(comp_buf, comp_size,
                                       bhdr.checksum);

                disk_buf = calloc(1, (size_t)db->capacity);
                if (disk_buf) {
                    memcpy(disk_buf, &bhdr, sizeof(bhdr));
                    memcpy(disk_buf + sizeof(bhdr), comp_buf, comp_size);
                    compressed = 1;
                }
            }
            free(comp_buf);
        }
    }

    if (!compressed) {
        bhdr.flags           = 0;
        bhdr.compressed_size = bhdr.original_size;
        obmafs3_checksum_block(db->data + sizeof(bhdr),
                               (size_t)bhdr.original_size,
                               bhdr.checksum);
        memcpy(db->data, &bhdr, sizeof(bhdr));
    }

    const void *write_src = compressed ? disk_buf : db->data;
    int rc = obmafs3_block_write(ctx, db->block_lba, write_src,
                                 (size_t)db->capacity);
    free(disk_buf);
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
 *
 * When @bg is non-NULL and the current block is full, the buffer is
 * handed off to the background worker for compression while a fresh
 * buffer is allocated immediately for continued accumulation.
 */
static int dedup_block_store(struct obmafs3_ctx *ctx,
                             struct dedup_block_ctx *db,
                             const void *sector_data, size_t sector_len,
                             uint64_t *out_lba, uint64_t *out_offset,
                             struct bg_compress_ctx *bg)
{
    int rc;

    /* Need a new block? */
    if (db->block_lba == 0) {
        rc = dedup_block_new(ctx, db);
        if (rc != OBMAFS3_OK)
            return rc;
    } else if (db->offset + sector_len > db->capacity) {
        /* Current block is full */
        if (bg) {
            /* Wait for any previous background job */
            rc = bg_compress_wait(bg);
            if (rc != OBMAFS3_OK)
                return rc;
            /* Hand off the current buffer to the background worker.
             * Ownership of db->data transfers to the bg thread. */
            bg_compress_submit(bg, db->data, db->block_lba,
                               db->offset, db->capacity);
            /* Allocate a fresh buffer and new LBA */
            db->data = calloc(1, (size_t)db->capacity);
            if (!db->data)
                return OBMAFS3_ERR_NOMEM;
            db->dirty = 0;
            uint64_t start_lba;
            rc = obmafs3_alloc_blocks(ctx, db->std_blocks, &start_lba);
            if (rc != OBMAFS3_OK)
                return rc;
            db->block_lba = start_lba;
            db->offset    = sizeof(struct block_header);
        } else {
            /* Synchronous path */
            rc = dedup_block_flush(ctx, db);
            if (rc != OBMAFS3_OK)
                return rc;
            rc = dedup_block_new(ctx, db);
            if (rc != OBMAFS3_OK)
                return rc;
        }
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
 * If @db_cache is non-NULL, the dedup data block accumulator is kept
 * alive across calls instead of being re-read from disk each time.
 * The caller must call obmafs3_flush_dedup_block_cache() on close.
 *
 * The last sector of the file may be smaller than sector_size if the
 * file size is not a multiple of sector_size.
 */
int obmafs3_write_media_image_data(struct obmafs3_ctx *ctx,
                                   struct btree_node_inode *inode,
                                   uint64_t offset, const void *buf,
                                   size_t size, uint16_t sector_size,
                                   struct sector_map_cache *cache,
                                   struct dedup_block_cache *db_cache)
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

    /* Initialize the in-memory dedup block — use the persistent cache
     * if the caller provided one, otherwise fall back to a local ctx
     * that is read from disk each call. */
    struct dedup_block_ctx db_local;
    struct dedup_block_ctx *db;
    int db_is_cached = 0;

    if (db_cache) {
        if (!db_cache->initialized) {
            /* First call — bootstrap the cache from the tree header */
            db_local.data       = NULL;
            db_local.block_lba  = 0;
            db_local.offset     = 0;
            db_local.capacity   = 0;
            db_local.std_blocks = 0;
            db_local.dirty      = 0;
            rc = dedup_block_init(ctx, &dedup_hdr, &db_local);
            if (rc != OBMAFS3_OK)
                return rc;
            /* Migrate into the persistent cache struct */
            db_cache->data       = db_local.data;
            db_cache->block_lba  = db_local.block_lba;
            db_cache->offset     = db_local.offset;
            db_cache->capacity   = db_local.capacity;
            db_cache->std_blocks = db_local.std_blocks;
            db_cache->dirty      = db_local.dirty;
            db_cache->initialized = 1;
        }
        /* Wrap the cache fields into a stack-local dedup_block_ctx
         * that points to the same buffer.  We copy back at the end. */
        db_local.data       = db_cache->data;
        db_local.block_lba  = db_cache->block_lba;
        db_local.offset     = db_cache->offset;
        db_local.capacity   = db_cache->capacity;
        db_local.std_blocks = db_cache->std_blocks;
        db_local.dirty      = db_cache->dirty;
        db = &db_local;
        db_is_cached = 1;
    } else {
        rc = dedup_block_init(ctx, &dedup_hdr, &db_local);
        if (rc != OBMAFS3_OK)
            return rc;
        db = &db_local;
    }

    /*
     * Pre-allocate a buffer for sector_map_entries.
     * Maximum number of sectors in this write = size / sector_size + 1.
     */
    uint64_t max_sectors = size / sector_size + 1;
    struct sector_map_entry *sme_buf =
        malloc((size_t)(max_sectors * sizeof(struct sector_map_entry)));
    if (!sme_buf) {
        if (!db_is_cached)
            dedup_block_free(db);
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
            struct bg_compress_ctx *bg = db_cache
                ? (struct bg_compress_ctx *)db_cache->bg_compress
                : NULL;
            rc = dedup_block_store(ctx, db, sector_data, sector_data_len,
                                   &stored_lba, &stored_offset, bg);
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

    /* Wait for any pending background compression before syncing */
    if (db_cache && db_cache->bg_compress) {
        int bg_rc = bg_compress_wait(
            (struct bg_compress_ctx *)db_cache->bg_compress);
        if (rc == OBMAFS3_OK)
            rc = bg_rc;
    }

    /* Flush any remaining dedup block data to disk (synchronous) */
    if (db->dirty) {
        int flush_rc = dedup_block_flush(ctx, db);
        if (rc == OBMAFS3_OK)
            rc = flush_rc;
    }

    /* Update the tree header with the current partial block state */
    dedup_hdr.last_block_lba    = db->block_lba;
    dedup_hdr.last_block_offset = db->offset;
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
    /* Copy updated state back to the persistent cache if used */
    if (db_is_cached) {
        db_cache->data       = db->data;
        db_cache->block_lba  = db->block_lba;
        db_cache->offset     = db->offset;
        db_cache->capacity   = db->capacity;
        db_cache->std_blocks = db->std_blocks;
        db_cache->dirty      = db->dirty;
    } else {
        dedup_block_free(db);
    }
    return rc;

out:
    /* Error path — wait for bg, then flush dedup state */
    if (db_cache && db_cache->bg_compress)
        bg_compress_wait((struct bg_compress_ctx *)db_cache->bg_compress);
    if (db->dirty)
        dedup_block_flush(ctx, db);

    dedup_hdr.last_block_lba    = db->block_lba;
    dedup_hdr.last_block_offset = db->offset;
    obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);

    free(sme_buf);
    if (db_is_cached) {
        db_cache->data       = db->data;
        db_cache->block_lba  = db->block_lba;
        db_cache->offset     = db->offset;
        db_cache->capacity   = db->capacity;
        db_cache->std_blocks = db->std_blocks;
        db_cache->dirty      = db->dirty;
    } else {
        dedup_block_free(db);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Dedup block cache flush / free                                     */
/* ------------------------------------------------------------------ */

int obmafs3_flush_dedup_block_cache(struct obmafs3_ctx *ctx,
                                    uint16_t sector_size,
                                    struct dedup_block_cache *db_cache)
{
    if (!db_cache || !db_cache->initialized)
        return OBMAFS3_OK;

    /* Build a temporary dedup_block_ctx from the cache */
    struct dedup_block_ctx db;
    db.data       = db_cache->data;
    db.block_lba  = db_cache->block_lba;
    db.offset     = db_cache->offset;
    db.capacity   = db_cache->capacity;
    db.std_blocks = db_cache->std_blocks;
    db.dirty      = db_cache->dirty;

    int rc = OBMAFS3_OK;

    /* Wait for any pending background compression first */
    if (db_cache->bg_compress) {
        int bg_rc = bg_compress_wait(
            (struct bg_compress_ctx *)db_cache->bg_compress);
        if (bg_rc != OBMAFS3_OK)
            rc = bg_rc;
    }

    if (db.dirty) {
        rc = dedup_block_flush(ctx, &db);
        if (rc != OBMAFS3_OK) {
            db_cache->dirty = db.dirty;
            return rc;
        }
    }

    /* Update the tree header with the current partial block state */
    struct btree_header dedup_hdr;
    uint64_t dedup_hdr_lba;
    int hrc = obmafs3_dedup_get_tree(ctx, sector_size, &dedup_hdr,
                                     &dedup_hdr_lba);
    if (hrc == OBMAFS3_OK) {
        dedup_hdr.last_block_lba    = db.block_lba;
        dedup_hdr.last_block_offset = db.offset;
        obmafs3_btree_header_write(ctx, dedup_hdr_lba, &dedup_hdr);
    }

    /* Copy state back */
    db_cache->block_lba  = db.block_lba;
    db_cache->offset     = db.offset;
    db_cache->dirty      = db.dirty;

    return rc;
}

void obmafs3_free_dedup_block_cache(struct dedup_block_cache *db_cache)
{
    if (!db_cache)
        return;
    /* Stop the background compression worker if running */
    obmafs3_bg_compress_stop(db_cache);
    free(db_cache->data);
    db_cache->data = NULL;
    db_cache->initialized = 0;
}

/* ------------------------------------------------------------------ */
/*  Background compression start / stop                                */
/* ------------------------------------------------------------------ */

int obmafs3_bg_compress_start(struct obmafs3_ctx *ctx,
                              struct dedup_block_cache *db_cache)
{
    if (!db_cache || db_cache->bg_compress)
        return OBMAFS3_OK;   /* already started or nothing to do */

    struct bg_compress_ctx *bc = calloc(1, sizeof(*bc));
    if (!bc)
        return OBMAFS3_ERR_NOMEM;

    bc->ctx = ctx;
    pthread_mutex_init(&bc->mutex, NULL);
    pthread_cond_init(&bc->cond_work, NULL);
    pthread_cond_init(&bc->cond_done, NULL);

    int err = pthread_create(&bc->thread, NULL, bg_compress_worker, bc);
    if (err) {
        pthread_cond_destroy(&bc->cond_done);
        pthread_cond_destroy(&bc->cond_work);
        pthread_mutex_destroy(&bc->mutex);
        free(bc);
        return OBMAFS3_ERR_IO;
    }

    db_cache->bg_compress = bc;
    return OBMAFS3_OK;
}

void obmafs3_bg_compress_stop(struct dedup_block_cache *db_cache)
{
    if (!db_cache || !db_cache->bg_compress)
        return;

    struct bg_compress_ctx *bc =
        (struct bg_compress_ctx *)db_cache->bg_compress;

    /* Signal shutdown and wait for the worker to exit */
    pthread_mutex_lock(&bc->mutex);
    bc->shutdown = 1;
    pthread_cond_signal(&bc->cond_work);
    pthread_mutex_unlock(&bc->mutex);

    pthread_join(bc->thread, NULL);

    pthread_cond_destroy(&bc->cond_done);
    pthread_cond_destroy(&bc->cond_work);
    pthread_mutex_destroy(&bc->mutex);
    free(bc);
    db_cache->bg_compress = NULL;
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

    /* Decompressed payload buffer (allocated on first compressed block) */
    uint8_t *decomp_buf = NULL;

    /* Cache the last read dedup block LBA to avoid re-reading */
    uint64_t cached_dedup_lba = 0;
    int      cached_compressed = 0;

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
            free(decomp_buf);
            free(dedup_buf);
            return rc;
        }

        /* Look up the hash in the dedup tree */
        struct dedup_entry de;
        rc = obmafs3_dedup_lookup(ctx, &dedup_hdr, sme.hash, &de);
        if (rc != OBMAFS3_OK) {
            free(decomp_buf);
            free(dedup_buf);
            return rc;
        }

        /* Read the dedup data block if not already cached */
        if (de.block_lba != cached_dedup_lba) {
            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf,
                                    (size_t)ctx->sb.dedup_block_size);
            if (rc != OBMAFS3_OK) {
                free(decomp_buf);
                free(dedup_buf);
                return rc;
            }
            cached_dedup_lba = de.block_lba;

            /* Check if the block is compressed */
            struct block_header bhdr;
            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

            if (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) {
                if (!decomp_buf) {
                    decomp_buf = malloc((size_t)ctx->sb.dedup_block_size);
                    if (!decomp_buf) {
                        free(dedup_buf);
                        return OBMAFS3_ERR_NOMEM;
                    }
                }
                rc = obmafs3_decompress(
                    dedup_buf + sizeof(bhdr),
                    (size_t)bhdr.compressed_size,
                    decomp_buf, (size_t)bhdr.original_size);
                if (rc != OBMAFS3_OK) {
                    free(decomp_buf);
                    free(dedup_buf);
                    return rc;
                }
                cached_compressed = 1;
            } else {
                cached_compressed = 0;
            }
        }

        /* Copy sector data from the dedup block at the stored offset.
         * block_offset includes the header prefix; for compressed blocks
         * decomp_buf holds only the payload so subtract the header. */
        if (cached_compressed) {
            size_t decomp_off = de.block_offset
                                - sizeof(struct block_header);
            memcpy(out + bytes_read,
                   decomp_buf + decomp_off + offset_in_sector,
                   chunk);
        } else {
            memcpy(out + bytes_read,
                   dedup_buf + de.block_offset + offset_in_sector,
                   chunk);
        }
        bytes_read += chunk;
    }

    free(decomp_buf);
    free(dedup_buf);
    return OBMAFS3_OK;
}
