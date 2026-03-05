// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : defrag_compact.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     Data structures and API for the compaction engine.
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

#ifndef DEFRAG_COMPACT_H
#define DEFRAG_COMPACT_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/* Forward declarations */
struct obmafs3_ctx;
struct analysis_state;

/* ------------------------------------------------------------------ */
/*  Compaction phases                                                   */
/* ------------------------------------------------------------------ */

#define COMPACT_PHASE_PREPARE     0
#define COMPACT_PHASE_TREES       1 /**< Relocating B+Tree nodes          */
#define COMPACT_PHASE_DATA        2 /**< Moving non-dedup data blocks     */
#define COMPACT_PHASE_DEDUP       3 /**< Moving dedup data blocks         */
#define COMPACT_PHASE_REFS        4 /**< Updating references              */
#define COMPACT_PHASE_BITMAP      5 /**< Flushing bitmap & superblock     */
#define COMPACT_PHASE_DONE        6
#define COMPACT_NUM_PHASES        7

extern const char *compact_phase_labels[COMPACT_NUM_PHASES];

/* ------------------------------------------------------------------ */
/*  Tree relocation descriptor                                         */
/* ------------------------------------------------------------------ */

/** Describes a single B+Tree that needs to be relocated. */
struct tree_reloc
{
    const char *name;           /**< Human label ("Catalog", etc.)        */
    uint64_t    hdr_lba;        /**< LBA of the btree_header block        */
    size_t      idx_entry_size; /**< sizeof the tree's index entry        */
    size_t      child_lba_off;  /**< offsetof(child_lba) in index entry   */
    uint64_t   *node_lbas;      /**< Collected node LBAs (malloc'd)       */
    uint64_t    node_count;     /**< Number of nodes                      */
};

/* ------------------------------------------------------------------ */
/*  Shared compaction state                                            */
/* ------------------------------------------------------------------ */

/** Clump size: free space left between relocated trees (blocks). */
#define COMPACT_TREE_CLUMP 64

/** State shared between the compaction thread and the TUI. */
struct compact_state
{
    /* Inputs (set before starting) */
    struct obmafs3_ctx   *ctx;
    struct analysis_state *analysis; /**< From prior analysis pass          */

    /* Progress (atomics for lock-free TUI reads) */
    _Atomic uint64_t done_steps;
    _Atomic uint64_t total_steps;
    _Atomic int      phase;
    _Atomic int      finished;
    _Atomic int      error;
    _Atomic int      cancel_requested; /**< Set by TUI to request safe stop */

    /* Status message for TUI (written by engine, read by TUI) */
    _Atomic uint64_t current_src_lba;
    _Atomic uint64_t current_dst_lba;

    /* Timing */
    struct timespec start_time;

    /* Block move cursor: next free LBA to write to (data/dedup phases) */
    uint64_t write_cursor;

    /* Block move counters */
    uint64_t data_blocks_moved;
    uint64_t dedup_blocks_moved;
    uint64_t tree_nodes_moved;

    /* Dedup block relocation map (built during data+dedup phases,
     * consumed during reference update phase).
     * Stored as parallel arrays: reloc_old[i] → reloc_new[i]. */
    uint64_t *reloc_old;
    uint64_t *reloc_new;
    uint64_t  reloc_count;
    uint64_t  reloc_cap;

    /* Data block relocation map (built during data phase,
     * consumed during inode extent update phase). */
    uint64_t *data_reloc_old;
    uint64_t *data_reloc_new;
    uint64_t  data_reloc_count;
    uint64_t  data_reloc_cap;

    /* Tree node relocation map (built during tree relocation phase,
     * consumed by SME update to fix dedup_subchannel_lba in CD SMEs). */
    uint64_t *tree_reloc_old;
    uint64_t *tree_reloc_new;
    uint64_t  tree_reloc_count;
    uint64_t  tree_reloc_cap;
};

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * Run the full compaction process.
 *
 * Intended to be called from a background pthread.
 *
 * Phase 1: Move non-dedup data blocks to the beginning of the disk.
 * Phase 2: Move dedup data blocks immediately after.
 * Phase 3: Relocate all B+Trees to the end of the disk, contiguously,
 *           with COMPACT_TREE_CLUMP blocks of free space between them.
 *
 * @param state  Pre-initialised compaction state.
 * @return 0 on success, negative error code on failure.
 */
int defrag_compact_run(struct compact_state *state);

#endif /* DEFRAG_COMPACT_H */
