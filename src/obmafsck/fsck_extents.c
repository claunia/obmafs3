// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_extents.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Extent validation & refcount tree verification for obmafsck.
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

#include "fsck.h"

/* ------------------------------------------------------------------ */
/*  Extent validation                                                  */
/* ------------------------------------------------------------------ */

/**
 * Validate a single extent run.
 *
 * Checks performed:
 * - @c start_block must not be 0 (that is the superblock).
 * - @c start_block + @c block_count must not exceed @p total_blocks.
 * - @c logical_blocks must be >= @c block_count.
 * - If @c block_count is 0, both @c start_block and @c logical_blocks
 *   must also be 0 (unused slot).
 *
 * @param ext          Pointer to the extent run to validate.
 * @param total_blocks Total block count of the filesystem.
 * @param inode_id     Owning inode (for diagnostics).
 * @param slot         0-based slot/overflow index (for diagnostics).
 * @param label        "inline" or "overflow" (for diagnostics).
 * @param bad_count    Incremented for each violation found.
 */
static void validate_extent(const struct extent_run *ext, uint64_t total_blocks, uint64_t inode_id, int slot,
                            const char *label, uint64_t *bad_count)
{
    if(ext->block_count == 0 && ext->start_block == 0 && ext->logical_blocks == 0) return; /* unused slot — OK */

    if(ext->block_count == 0)
    {
        printf("    inode %" PRIu64 " %s extent %d: block_count=0 but start_block=%" PRIu64 " logical_blocks=%" PRIu64
               "\n",
               inode_id, label, slot, ext->start_block, ext->logical_blocks);
        (*bad_count)++;
        return;
    }

    if(ext->start_block == 0)
    {
        printf("    inode %" PRIu64 " %s extent %d: start_block=0 (superblock) "
               "block_count=%" PRIu64 "\n",
               inode_id, label, slot, ext->block_count);
        (*bad_count)++;
        return;
    }

    if(ext->start_block + ext->block_count > total_blocks)
    {
        printf("    inode %" PRIu64 " %s extent %d: out of bounds "
               "(start=%" PRIu64 " count=%" PRIu64 " total=%" PRIu64 ")\n",
               inode_id, label, slot, ext->start_block, ext->block_count, total_blocks);
        (*bad_count)++;
    }

    if(ext->logical_blocks < ext->block_count)
    {
        printf("    inode %" PRIu64 " %s extent %d: logical_blocks (%" PRIu64 ") < block_count (%" PRIu64 ")\n",
               inode_id, label, slot, ext->logical_blocks, ext->block_count);
        (*bad_count)++;
    }
}

/**
 * Walk all inode records and validate inline extents.
 *
 * For each inode leaf record, calls validate_extent() on each of the
 * 8 inline extent slots.  When a bad extent is found the user is
 * offered the option to zero it (clearing both the data reference and
 * the corresponding file_size contribution).
 *
 * @param ctx          Filesystem context.
 * @param total_blocks Total block count of the filesystem.
 * @param auto_yes     If nonzero, always repair.
 * @param auto_no      If nonzero, never repair.
 * @param bad_count    Output: total bad extents found.
 * @param fixed_count  Output: total bad extents cleared.
 */
static void validate_inline_extents(struct obmafs3_ctx *ctx, uint64_t total_blocks, int auto_yes, int auto_no,
                                    uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack)
    {
        free(buf);
        return;
    }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        return;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: validate each inode's inline extents */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct inode_record rec;
            memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

            uint64_t this_bad = 0;
            for(int e = 0; e < 8; e++)
                validate_extent(&rec.extents[e], total_blocks, rec.inode_id, e, "inline", &this_bad);

            if(this_bad > 0)
            {
                *bad_count += this_bad;

                if(ask_fix(auto_yes, auto_no, "    Clear bad inline extent(s)?"))
                {
                    int changed = 0;
                    for(int e = 0; e < 8; e++)
                    {
                        struct extent_run *ext = &rec.extents[e];
                        int                bad = 0;

                        if(ext->block_count == 0 && ext->start_block == 0 && ext->logical_blocks == 0) continue;
                        if(ext->block_count == 0) bad = 1;
                        if(ext->start_block == 0 && ext->block_count != 0) bad = 1;
                        if(ext->block_count != 0 && ext->start_block + ext->block_count > total_blocks) bad = 1;
                        if(ext->block_count != 0 && ext->logical_blocks < ext->block_count) bad = 1;

                        if(bad)
                        {
                            memset(ext, 0, sizeof(*ext));
                            changed++;
                            (*fixed_count)++;
                        }
                    }
                    if(changed)
                    {
                        /* Re-compute file_size from remaining valid extents */
                        uint64_t logical_total = 0;
                        for(int e = 0; e < 8; e++) logical_total += rec.extents[e].logical_blocks;
                        rec.file_size = logical_total * ctx->sb.block_size;

                        obmafs3_inode_put(ctx, &rec);
                    }
                }
            }
        }
    }

    free(buf);
    free(stack);
}

