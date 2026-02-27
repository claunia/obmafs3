// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_internal.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Internal declarations shared between dedup_*.c files.
//
// --[ License ] --------------------------------------------------------------
//
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as
//     published by the Free Software Foundation, either version 3 of the
//     License, or (at your option) any later version.
//
//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.
//
//     You should have received a copy of the GNU General Public License
//     along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------
// Copyright © 2015-2026 Natalia Portillo
// ****************************************************************************/

/*
 * NOT a public header.  Only dedup_*.c translation units should include this.
 */
#ifndef OBMAFS3_DEDUP_INTERNAL_H
#define OBMAFS3_DEDUP_INTERNAL_H

#include "debug.h"
#include "obmafs.h"

#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <xxhash.h>
#include <zstd.h>

/* ------------------------------------------------------------------ */
/*  Timing instrumentation                                             */
/* ------------------------------------------------------------------ */

static inline double timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

/* ------------------------------------------------------------------ */
/*  Dedup B+Tree node cache (chained hash + LRU)                       */
/* ------------------------------------------------------------------ */

#define DEDUP_NC_NIL UINT32_MAX

struct dedup_cache_slot
{
    uint64_t lba;
    uint8_t *buf;
    int      dirty;
    /* Hash chain (singly-linked list within a bucket) */
    uint32_t hash_next;
    /* LRU doubly-linked list (intrusive) */
    uint32_t lru_prev;
    uint32_t lru_next;
};

struct dedup_node_cache
{
    struct dedup_cache_slot *slots;       ///< Flat pool of slots, indexed 0..capacity-1
    uint32_t                *buckets;     ///< Bucket heads (indices into slots[])
    uint32_t                 bucket_count;///< Number of buckets (power-of-two)
    uint32_t                 capacity;    ///< Total pool size (= max slot count)
    uint32_t                 count;       ///< Number of occupied slots
    uint32_t                 max_capacity;///< Hard cap = capacity (set once at init)
    size_t                   block_size;
    uint32_t                *dirty_list;
    uint32_t                 dirty_count;
    uint32_t                 dirty_cap;
    uint32_t                 writes_since_flush;
    /* Free-slot singly-linked list (threaded through hash_next) */
    uint32_t                 free_head;
    /* LRU list endpoints */
    uint32_t                 lru_head;    ///< Most recently used
    uint32_t                 lru_tail;    ///< Least recently used (evict from here)
    pthread_mutex_t          lock;
};

#define DEDUP_CACHE_INIT_CAP 2048

/** Default node-cache memory budget: 8 GiB.  Converted to a slot count
 *  at creation time based on the filesystem's block_size. */
#define DEDUP_NC_DEFAULT_BYTES   (8ULL * 1024 * 1024 * 1024)
#define DEDUP_NC_FLUSH_INTERVAL  32
#define DEDUP_NC_DIRTY_THRESHOLD 256
#define DEDUP_NC_IOV_MAX         1024
/** Number of hash buckets = 2× capacity for ~0.5 average chain length. */
#define DEDUP_NC_BUCKET_FACTOR   2

void compute_node_checksum(uint8_t *buf);

struct dedup_node_cache *dedup_cache_create(size_t block_size, uint64_t max_bytes);
struct dedup_cache_slot *cache_find_slot(struct dedup_node_cache *nc, uint64_t lba);
int  dedup_cache_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t bsz);
void dedup_cache_insert(struct dedup_node_cache *nc, uint64_t lba, const void *data);
int  dedup_cache_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, const void *buf, size_t bsz);
int  dedup_cache_flush(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx);
void dedup_cache_free(struct dedup_node_cache *nc);

/* ------------------------------------------------------------------ */
/*  Dedup key set  (chained hash + intrusive LRU, fixed capacity)      */
/* ------------------------------------------------------------------ */

/** Default keyset RAM budget: 4 GiB. */
#define DEDUP_KS_DEFAULT_BYTES (4ULL * 1024 * 1024 * 1024)

/** Sentinel index: "no node". */
#define DEDUP_KS_NIL UINT32_MAX

/** Hash-table bucket count = 2 × slot capacity for ~50 % load. */
#define DEDUP_KS_BUCKET_FACTOR 2

#define KEYSET_EMPTY 0ULL /* still needed for sentinel key value */

/** One slot in the keyset node pool. */
struct ks_slot
{
    uint64_t key;        /**< The hash key stored here (0 = unused). */
    uint32_t chain_next; /**< Next slot in same hash bucket (DEDUP_KS_NIL = end). */
    uint32_t lru_prev;   /**< Previous in LRU list (DEDUP_KS_NIL = head). */
    uint32_t lru_next;   /**< Next in LRU list (DEDUP_KS_NIL = tail). */
};

