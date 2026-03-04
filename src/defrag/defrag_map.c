// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : defrag_map.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     Block-map visualisation (placeholder — will be populated by the
//     defragmentation engine in a future commit).
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

#define _XOPEN_SOURCE_EXTENDED 1
#include "defrag_tui.h"

#include <stdatomic.h>
#include <inttypes.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>

/* ------------------------------------------------------------------ */
/*  Block-map drawing                                                  */
/* ------------------------------------------------------------------ */

/** Height reserved at the bottom of the map window for info panels. */
#define INFO_PANEL_HEIGHT 10

/** Draw the colour legend in a framed box in the bottom-right info area. */
static void draw_legend(WINDOW *win, int map_h, int map_w)
{
    /* Legend box dimensions */
    int box_w = 22;
    int box_h = 9;
    int box_y = map_h - box_h;
    int box_x = map_w - box_w - 1;

    if(box_y < 1) box_y = 1;
    if(box_x < 1) box_x = 1;

    /* Draw box frame on the desktop background */
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    for(int r = box_y; r < box_y + box_h && r < map_h; r++)
        for(int c = box_x; c < box_x + box_w && c < map_w; c++)
            mvwaddch(win, r, c, ' ');

    /* Frame */
    mvwaddch(win, box_y, box_x, ACS_ULCORNER);
    mvwaddch(win, box_y, box_x + box_w - 1, ACS_URCORNER);
    mvwaddch(win, box_y + box_h - 1, box_x, ACS_LLCORNER);
    mvwaddch(win, box_y + box_h - 1, box_x + box_w - 1, ACS_LRCORNER);
    for(int c = box_x + 1; c < box_x + box_w - 1; c++)
    {
        mvwaddch(win, box_y, c, ACS_HLINE);
        mvwaddch(win, box_y + box_h - 1, c, ACS_HLINE);
    }
    for(int r = box_y + 1; r < box_y + box_h - 1; r++)
    {
        mvwaddch(win, r, box_x, ACS_VLINE);
        mvwaddch(win, r, box_x + box_w - 1, ACS_VLINE);
    }

    /* Title */
    wattron(win, A_BOLD | COLOR_PAIR(CP_DESKTOP));
    mvwprintw(win, box_y, box_x + 2, " Legend ");
    wattroff(win, A_BOLD);

    int y = box_y + 1;
    int lx = box_x + 2;

    /* Used block */
    wattron(win, COLOR_PAIR(CP_MAP_USED));
    mvwaddch(win, y, lx, ' ');
    wattroff(win, COLOR_PAIR(CP_MAP_USED));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    mvwprintw(win, y++, lx + 2, "Used Block");

    /* Free block */
    wattron(win, COLOR_PAIR(CP_MAP_FREE));
    mvwaddch(win, y, lx, ' ');
    wattroff(win, COLOR_PAIR(CP_MAP_FREE));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    mvwprintw(win, y++, lx + 2, "Free Space");

    /* Tree node */
    wattron(win, COLOR_PAIR(CP_MAP_META));
    mvwaddch(win, y, lx, ' ');
    wattroff(win, COLOR_PAIR(CP_MAP_META));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    mvwprintw(win, y++, lx + 2, "B+Tree Node");

    /* Dedup data */
    wattron(win, COLOR_PAIR(CP_MAP_DEDUP));
    mvwaddch(win, y, lx, ' ');
    wattroff(win, COLOR_PAIR(CP_MAP_DEDUP));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    mvwprintw(win, y++, lx + 2, "Dedup Data");

    /* Metadata */
    wattron(win, COLOR_PAIR(CP_MAP_SUPER));
    mvwaddch(win, y, lx, ' ');
    wattroff(win, COLOR_PAIR(CP_MAP_SUPER));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    mvwprintw(win, y++, lx + 2, "Metadata");

    /* Moving block */
    wattron(win, COLOR_PAIR(CP_MAP_MOVING));
    mvwaddch(win, y, lx, ' ');
    wattroff(win, COLOR_PAIR(CP_MAP_MOVING));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    mvwprintw(win, y++, lx + 2, "Moving");
}