/**
 * Walk all overflow extent records and validate each one.
 *
 * Reports every overflow extent that fails the same structural checks
 * applied to inline extents.
 *
 * @param ctx          Filesystem context.
 * @param total_blocks Total block count of the filesystem.
 * @param bad_count    Output: total bad overflow extents found.
 */
static void validate_overflow_extents(struct obmafs3_ctx *ctx, uint64_t total_blocks, uint64_t *bad_count)
{
    *bad_count = 0;

    if(ctx->overflow_hdr.root_node_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack)
    {
        free(buf);
        return;
    }

    stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        if(nhdr.level > 0)
        {
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        return;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: validate each overflow_extent */
        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));

            struct extent_run ext;
            ext.start_block    = oe.start_block;
            ext.block_count    = oe.block_count;
            ext.logical_blocks = oe.logical_count;

            validate_extent(&ext, total_blocks, oe.inode_id, (int)i, "overflow", bad_count);
        }
    }

    free(buf);
    free(stack);
}

/* ------------------------------------------------------------------ */
/*  Refcount tree validation                                           */
/* ------------------------------------------------------------------ */

/**
 * Internal: insert or add to an LBA→count mapping in a sorted array.
 * Returns the new count on success, 0 on allocation failure.
 */
struct lba_count
{
    uint64_t lba;
    uint32_t count;
};

static uint64_t lba_map_add(struct lba_count **map, uint64_t *cap, uint64_t *len, uint64_t lba)
{
    /* Binary search for existing entry */
    uint64_t lo = 0, hi = *len;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if((*map)[mid].lba < lba)
            lo = mid + 1;
        else
            hi = mid;
    }

    if(lo < *len && (*map)[lo].lba == lba)
    {
        (*map)[lo].count++;
        return *len;
    }

    /* Insert new entry */
    if(*len >= *cap)
    {
        uint64_t          new_cap = *cap ? *cap * 2 : 4096;
        struct lba_count *tmp     = realloc(*map, (size_t)(new_cap * sizeof(struct lba_count)));
        if(!tmp) return 0;
        *map = tmp;
        *cap = new_cap;
    }

    if(lo < *len) memmove(&(*map)[lo + 1], &(*map)[lo], (size_t)(*len - lo) * sizeof(struct lba_count));

    (*map)[lo].lba   = lba;
    (*map)[lo].count = 1;
    (*len)++;
    return *len;
}

/**
 * Binary search for an LBA in a sorted lba_count array.
 * Returns the index if found, or UINT64_MAX if not.
 */
static uint64_t lba_map_find(const struct lba_count *map, uint64_t len, uint64_t lba)
{
    uint64_t lo = 0, hi = len;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if(map[mid].lba < lba)
            lo = mid + 1;
        else
            hi = mid;
    }
    if(lo < len && map[lo].lba == lba) return lo;
    return UINT64_MAX;
}

/**
 * Verify the refcount tree against actual block sharing across inodes.
 *
 * Phase 1: Walk all inode extents (inline + overflow) and count how
 *          many inodes reference each physical data block.
 * Phase 2: Walk the refcount tree leaf nodes and collect stored records.
 * Phase 3: Compare expected vs stored refcounts, reporting and optionally
 *          fixing mismatches.
 *
 * Blocks referenced by exactly one inode have an implicit refcount of 1
 * and should NOT appear in the refcount tree.  Blocks with refcount > 1
 * must appear with the correct count.
 *
 * @param ctx        Filesystem context.
 * @param auto_yes   If nonzero, always repair.
 * @param auto_no    If nonzero, never repair.
 * @param bad_count  Output: number of mismatches found.
 * @param fix_count  Output: number of mismatches repaired.
 */