/** Fixed-capacity hash set with LRU eviction. */
struct dedup_key_set
{
    struct ks_slot *slots;        /**< Pre-allocated node pool [capacity]. */
    uint32_t       *buckets;      /**< Hash-table bucket heads [bucket_count]. */
    uint32_t        capacity;     /**< Total number of slots. */
    uint32_t        bucket_count; /**< Number of hash buckets. */
    uint32_t        count;        /**< Number of occupied slots. */
    uint32_t        free_head;    /**< Head of the free-slot singly-linked list. */
    uint32_t        lru_head;     /**< Most-recently used slot. */
    uint32_t        lru_tail;     /**< Least-recently used slot (eviction candidate). */
};

struct dedup_key_set *keyset_create(uint64_t max_bytes);
void                  keyset_insert(struct dedup_key_set *ks, uint64_t key);
int                   keyset_contains(const struct dedup_key_set *ks, uint64_t key);
void                  keyset_free(struct dedup_key_set *ks);
void                  keyset_ingest_leaf(struct dedup_key_set *ks, const void *buf);
void                  keyset_seed_from_cache(struct dedup_key_set *ks, const struct dedup_node_cache *nc);

/* ------------------------------------------------------------------ */
/*  Pending insert buffer                                              */
/* ------------------------------------------------------------------ */

struct dedup_pending_buf
{
    struct dedup_entry *slots;
    uint32_t            capacity;
    uint32_t            count;
    uint16_t            sector_size;
};

#define PENDING_INIT_CAP 4096

struct dedup_pending_buf *pending_create(void);
struct dedup_pending_buf *pending_create_presized(uint64_t min_entries);
void                      pending_insert(struct dedup_pending_buf *pb, const struct dedup_entry *entry);
const struct dedup_entry *pending_lookup(const struct dedup_pending_buf *pb, uint64_t hash);
void                      pending_free(struct dedup_pending_buf *pb);
int                       pending_entry_cmp(const void *a, const void *b);

/* ------------------------------------------------------------------ */
/*  Global dedup lookup cache (hash → dedup_entry, immutable entries)   */
/* ------------------------------------------------------------------ */

/**
 * Global dedup lookup cache — LRU hash table.
 *
 * Fixed-capacity hash table with separate chaining and a doubly-linked
 * LRU list threaded through the same node pool.  On a cache miss we
 * evict the least-recently-used entry to make room.
 *
 * Memory budget: ~416 MiB for 8M entries (buckets 32 MiB + nodes 384 MiB).
 */

#define DEDUP_LC_CAPACITY 8388608u    ///< 2^23 = 8M entries (~416 MiB)
#define DEDUP_LC_BUCKETS  8388608u    ///< must equal capacity (power of 2)
#define DEDUP_LC_NIL      UINT32_MAX  ///< sentinel for "no node"

/** A single node in the LRU lookup cache. */
struct dedup_lc_node
{
    uint64_t hash;          ///< Hash key
    uint64_t tree_lba;      ///< Distinguishes different dedup trees
    uint64_t block_lba;     ///< Dedup block LBA
    uint64_t block_offset;  ///< Offset within the dedup block
    uint32_t lru_prev;      ///< Previous node in LRU list (DEDUP_LC_NIL = head)
    uint32_t lru_next;      ///< Next node in LRU list (DEDUP_LC_NIL = tail)
    uint32_t chain_next;    ///< Next node in hash bucket chain (DEDUP_LC_NIL = end)
};

/** Global dedup lookup cache — LRU hash→dedup_entry map. */
struct dedup_lookup_cache
{
    struct dedup_lc_node *nodes;      ///< Node pool [0 .. capacity-1]
    uint32_t             *buckets;    ///< Hash bucket heads [0 .. DEDUP_LC_BUCKETS-1]
    uint32_t              capacity;   ///< Total node pool size
    uint32_t              count;      ///< Currently occupied nodes
    uint32_t              lru_head;   ///< Most recently used (DEDUP_LC_NIL if empty)
    uint32_t              lru_tail;   ///< Least recently used (DEDUP_LC_NIL if empty)
    uint32_t              free_head;  ///< Head of free-list (singly-linked via chain_next)
    pthread_mutex_t       lock;
};

struct dedup_lookup_cache *dedup_lc_create(void);
void                       dedup_lc_free(struct dedup_lookup_cache *lc);
int  dedup_lc_get(struct dedup_lookup_cache *lc, uint64_t hash, uint64_t tree_lba, struct dedup_entry *out);
void dedup_lc_put(struct dedup_lookup_cache *lc, uint64_t hash, uint64_t tree_lba, const struct dedup_entry *entry);

/* ------------------------------------------------------------------ */
/*  B+Tree upsert context                                              */
/* ------------------------------------------------------------------ */

#ifndef DEDUP_BTREE_MAX_DEPTH
#define DEDUP_BTREE_MAX_DEPTH 8
#endif

struct dedup_btree_path
{
    uint64_t lba;
    uint16_t slot;
};

struct dedup_upsert_ctx
{
    struct dedup_btree_path  path[DEDUP_BTREE_MAX_DEPTH];
    int                      depth;
    uint64_t                 leaf_lba;
    int                      insert_pos;
    struct btree_node_header leaf_hdr;
};

