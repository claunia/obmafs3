// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : defrag_analysis.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     Data structures and API for volume fragmentation analysis.
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

#ifndef DEFRAG_ANALYSIS_H
#define DEFRAG_ANALYSIS_H

#include <stdatomic.h>
#include <stdint.h>

/* Forward-declare the filesystem context so we don't pull in obmafs.h here. */
struct obmafs3_ctx;

/* ------------------------------------------------------------------ */
/*  Block-type classification                                          */
/* ------------------------------------------------------------------ */

/** Classification of a single filesystem block. */
enum block_type
{
    BT_FREE  = 0, /**< Not allocated in the bitmap       */
    BT_USED  = 1, /**< Standard data block               */
    BT_TREE  = 2, /**< B+Tree node (any tree)            */
    BT_DEDUP = 3, /**< Deduplicated data block           */
    BT_META  = 4  /**< Superblock, bitmap, tree headers  */
};

/* ------------------------------------------------------------------ */
/*  Per-tree fragmentation statistics                                  */
/* ------------------------------------------------------------------ */

#define ANALYSIS_MAX_TREES 32 /**< Upper limit on tracked trees */

/** Fragmentation statistics for a single B+Tree. */
struct tree_frag_stats
{
    char     name[64]; /**< Human-readable label ("Catalog", "Dedup 512", …) */
    uint64_t node_count;       /**< Total nodes walked           */
    uint64_t contiguous_runs;  /**< Number of contiguous runs    */
    double   frag_pct;         /**< Fragmentation percentage     */
};

/* ------------------------------------------------------------------ */
/*  Overall analysis result                                            */
/* ------------------------------------------------------------------ */

/** Aggregate results produced by the analysis pass. */
struct analysis_result
{
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t used_blocks;
    uint64_t tree_blocks;
    uint64_t dedup_blocks;
    uint64_t meta_blocks;

    /* Free-space fragmentation */
    uint64_t free_runs;   /**< Number of contiguous free-block runs */
    double   free_frag_pct;

    /* Per-tree fragmentation */
    struct tree_frag_stats trees[ANALYSIS_MAX_TREES];
    int                    tree_count;

    /* Dedup data-block fragmentation */
    uint64_t dedup_runs;
    double   dedup_frag_pct;
};

/* ------------------------------------------------------------------ */
/*  Shared analysis state (accessed from analysis thread + TUI thread) */
/* ------------------------------------------------------------------ */

/** State shared between the background analysis thread and the TUI. */
struct analysis_state
{
    /* Inputs (set by caller before starting) */
    struct obmafs3_ctx *ctx;        /**< Opened filesystem context                */

    /* Outputs (written by the analysis thread) */
    uint8_t               *block_types; /**< Per-block classification array        */
    uint64_t               total_blocks;
    struct analysis_result result;

    /* Progress tracking (atomics for lock-free TUI reads) */
    _Atomic uint64_t done_blocks;   /**< Blocks classified so far                 */
    _Atomic int      phase;         /**< Human-readable phase index (0-based)     */
    _Atomic int      finished;      /**< 1 when analysis is complete              */
    _Atomic int      error;         /**< Non-zero error code on failure           */
};

/* ------------------------------------------------------------------ */
/*  Phase labels (usable as indices into phase_labels[])               */
/* ------------------------------------------------------------------ */

#define ANALYSIS_PHASE_BITMAP    0
#define ANALYSIS_PHASE_TREES     1
#define ANALYSIS_PHASE_DEDUP     2
#define ANALYSIS_PHASE_CLASSIFY  3
#define ANALYSIS_PHASE_STATS     4
#define ANALYSIS_PHASE_DONE      5
#define ANALYSIS_NUM_PHASES      6

/** Human-readable labels for each analysis phase. */
extern const char *analysis_phase_labels[ANALYSIS_NUM_PHASES];

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * Run a full fragmentation analysis.
 *
 * This is intended to be called from a background pthread.  It populates
 * @p state->block_types and @p state->result, updating the atomic
 * progress counters as it goes so the TUI can refresh live.
 *
 * @param state Pre-initialised analysis state.  The caller must have set
 *              @c ctx and allocated @c block_types (total_blocks bytes).
 * @return 0 on success, negative error code on failure.
 */
int defrag_analysis_run(struct analysis_state *state);

/**
 * Compute fragmentation statistics from a classified block_types array.
 *
 * Called internally by defrag_analysis_run() but also usable standalone
 * after the block_types array has been populated by other means.
 */
void defrag_analysis_compute_stats(struct analysis_state *state);

#endif /* DEFRAG_ANALYSIS_H */