void verify_refcount_tree(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, uint64_t *bad_count, uint64_t *fix_count)
{
    *bad_count = 0;
    *fix_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    /* ---- Phase 1: build expected refcount map from all inode extents ---- */
    struct lba_count *expected = NULL;
    uint64_t          exp_cap  = 0;
    uint64_t          exp_len  = 0;

    /* 1a: Inline extents from inode tree */
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack)
        {
            free(buf);
            return;
        }

        stack[stk_size++] = root_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];
            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(buf);
                            free(stack);
                            free(expected);
                            return;
                        }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: collect physical blocks from each inode's inline extents */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct inode_record rec;
                memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

                for(int e = 0; e < 8; e++)
                {
                    if(rec.extents[e].block_count == 0 || rec.extents[e].start_block == 0) continue;

                    for(uint64_t b = 0; b < rec.extents[e].block_count; b++)
                    {
                        if(!lba_map_add(&expected, &exp_cap, &exp_len, rec.extents[e].start_block + b))
                        {
                            free(buf);
                            free(stack);
                            free(expected);
                            return;
                        }
                    }
                }
            }
        }

        free(stack);
    }

    /* 1b: Overflow extents */
    if(ctx->overflow_hdr.root_node_lba != 0)
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack)
        {
            free(buf);
            free(expected);
            return;
        }

        stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];
            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(buf);
                            free(stack);
                            free(expected);
                            return;
                        }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: collect physical blocks from overflow extents */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct overflow_extent oe;
                memcpy(&oe, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(oe), sizeof(oe));

                if(oe.block_count == 0 || oe.start_block == 0) continue;

                for(uint64_t b = 0; b < oe.block_count; b++)
                {
                    if(!lba_map_add(&expected, &exp_cap, &exp_len, oe.start_block + b))
                    {
                        free(buf);
                        free(stack);
                        free(expected);
                        return;
                    }
                }
            }
        }

        free(stack);
    }

    /* ---- Phase 2: collect stored refcount records ---- */
    struct lba_count *stored  = NULL;
    uint64_t          sto_cap = 0;
    uint64_t          sto_len = 0;

    if(ctx->refcount_hdr.root_node_lba != 0)
    {
        uint64_t lba = ctx->refcount_hdr.root_node_lba;

        /* Descend to left-most leaf */
        while(lba != 0)
        {
            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                /* Walk leaf chain */
                while(lba != 0)
                {
                    if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;
                    memcpy(&nhdr, buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                    const uint8_t *rp = buf + sizeof(struct btree_node_header);
                    for(uint16_t i = 0; i < nhdr.node_keys; i++)
                    {
                        struct refcount_record rr;
                        memcpy(&rr, rp + (size_t)i * sizeof(rr), sizeof(rr));

                        if(sto_len >= sto_cap)
                        {
                            sto_cap               = sto_cap ? sto_cap * 2 : 256;
                            struct lba_count *tmp = realloc(stored, (size_t)(sto_cap * sizeof(*tmp)));
                            if(!tmp)
                            {
                                free(buf);
                                free(expected);
                                free(stored);
                                return;
                            }
                            stored = tmp;
                        }
                        stored[sto_len].lba   = rr.lba;
                        stored[sto_len].count = rr.ref_count;
                        sto_len++;
                    }

                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

    /* ---- Phase 3: compare expected vs stored ---- */

    /* 3a: Check blocks that should have refcount > 1 */
    for(uint64_t i = 0; i < exp_len; i++)
    {
        uint32_t exp_rc = expected[i].count;
        if(exp_rc <= 1) continue; /* Implicit refcount 1 — should NOT be in the tree */

        /* Look up in stored records */
        uint64_t si = lba_map_find(stored, sto_len, expected[i].lba);
        if(si == UINT64_MAX)
        {
            /* Should be in tree but is not */
            printf("    LBA %" PRIu64 ": expected refcount %" PRIu32 ", not in tree\n", expected[i].lba, exp_rc);
            (*bad_count)++;
            if(ask_fix(auto_yes, auto_no, "    Insert correct refcount?"))
            {
                int rc = obmafs3_refcount_set(ctx, expected[i].lba, exp_rc);
                if(rc == OBMAFS3_OK)
                    (*fix_count)++;
                else
                    fprintf(stderr, "    Error: could not set refcount: %d\n", rc);
            }
        }
        else if(stored[si].count != exp_rc)
        {
            printf("    LBA %" PRIu64 ": stored refcount %" PRIu32 ", expected %" PRIu32 "\n", expected[i].lba,
                   stored[si].count, exp_rc);
            (*bad_count)++;
            if(ask_fix(auto_yes, auto_no, "    Fix refcount?"))
            {
                int rc = obmafs3_refcount_set(ctx, expected[i].lba, exp_rc);
                if(rc == OBMAFS3_OK)
                    (*fix_count)++;
                else
                    fprintf(stderr, "    Error: could not set refcount: %d\n", rc);
            }
            /* Mark as verified by zeroing (to detect stale entries below) */
            stored[si].count = 0;
        }
        else
        {
            /* Correct — mark as verified */
            stored[si].count = 0;
        }
    }

    /* 3b: Check for stale entries in the refcount tree
     * (entries for blocks that are not shared, or not referenced at all) */
    for(uint64_t i = 0; i < sto_len; i++)
    {
        if(stored[i].count == 0) continue; /* Already verified in 3a */

        /* This entry exists in the tree but shouldn't (block isn't shared) */
        uint64_t ei     = lba_map_find(expected, exp_len, stored[i].lba);
        uint32_t actual = (ei != UINT64_MAX) ? expected[ei].count : 0;

        if(actual <= 1)
        {
            printf("    LBA %" PRIu64 ": stale refcount %" PRIu32 " in tree (actual %s)\n", stored[i].lba,
                   stored[i].count, actual == 0 ? "unallocated/unreferenced" : "1");
            (*bad_count)++;
            if(ask_fix(auto_yes, auto_no, "    Remove stale refcount entry?"))
            {
                /* Setting to 1 removes the entry from the tree */
                int rc = obmafs3_refcount_set(ctx, stored[i].lba, 1);
                if(rc == OBMAFS3_OK)
                    (*fix_count)++;
                else
                    fprintf(stderr, "    Error: could not remove refcount: %d\n", rc);
            }
        }
    }

    free(buf);
    free(expected);
    free(stored);
}

/**
 * Verify that each inode's file_size matches the sum of its extent
 * logical blocks (inline + overflow) multiplied by block_size.
 *
 * Directories (file_type == kFileTypeDirectory) are skipped because
 * they have no data extents.
 *
 * When a mismatch is detected, the user is offered to update
 * file_size to match the extent sum.
 *
 * @param ctx          Filesystem context.
 * @param auto_yes     If nonzero, always repair.
 * @param auto_no      If nonzero, never repair.
 * @param bad_count    Output: number of mismatches found.
 * @param fixed_count  Output: number of mismatches repaired.
 */
static void check_file_size_vs_extents(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, uint64_t *bad_count,
                                       uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    /* --- Phase 1: collect per-inode overflow logical_count sums --- */

    /* Hash map: simple open-addressing table mapping inode_id → sum */
    typedef struct
    {
        uint64_t inode_id;
        uint64_t logical_sum;
    } ovf_entry_t;

    uint64_t     ovf_cap   = 0;
    uint64_t     ovf_count = 0;
    ovf_entry_t *ovf_map   = NULL;

    if(ctx->overflow_hdr.root_node_lba != 0)
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack)
        {
            free(buf);
            return;
        }

        stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];

            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(buf);
                            free(stack);
                            free(ovf_map);
                            return;
                        }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: accumulate overflow logical_count per inode_id */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct overflow_extent oe;
                memcpy(&oe, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(oe), sizeof(oe));

                /* Linear scan (sufficient for fsck; inodes with overflow are rare) */
                int found = 0;
                for(uint64_t j = 0; j < ovf_count; j++)
                {
                    if(ovf_map[j].inode_id == oe.inode_id)
                    {
                        ovf_map[j].logical_sum += oe.logical_count;
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    if(ovf_count >= ovf_cap)
                    {
                        ovf_cap          = ovf_cap ? ovf_cap * 2 : 64;
                        ovf_entry_t *tmp = realloc(ovf_map, ovf_cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(buf);
                            free(stack);
                            free(ovf_map);
                            return;
                        }
                        ovf_map = tmp;
                    }
                    ovf_map[ovf_count].inode_id    = oe.inode_id;
                    ovf_map[ovf_count].logical_sum = oe.logical_count;
                    ovf_count++;
                }
            }
        }

        free(stack);
    }

    /* --- Phase 2: walk inodes, compare file_size vs extent sum --- */
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack)
        {
            free(buf);
            free(ovf_map);
            return;
        }

        stack[stk_size++] = root_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];

            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header hdr;
            memcpy(&hdr, buf, sizeof(hdr));
            if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(hdr.level > 0)
            {
                for(uint16_t i = 0; i < hdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(buf);
                            free(stack);
                            free(ovf_map);
                            return;
                        }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: check each inode */
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct inode_record rec;
                memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

                /* Skip directories — they have no data extents */
                if(rec.file_type == kFileTypeDirectory) continue;

                /* Skip media/CD images — file_size reflects logical disk size,
                 * not extent capacity, because data is stored via dedup trees */
                if(rec.file_type == kFileTypeMediaImage || rec.file_type == kFileTypeCompactDiscImage) continue;

                /* Sum inline extents */
                uint64_t logical_sum = 0;
                for(int e = 0; e < 8; e++) logical_sum += rec.extents[e].logical_blocks;

                /* Add overflow extents for this inode */
                for(uint64_t j = 0; j < ovf_count; j++)
                {
                    if(ovf_map[j].inode_id == rec.inode_id)
                    {
                        logical_sum += ovf_map[j].logical_sum;
                        break;
                    }
                }

                uint64_t expected_size = logical_sum * bsz;

                /*
                 * The last extent's logical coverage may exceed the actual
                 * file_size (partial last block).  So file_size must be:
                 *   (total_logical - last_extent_logical) * bsz < file_size <= total_logical * bsz
                 *
                 * Simplified: file_size must not exceed extent capacity,
                 * and extent capacity minus one extent worth must not exceed file_size.
                 * But for files with zero extents, file_size must be 0.
                 */
                if(logical_sum == 0)
                {
                    if(rec.file_size != 0)
                    {
                        printf("    inode %" PRIu64 ": file_size=%" PRIu64 " but no extents (expected 0)\n",
                               rec.inode_id, rec.file_size);
                        (*bad_count)++;
                        if(ask_fix(auto_yes, auto_no, "    Set file_size to 0?"))
                        {
                            rec.file_size = 0;
                            obmafs3_inode_put(ctx, &rec);
                            (*fixed_count)++;
                        }
                    }
                    continue;
                }

                if(rec.file_size > expected_size)
                {
                    printf("    inode %" PRIu64 ": file_size=%" PRIu64 " exceeds extent capacity %" PRIu64 " (%" PRIu64
                           " logical blocks)\n",
                           rec.inode_id, rec.file_size, expected_size, logical_sum);
                    (*bad_count)++;
                    if(ask_fix(auto_yes, auto_no, "    Clamp file_size to extent capacity?"))
                    {
                        rec.file_size = expected_size;
                        obmafs3_inode_put(ctx, &rec);
                        (*fixed_count)++;
                    }
                }
                else if(rec.file_size == 0 && logical_sum > 0)
                {
                    printf("    inode %" PRIu64 ": file_size=0 but has %" PRIu64 " logical blocks (capacity %" PRIu64
                           ")\n",
                           rec.inode_id, logical_sum, expected_size);
                    (*bad_count)++;
                    if(ask_fix(auto_yes, auto_no, "    Set file_size to extent capacity?"))
                    {
                        rec.file_size = expected_size;
                        obmafs3_inode_put(ctx, &rec);
                        (*fixed_count)++;
                    }
                }
            }
        }

        free(stack);
    }

    free(buf);
    free(ovf_map);
}