int dedup_upsert_find(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash,
                      struct dedup_entry *existing, struct dedup_upsert_ctx *uctx, uint8_t *buf,
                      struct dedup_node_cache *nc);
int dedup_upsert_insert(struct obmafs3_ctx *ctx, struct btree_header *hdr, const struct dedup_entry *entry,
                        struct dedup_upsert_ctx *uctx, uint8_t *buf, struct dedup_node_cache *nc);

/* ------------------------------------------------------------------ */
/*  Node-cache I/O wrappers (dedup_tree.c)                             */
/* ------------------------------------------------------------------ */

int nc_block_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, void *buf, size_t bsz);
int nc_block_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx, uint64_t lba, const void *buf, size_t bsz);

/* ------------------------------------------------------------------ */
/*  Tree list management (dedup_tree.c)                                */
/* ------------------------------------------------------------------ */

int dedup_tree_list_read(struct obmafs3_ctx *ctx, struct tree_list_header *hdr, struct tree_list_entry **entries,
                         uint64_t *count);

/* ------------------------------------------------------------------ */
/*  Leaf-scan warmup (dedup_tree.c)                                    */
/* ------------------------------------------------------------------ */

int  collect_all_leaf_lbas(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint8_t *buf,
                           struct dedup_node_cache *nc, uint64_t **out_lbas, uint64_t *out_count);
void keyset_warmup(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint8_t *buf, struct dedup_node_cache *nc);
void dlc_warmup(struct obmafs3_ctx *ctx);

/* ------------------------------------------------------------------ */
/*  Dedup tree lookup / traversal (dedup_tree.c)                       */
/* ------------------------------------------------------------------ */

/** Leaf-level lookup cache (stack-allocated in read paths). */
struct dedup_leaf_cache
{
    uint8_t *leaf_buf;
    uint16_t num_keys;
    uint64_t min_key;
    uint64_t max_key;
};

#define DEDUP_LEAF_CACHE_INIT {NULL, 0, 0, 0}

int  dedup_leaf_cache_search(const struct dedup_leaf_cache *lc, uint64_t hash, struct dedup_entry *out);
void dedup_leaf_cache_populate(struct dedup_leaf_cache *lc, const uint8_t *buf, size_t block_size);
int  dedup_lookup_cached(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t tree_lba, uint64_t hash,
                         struct dedup_entry *entry, struct dedup_leaf_cache *lc);
void dedup_readahead_next(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t tree_lba,
                          uint64_t next_hash, uint64_t current_lba, struct dedup_leaf_cache *lc);
int  lba_cmp(const void *a, const void *b);
int  dedup_batch_find_leaves(struct obmafs3_ctx *ctx, const struct btree_header *hdr, const uint64_t *hashes,
                             uint64_t count, uint64_t *leaf_lbas, struct dedup_node_cache *nc);
int  dedup_find_leaf_lba(struct obmafs3_ctx *ctx, const struct btree_header *hdr, uint64_t hash, uint64_t *out_leaf_lba,
                         uint8_t *buf, struct dedup_node_cache *nc);

int pending_flush(struct dedup_pending_buf *pb, struct obmafs3_ctx *ctx, struct btree_header *dedup_hdr,
                  uint64_t dedup_hdr_lba);

/* ------------------------------------------------------------------ */
/*  Background compression (dedup_compress.c)                          */
/* ------------------------------------------------------------------ */

int dedup_bg_wait(struct obmafs3_ctx *ctx, void **pjob);
int dedup_bg_submit(struct compress_pool *pool, void **pjob, struct obmafs3_ctx *ctx, uint8_t *data, uint64_t block_lba,
                    uint64_t offset, uint64_t capacity);

/* ------------------------------------------------------------------ */
/*  Dedup data block management (dedup_block.c)                        */
/* ------------------------------------------------------------------ */

struct dedup_block_ctx
{
    uint8_t *data;
    uint64_t block_lba;
    uint64_t offset;
    uint64_t capacity;
    uint64_t std_blocks;
    int      dirty;
};

int  dedup_block_init(struct obmafs3_ctx *ctx, const struct btree_header *hdr, struct dedup_block_ctx *db);
int  dedup_block_flush(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db);
int  dedup_block_new(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db);
int  dedup_block_store(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db, const void *sector_data, size_t sector_len,
                       uint64_t *out_lba, uint64_t *out_offset, struct compress_pool *pool, void **pending_job);
void dedup_block_free(struct dedup_block_ctx *db);

/* ------------------------------------------------------------------ */
/*  Sector map writing (dedup_write.c)                                 */
/* ------------------------------------------------------------------ */

int write_sector_map_batch(struct obmafs3_ctx *ctx, struct inode_record *inode, const struct sector_map_entry *entries,
                           uint64_t count);

int sector_map_finalize_checksum(struct obmafs3_ctx *ctx, struct inode_record *inode, uint64_t entry_count,
                                 size_t entry_size);

#endif /* OBMAFS3_DEDUP_INTERNAL_H */
