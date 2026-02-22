/*
 * fsck.h — Shared declarations for the obmafsck filesystem checker.
 *
 * Every .c file in the obmafsck directory includes this header to
 * access common helpers, macros, and global state.
 */
#ifndef OBMAFSCK_FSCK_H
#define OBMAFSCK_FSCK_H

#include "obmafs.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

/* ------------------------------------------------------------------ */
/*  Terminal colour & symbol helpers                                    */
/* ------------------------------------------------------------------ */

extern int g_use_color; /* set to 1 when stderr+stdout are ttys */

/* ANSI SGR codes — only emitted when g_use_color is set */
#define CLR_RESET     (g_use_color ? "\033[0m"    : "")
#define CLR_BOLD      (g_use_color ? "\033[1m"    : "")
#define CLR_DIM       (g_use_color ? "\033[2m"    : "")
#define CLR_RED       (g_use_color ? "\033[31m"   : "")
#define CLR_GREEN     (g_use_color ? "\033[32m"   : "")
#define CLR_YELLOW    (g_use_color ? "\033[33m"   : "")
#define CLR_BLUE      (g_use_color ? "\033[34m"   : "")
#define CLR_CYAN      (g_use_color ? "\033[36m"   : "")
#define CLR_BOLD_RED  (g_use_color ? "\033[1;31m" : "")
#define CLR_BOLD_GRN  (g_use_color ? "\033[1;32m" : "")
#define CLR_BOLD_YLW  (g_use_color ? "\033[1;33m" : "")
#define CLR_BOLD_CYAN (g_use_color ? "\033[1;36m" : "")

/* Unicode status symbols (with colour) for structured output */
#define SYM_OK      (g_use_color ? "\033[32m\u2714\033[0m" : "OK")      /* ✔ green  */
#define SYM_BAD     (g_use_color ? "\033[31m\u2718\033[0m" : "BAD")     /* ✘ red    */
#define SYM_WARN    (g_use_color ? "\033[33m\u26A0\033[0m" : "WARNING") /* ⚠ yellow */
#define SYM_FIXED   (g_use_color ? "\033[34m\u2726\033[0m" : "FIXED")   /* ✦ blue   */
#define SYM_SKIP    (g_use_color ? "\033[2m\u2500\033[0m"  : "-")       /* ─ dim    */

/* ------------------------------------------------------------------ */
/*  Timing state (owned by fsck_util.c)                                */
/* ------------------------------------------------------------------ */

extern struct timespec g_start_time;
extern int             g_phase_num;

/* ------------------------------------------------------------------ */
/*  Ordering tree-type identifiers for B+Tree key comparisons          */
/* ------------------------------------------------------------------ */

#define ORD_UINT64_KEY  0  /**< key = first uint64_t (inode, dedup, cd_*, refcount) */
#define ORD_CATALOG     1  /**< key = (parent_id, name) */
#define ORD_OVERFLOW    2  /**< leaf: (inode_id, logical_offset), index: (inode_id, logical_offset) */
#define ORD_MEDIA_TAG   3  /**< key = (inode_id, tag_type) */
#define ORD_METADATA    4  /**< key = (inode_id, key[256]) */
#define ORD_METADATA_IDX 5 /**< key = (key[256], value[1025], inode_id) */

/* ------------------------------------------------------------------ */
/*  fsck_util.c — colour, timing, phase, result, options helpers       */
/* ------------------------------------------------------------------ */

void init_color(void);
void timer_now(struct timespec *ts);
double timer_elapsed(const struct timespec *start, const struct timespec *end);
void fmt_duration(double secs, char *buf, size_t len);
void phase_begin(const char *title);
void phase_end(void);

