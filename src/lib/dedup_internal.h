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

#include "obmafs.h"
#include "debug.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <time.h>
#include <zstd.h>

/* ------------------------------------------------------------------ */
/*  Timing instrumentation                                             */
/* ------------------------------------------------------------------ */

static inline double timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

/* ------------------------------------------------------------------ */
/*  Dedup B+Tree node cache                                            */
/* ------------------------------------------------------------------ */

struct dedup_cache_slot
{
    uint64_t lba;
    uint8_t *buf;
    int      dirty;
};

struct dedup_node_cache
{
    struct dedup_cache_slot *slots;
    uint32_t                 capacity;
    uint32_t                 count;
    size_t                   block_size;
    uint32_t                *dirty_list;
    uint32_t                 dirty_count;
    uint32_t                 dirty_cap;
    uint32_t                 writes_since_flush;
    pthread_mutex_t          lock;
};

#define DEDUP_CACHE_INIT_CAP        2048
#define DEDUP_NC_FLUSH_INTERVAL       32
#define DEDUP_NC_DIRTY_THRESHOLD     256
#define DEDUP_NC_IOV_MAX            1024

void compute_node_checksum(uint8_t *buf);

struct dedup_node_cache *dedup_cache_create(size_t block_size);
struct dedup_cache_slot *cache_find_slot(struct dedup_node_cache *nc, uint64_t lba);
int  dedup_cache_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx,
                      uint64_t lba, void *buf, size_t bsz);
void dedup_cache_insert(struct dedup_node_cache *nc, uint64_t lba, const void *data);
int  dedup_cache_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx,
                       uint64_t lba, const void *buf, size_t bsz);
int  dedup_cache_flush(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx);
void dedup_cache_free(struct dedup_node_cache *nc);

/* ------------------------------------------------------------------ */
/*  Dedup key set                                                      */
/* ------------------------------------------------------------------ */

struct dedup_key_set
{
    uint64_t *keys;
    uint32_t  capacity;
    uint32_t  count;
};

#define KEYSET_EMPTY    0ULL
#define KEYSET_INIT_CAP 4096

uint32_t keyset_hash(uint64_t key, uint32_t mask);

struct dedup_key_set *keyset_create(void);
void     keyset_insert(struct dedup_key_set *ks, uint64_t key);
int      keyset_contains(const struct dedup_key_set *ks, uint64_t key);
void     keyset_free(struct dedup_key_set *ks);
void     keyset_ingest_leaf(struct dedup_key_set *ks, const void *buf);
void     keyset_seed_from_cache(struct dedup_key_set *ks, const struct dedup_node_cache *nc);

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
void pending_insert(struct dedup_pending_buf *pb, const struct dedup_entry *entry);
const struct dedup_entry *pending_lookup(const struct dedup_pending_buf *pb, uint64_t hash);
void pending_free(struct dedup_pending_buf *pb);
int  pending_entry_cmp(const void *a, const void *b);

/* ------------------------------------------------------------------ */
/*  Global dedup lookup cache (hash → dedup_entry, immutable entries)   */
/* ------------------------------------------------------------------ */

/** Slot in the global dedup lookup cache (open-addressing hash table). */
struct dedup_lc_slot
{
    uint64_t hash;          ///< Hash key (0 + !valid = empty sentinel)
    uint64_t tree_lba;      ///< Dedup tree header LBA (distinguishes trees)
    uint64_t block_lba;     ///< Dedup block LBA
    uint64_t block_offset;  ///< Offset within the dedup block
    uint32_t valid;         ///< Non-zero when slot is occupied
};

/** Global dedup lookup cache — caches hash→dedup_entry across all FUSE calls. */
struct dedup_lookup_cache
{
    struct dedup_lc_slot *slots;
    uint32_t              capacity;  ///< Must be power of 2
    uint32_t              count;
    uint32_t              max_cap;   ///< Hard ceiling (stop growing beyond this)
    pthread_mutex_t       lock;
};