/**
 * Get the colour pair and glyph for a block type.
 * Used/allocated types use a solid space; free uses U+1FB90
 * (INVERSE MEDIUM SHADE) for the dithered texture like Norton Speed Disk.
 */
static void block_type_to_attr(uint8_t bt, int *cp, int *use_wide)
{
    *use_wide = 0;
    switch(bt)
    {
        case BT_USED:  *cp = CP_MAP_USED;  break;
        case BT_TREE:  *cp = CP_MAP_META;  break;
        case BT_DEDUP: *cp = CP_MAP_DEDUP; break;
        case BT_META:  *cp = CP_MAP_SUPER; break;
        case BT_FREE:
        default:       *cp = CP_MAP_FREE;  *use_wide = 1; break;
    }
}

/**
 * Determine the dominant block type in a range of blocks.
 *
 * Counts how many blocks of each type exist in [start, start+count)
 * and returns the type with the most blocks (ties broken by priority:
 * META > TREE > DEDUP > USED > FREE).
 */
static uint8_t dominant_type(const uint8_t *block_types, uint64_t total,
                             uint64_t start, uint64_t count)
{
    uint32_t counts[5] = {0};
    uint64_t end = start + count;
    if(end > total) end = total;

    for(uint64_t i = start; i < end; i++)
    {
        uint8_t t = block_types[i];
        if(t < 5) counts[t]++;
    }

    /* Priority order: META(4) > TREE(2) > DEDUP(3) > USED(1) > FREE(0) */
    static const uint8_t priority[] = {BT_META, BT_TREE, BT_DEDUP, BT_USED, BT_FREE};
    uint8_t  best = BT_FREE;
    uint32_t best_count = 0;

    for(int i = 0; i < 5; i++)
    {
        uint8_t t = priority[i];
        if(counts[t] > best_count)
        {
            best_count = counts[t];
            best = t;
        }
    }
    return best;
}

/**
 * Draw the block-map area.
 *
 * Each character cell represents a group of filesystem blocks.
 * The cell is coloured according to the dominant block type in
 * that group.  Shows a placeholder when no analysis data exists.
 */