void result_ok(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void result_bad(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void result_fixed(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void result_info(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void usage(const char *prog);
int  ask_fix(int auto_yes, int auto_no, const char *prompt);
int  is_power_of_two(uint64_t v);

/* ------------------------------------------------------------------ */
/*  fsck_superblock.c                                                  */
/* ------------------------------------------------------------------ */

void validate_superblock_fields(struct obmafs3_sb *sb, int fd, uint64_t file_size,
                                int auto_yes, int auto_no, int *errors);

/* ------------------------------------------------------------------ */
/*  fsck_progress.c                                                    */
/* ------------------------------------------------------------------ */

void bar_clear(void);
void print_bar(const char *prefix, uint64_t done, uint64_t total);

/* ------------------------------------------------------------------ */
/*  fsck_btree.c                                                       */
/* ------------------------------------------------------------------ */

int walk_inode_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                           size_t child_lba_off, uint64_t **out_lbas, uint64_t *out_count,
                           uint32_t total_nodes, const char *label);

int walk_catalog_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba,
                             uint64_t **out_lbas, uint64_t *out_count);

int verify_btree_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                const char *tree_name, int auto_yes, int auto_no,
                                uint64_t *bad_count, uint64_t *fixed_count);

int ordering_key_cmp(const uint8_t *a, const uint8_t *b, int type, int is_leaf);

int fix_node_ordering(struct obmafs3_ctx *ctx, uint8_t *buf, size_t buf_size, uint64_t lba, int nblocks,
                      struct btree_node_header *hdr, int key_type, int is_leaf, size_t stride);

void verify_fix_total_nodes(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba,
                            uint64_t actual, const char *tree_name, const char *indent,
                            int auto_yes, int auto_no, int *errors);

void verify_fix_free_nodes(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba,
                           const char *tree_name, const char *indent,
                           int auto_yes, int auto_no, int *errors);

int patch_sibling_links(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t new_left, uint64_t new_right,
                        int nblocks);

int verify_fix_sibling_links(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                             size_t child_lba_off, int nblocks,
                             const char *tree_name, int auto_yes, int auto_no,
                             uint64_t *bad_count, uint64_t *fixed_count);

int verify_btree_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                          int key_type, size_t leaf_rec_size, size_t idx_entry_size,
                          const char *tree_name, int auto_yes, int auto_no,
                          uint64_t *bad_count, uint64_t *fixed_count);

int verify_meta_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                         int key_type, size_t leaf_rec_size, size_t idx_entry_size,
                         const char *tree_name, int auto_yes, int auto_no,
                         uint64_t *bad_count, uint64_t *fixed_count);

int walk_meta_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                          size_t child_lba_off, uint64_t **out_lbas, uint64_t *out_count);

int verify_meta_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                               const char *tree_name, int auto_yes, int auto_no,
                               uint64_t *bad_count, uint64_t *fixed_count);

/* ------------------------------------------------------------------ */
/*  fsck_collect.c                                                     */
/* ------------------------------------------------------------------ */

int collect_inode_data_blocks(struct obmafs3_ctx *ctx, uint64_t inode_root_lba,
                              uint64_t **out_lbas, uint64_t *out_count);

int collect_overflow_data_blocks(struct obmafs3_ctx *ctx,
                                 uint64_t **out_lbas, uint64_t *out_count);

int collect_media_tag_data_blocks(struct obmafs3_ctx *ctx,
                                  uint64_t **out_lbas, uint64_t *out_count);

int collect_dedup_blocks(struct obmafs3_ctx *ctx,
                         uint64_t **out_lbas, uint64_t *out_count);

int collect_free_chain_blocks(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                              uint64_t blocks_per_node,
                              uint64_t **out_lbas, uint64_t *out_count);

uint8_t *build_expected_bitmap(struct obmafs3_ctx *ctx, uint64_t total_blocks,
                               uint64_t bitmap_bytes, int *out_error);

/* ------------------------------------------------------------------ */
/*  fsck_scrub.c                                                       */
/* ------------------------------------------------------------------ */

void     print_progress(const char *label, uint64_t done, uint64_t total, uint64_t bad);
uint64_t scrub_data_blocks(struct obmafs3_ctx *ctx);
uint64_t scrub_dedup_data_blocks(struct obmafs3_ctx *ctx);

/* ------------------------------------------------------------------ */
/*  fsck_hash.c                                                        */
/* ------------------------------------------------------------------ */

uint64_t verify_cd_tree_hashes(struct obmafs3_ctx *ctx, const struct btree_header *hdr,
                               const char *label, size_t rec_size, size_t data_size);
uint64_t verify_dedup_hashes(struct obmafs3_ctx *ctx);

/* ------------------------------------------------------------------ */
/*  fsck_dedup.c                                                       */
/* ------------------------------------------------------------------ */

void print_human_size(uint64_t bytes);
int  compute_dedup_stats(struct obmafs3_ctx *ctx);

/* ------------------------------------------------------------------ */
/*  fsck_metadata.c                                                    */
/* ------------------------------------------------------------------ */

void check_metadata_bidirectional(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors);

/* ------------------------------------------------------------------ */
/*  fsck_crosscheck.c                                                  */
/* ------------------------------------------------------------------ */

int  collect_inode_ids(struct obmafs3_ctx *ctx, uint64_t **out_ids, uint64_t *out_count);
void cross_check_inodes_catalog(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors);

/* ------------------------------------------------------------------ */
/*  fsck_extents.c                                                     */
/* ------------------------------------------------------------------ */

void verify_refcount_tree(struct obmafs3_ctx *ctx, int auto_yes, int auto_no,
                          uint64_t *bad_count, uint64_t *fix_count);
void check_extent_validity(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors);

/* ------------------------------------------------------------------ */
/*  fsck_defrag.c                                                      */
/* ------------------------------------------------------------------ */

void defrag_all_trees(struct obmafs3_ctx *ctx, int auto_yes, int auto_no);

#endif /* OBMAFSCK_FSCK_H */