#define DEDUP_LC_INIT_CAP   131072  ///< 128K slots ≈ 4 MB initial
#define DEDUP_LC_MAX_CAP   4194304  ///< 4M slots ≈ 128 MB hard ceiling

struct dedup_lookup_cache *dedup_lc_create(void);
void  dedup_lc_free(struct dedup_lookup_cache *lc);
int   dedup_lc_get(struct dedup_lookup_cache *lc, uint64_t hash, uint64_t tree_lba, struct dedup_entry *out);
void  dedup_lc_put(struct dedup_lookup_cache *lc, uint64_t hash, uint64_t tree_lba, const struct dedup_entry *entry);

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

int nc_block_read(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx,
                  uint64_t lba, void *buf, size_t bsz);
int nc_block_write(struct dedup_node_cache *nc, struct obmafs3_ctx *ctx,
                   uint64_t lba, const void *buf, size_t bsz);

/* ------------------------------------------------------------------ */
/*  Tree list management (dedup_tree.c)                                */
/* ------------------------------------------------------------------ */

int dedup_tree_list_read(struct obmafs3_ctx *ctx, struct tree_list_header *hdr,
                        struct tree_list_entry **entries, uint64_t *count);

/* ------------------------------------------------------------------ */
/*  Leaf-scan warmup (dedup_tree.c)                                    */
/* ------------------------------------------------------------------ */

int collect_all_leaf_lbas(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                          uint8_t *buf, struct dedup_node_cache *nc,
                          uint64_t **out_lbas, uint64_t *out_count);
void keyset_warmup(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                   uint8_t *buf, struct dedup_node_cache *nc);
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
#define DEDUP_LEAF_CACHE_INIT { NULL, 0, 0, 0 }

int  dedup_leaf_cache_search(const struct dedup_leaf_cache *lc, uint64_t hash,
                             struct dedup_entry *out);
void dedup_leaf_cache_populate(struct dedup_leaf_cache *lc, const uint8_t *buf,
                               size_t block_size);
int  dedup_lookup_cached(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                         uint64_t tree_lba, uint64_t hash,
                         struct dedup_entry *entry,
                         struct dedup_leaf_cache *lc);
void dedup_readahead_next(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                          uint64_t tree_lba, uint64_t next_hash,
                          uint64_t current_lba,
                          struct dedup_leaf_cache *lc);
int  lba_cmp(const void *a, const void *b);
int  dedup_batch_find_leaves(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                             const uint64_t *hashes, uint64_t count,
                             uint64_t *leaf_lbas, struct dedup_node_cache *nc);
int  dedup_find_leaf_lba(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                         uint64_t hash, uint64_t *out_leaf_lba,
                         uint8_t *buf, struct dedup_node_cache *nc);

int  pending_flush(struct dedup_pending_buf *pb, struct obmafs3_ctx *ctx,
                   struct btree_header *dedup_hdr, uint64_t dedup_hdr_lba);

/* ------------------------------------------------------------------ */
/*  Background compression (dedup_compress.c)                          */
/* ------------------------------------------------------------------ */

int  dedup_bg_wait(struct obmafs3_ctx *ctx, void **pjob);
int  dedup_bg_submit(struct compress_pool *pool, void **pjob,
                     struct obmafs3_ctx *ctx, uint8_t *data,
                     uint64_t block_lba, uint64_t offset, uint64_t capacity);

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
int  dedup_block_store(struct obmafs3_ctx *ctx, struct dedup_block_ctx *db,
                       const void *sector_data, size_t sector_len,
                       uint64_t *out_lba, uint64_t *out_offset,
                       struct compress_pool *pool, void **pending_job);
void dedup_block_free(struct dedup_block_ctx *db);

/* ------------------------------------------------------------------ */
/*  Sector map writing (dedup_write.c)                                 */
/* ------------------------------------------------------------------ */

int write_sector_map_batch(struct obmafs3_ctx *ctx, struct inode_record *inode,
                           const struct sector_map_entry *entries, uint64_t count);

#endif /* OBMAFS3_DEDUP_INTERNAL_H */
