// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : defrag_tui.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     Internal header for the ncurses-based text user interface.
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

#ifndef DEFRAG_TUI_H
#define DEFRAG_TUI_H

#include <ncurses.h>
#include <pthread.h>
#include <stdatomic.h>

#include "defrag_analysis.h"
#include "defrag_compact.h"

/* ------------------------------------------------------------------ */
/*  Classic DOS colour pairs                                           */
/* ------------------------------------------------------------------ */

/** Colour pair identifiers (passed to COLOR_PAIR()). */
enum defrag_color
{
    CP_DESKTOP    = 1, /**< Blue background, cyan text (desktop area)              */
    CP_MENU_BAR   = 2, /**< White on black (menu bar / status bar)                 */
    CP_MENU_HOT   = 3, /**< Yellow on black (hot-key letter in menu)               */
    CP_MAP_FREE   = 4, /**< Cyan on blue   (free block = dotted texture)          */
    CP_MAP_USED   = 5, /**< White on white (used block = solid white square)       */
    CP_MAP_FRAG   = 6, /**< Red on red     (fragmented block = solid red square)   */
    CP_MAP_MOVING = 7, /**< Yellow on yellow (block being relocated = solid yellow)*/
    CP_DIALOG_BG  = 8, /**< Black on white (dialog body)                           */
    CP_DIALOG_BTN = 9, /**< White on green (dialog button)                         */
    CP_STATUS_BAR = 10, /**< Black on cyan  (bottom status bar)                    */
    CP_MENU_SEL   = 11, /**< Black on white (selected menu item)                   */
    CP_PROGRESS   = 12, /**< White on magenta (progress bar fill)                  */
    CP_MAP_META   = 13, /**< Magenta on magenta (B+Tree node = solid magenta)      */
    CP_MAP_DEDUP  = 14, /**< Green on green (dedup data = solid green square)      */
    CP_MAP_SUPER  = 15, /**< Cyan on cyan   (superblock/bitmap = solid cyan)       */
};

/* ------------------------------------------------------------------ */
/*  Window geometry                                                    */
/* ------------------------------------------------------------------ */

/** Fixed row heights. */
#define MENU_BAR_ROWS   1
#define STATUS_BAR_ROWS 1

/* ------------------------------------------------------------------ */
/*  TUI state                                                          */
/* ------------------------------------------------------------------ */

/** Top-level application state for the defrag TUI. */
struct defrag_tui
{
    WINDOW *win_menu;     /**< Menu bar window (top row)                */
    WINDOW *win_map;      /**< Block map window (middle area)           */
    WINDOW *win_status;   /**< Status bar window (bottom row)           */

    int rows;             /**< Terminal height                          */
    int cols;             /**< Terminal width                           */

    int running;          /**< Non-zero while the event loop is active  */

    /* Filesystem context (set by main before launching the TUI) */
    struct obmafs3_ctx *ctx;

    /* Analysis state (shared with background thread) */
    struct analysis_state analysis;
    pthread_t             analysis_thread;
    int                   analysis_thread_started;
    int                   summary_shown; /**< Set after auto-showing the summary dialog */

    /* Compaction state (shared with background thread) */
    struct compact_state compaction;
    pthread_t            compact_thread;
    int                  compact_thread_started;
    int                  compact_done_shown;
};

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * Initialise ncurses, create sub-windows, and set up DOS colour scheme.
 * Returns 0 on success, -1 on failure.
 */
int defrag_tui_init(struct defrag_tui *tui);

/**
 * Tear down ncurses and free sub-windows.
 */
void defrag_tui_shutdown(struct defrag_tui *tui);

/**
 * Redraw all static chrome (menu bar, status bar, map background).
 */
void defrag_tui_draw_chrome(struct defrag_tui *tui);

/**
 * Show the initial "run fsck first" modal dialog.
 * Returns 0 if the user chose OK, 1 if the user chose Exit.
 */
int defrag_tui_fsck_dialog(struct defrag_tui *tui);

/**
 * Run the main event loop until the user quits.
 */
void defrag_tui_run(struct defrag_tui *tui);

/**
 * Redraw the block map area from the analysis block_types array.
 * Shows a placeholder when no analysis has been run yet.
 */
void defrag_map_draw(struct defrag_tui *tui);

/**
 * Start the background analysis thread.
 * Returns 0 on success, -1 if already running or on error.
 */
int defrag_tui_start_analysis(struct defrag_tui *tui);

/**
 * Update the status bar with current analysis progress.
 */
void defrag_tui_update_status(struct defrag_tui *tui);

/**
 * Show a modal dialog with the analysis results summary.
 */
void defrag_tui_summary_dialog(struct defrag_tui *tui);

/**
 * Start the background compaction thread.
 * Analysis must have completed successfully first.
 * Returns 0 on success, -1 if not ready or on error.
 */
int defrag_tui_start_compaction(struct defrag_tui *tui);

#endif /* DEFRAG_TUI_H */