void defrag_map_draw(struct defrag_tui *tui)
{
    int map_h, map_w;
    getmaxyx(tui->win_map, map_h, map_w);

    /* If no analysis data, show placeholder */
    const uint8_t *bt = tui->analysis.block_types;
    uint64_t total = tui->analysis.total_blocks;

    if(!bt || total == 0)
    {
        const char *placeholder = "[ Press A to analyse ]";
        int         ph_len      = (int)strlen(placeholder);
        int         ph_y        = map_h / 2;
        int         ph_x        = (map_w - ph_len) / 2;
        if(ph_x < 0) ph_x = 0;
        if(ph_y < 1) ph_y = 1;

        wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP) | A_DIM);
        mvwprintw(tui->win_map, ph_y, ph_x, "%s", placeholder);
        wattroff(tui->win_map, A_DIM);
        return;
    }

    /* Map area: rows 0 .. (map_h - INFO_PANEL_HEIGHT - 1)
     * The bottom INFO_PANEL_HEIGHT rows are reserved for info panels. */
    int map_rows = map_h - INFO_PANEL_HEIGHT;
    if(map_rows < 1) map_rows = 1;
    int usable_rows = map_rows;
    int usable_cols = map_w;
    uint64_t usable_cells = (uint64_t)usable_rows * (uint64_t)usable_cols;

    /* How many blocks each cell represents */
    uint64_t blocks_per_cell = (total + usable_cells - 1) / usable_cells;
    if(blocks_per_cell < 1) blocks_per_cell = 1;

    uint64_t block_idx = 0;

    /* U+2592 MEDIUM SHADE — classic DOS dithered free-space glyph */
    static const wchar_t free_glyph[] = { 0x2592, L'\0' };
    cchar_t free_cch;

    for(int row = 0; row < usable_rows && block_idx < total; row++)
    {
        wmove(tui->win_map, row, 0);

        for(int col = 0; col < usable_cols && block_idx < total; col++)
        {
            uint8_t dt = dominant_type(bt, total, block_idx, blocks_per_cell);
            int cp;
            int use_wide;
            block_type_to_attr(dt, &cp, &use_wide);

            if(use_wide)
            {
                setcchar(&free_cch, free_glyph, 0, (short)cp, NULL);
                wadd_wch(tui->win_map, &free_cch);
            }
            else
            {
                wattron(tui->win_map, COLOR_PAIR(cp));
                waddch(tui->win_map, ' ');
                wattroff(tui->win_map, COLOR_PAIR(cp));
            }

            block_idx += blocks_per_cell;
        }
    }

    /* Draw info panels in the reserved bottom area */
    draw_legend(tui->win_map, map_h, map_w);

    /* Draw block statistics panel on the bottom-left */
    {
        int panel_y = map_h - INFO_PANEL_HEIGHT;
        int panel_x = 1;
        int panel_w = map_w / 2 - 2;
        int panel_h = INFO_PANEL_HEIGHT;

        if(panel_w < 20) panel_w = 20;

        /* Frame */
        wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP));
        for(int r = panel_y; r < panel_y + panel_h && r < map_h; r++)
            for(int c = panel_x; c < panel_x + panel_w && c < map_w; c++)
                mvwaddch(tui->win_map, r, c, ' ');

        mvwaddch(tui->win_map, panel_y, panel_x, ACS_ULCORNER);
        mvwaddch(tui->win_map, panel_y, panel_x + panel_w - 1, ACS_URCORNER);
        mvwaddch(tui->win_map, panel_y + panel_h - 1, panel_x, ACS_LLCORNER);
        mvwaddch(tui->win_map, panel_y + panel_h - 1, panel_x + panel_w - 1, ACS_LRCORNER);
        for(int c = panel_x + 1; c < panel_x + panel_w - 1; c++)
        {
            mvwaddch(tui->win_map, panel_y, c, ACS_HLINE);
            mvwaddch(tui->win_map, panel_y + panel_h - 1, c, ACS_HLINE);
        }
        for(int r = panel_y + 1; r < panel_y + panel_h - 1; r++)
        {
            mvwaddch(tui->win_map, r, panel_x, ACS_VLINE);
            mvwaddch(tui->win_map, r, panel_x + panel_w - 1, ACS_VLINE);
        }

        wattron(tui->win_map, A_BOLD | COLOR_PAIR(CP_DESKTOP));
        mvwprintw(tui->win_map, panel_y, panel_x + 2, " Status ");
        wattroff(tui->win_map, A_BOLD);

        int y = panel_y + 1;
        wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP));

        if(atomic_load(&tui->analysis.finished))
        {
            struct analysis_result *r = &tui->analysis.result;
            mvwprintw(tui->win_map, y++, panel_x + 2, "Total:  %" PRIu64 " blocks", r->total_blocks);
            mvwprintw(tui->win_map, y++, panel_x + 2, "Used:   %" PRIu64, r->used_blocks);
            mvwprintw(tui->win_map, y++, panel_x + 2, "Free:   %" PRIu64, r->free_blocks);
            mvwprintw(tui->win_map, y++, panel_x + 2, "Tree:   %" PRIu64, r->tree_blocks);
            mvwprintw(tui->win_map, y++, panel_x + 2, "Dedup:  %" PRIu64, r->dedup_blocks);
            mvwprintw(tui->win_map, y++, panel_x + 2, "Meta:   %" PRIu64, r->meta_blocks);
            y++;
            mvwprintw(tui->win_map, y++, panel_x + 2, "1 block = %" PRIu64 " blocks",
                      blocks_per_cell);
        }
        else if(tui->analysis_thread_started)
        {
            int phase = atomic_load(&tui->analysis.phase);
            const char *label = (phase >= 0 && phase < ANALYSIS_NUM_PHASES)
                                    ? analysis_phase_labels[phase] : "Working";
            mvwprintw(tui->win_map, y++, panel_x + 2, "Analysing...");
            mvwprintw(tui->win_map, y++, panel_x + 2, "Phase: %s", label);
        }
        else
        {
            mvwprintw(tui->win_map, y++, panel_x + 2, "Press A to analyse");
        }
    }
}