/**
 * Top-level extent validation: inline + overflow.
 *
 * @param ctx       Filesystem context.
 * @param auto_yes  If nonzero, always repair.
 * @param auto_no   If nonzero, never repair.
 * @param errors    In/out: incremented for each unfixed error.
 */
void check_extent_validity(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors)
{
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;

    printf("\n  %sExtent validation%s\n", CLR_BOLD, CLR_RESET);

    /* Inline extents */
    uint64_t inline_bad = 0, inline_fixed = 0;
    validate_inline_extents(ctx, total_blocks, auto_yes, auto_no, &inline_bad, &inline_fixed);

    if(inline_bad == 0) { result_ok("Inline extents:", ""); }
    else
    {
        if(inline_fixed > 0)
            result_fixed("Inline extents:", "%" PRIu64 " bad, %" PRIu64 " cleared", inline_bad, inline_fixed);
        else
            result_bad("Inline extents:", "%" PRIu64 " bad", inline_bad);
        *errors += (int)(inline_bad - inline_fixed);
    }

    /* Overflow extents */
    uint64_t overflow_bad = 0;
    validate_overflow_extents(ctx, total_blocks, &overflow_bad);

    if(overflow_bad == 0) { result_ok("Overflow extents:", ""); }
    else
    {
        result_bad("Overflow extents:", "%" PRIu64 " bad", overflow_bad);
        *errors += (int)overflow_bad;
    }

    /* ---- File size vs extent sum ---- */
    uint64_t sz_bad = 0, sz_fixed = 0;
    check_file_size_vs_extents(ctx, auto_yes, auto_no, &sz_bad, &sz_fixed);

    if(sz_bad == 0) { result_ok("File size check:", ""); }
    else
    {
        if(sz_fixed > 0)
            result_fixed("File size check:", "%" PRIu64 " mismatch, %" PRIu64 " fixed", sz_bad, sz_fixed);
        else
            result_bad("File size check:", "%" PRIu64 " mismatch", sz_bad);
        *errors += (int)(sz_bad - sz_fixed);
    }
}

/* ------------------------------------------------------------------ */
