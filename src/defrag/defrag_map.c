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

#include "defrag_tui.h"

#include <stdatomic.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Block-map drawing                                                  */
/* ------------------------------------------------------------------ */

/** Draw the colour legend row at the top of the map window. */
static void draw_legend(WINDOW *win)
{
    int legend_y = 0;
    int legend_x = 1;

    wattron(win, COLOR_PAIR(CP_DESKTOP) | A_BOLD);
    mvwprintw(win, legend_y, legend_x, "Legend:");
    wattroff(win, A_BOLD);

    legend_x += 9;

    /* Used block */
    wattron(win, COLOR_PAIR(CP_MAP_USED));
    mvwaddch(win, legend_y, legend_x, ACS_CKBOARD);
    wattroff(win, COLOR_PAIR(CP_MAP_USED));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    waddstr(win, " Used  ");

    /* Free block */
    wattron(win, COLOR_PAIR(CP_MAP_FREE));
    waddch(win, ACS_BULLET);
    wattroff(win, COLOR_PAIR(CP_MAP_FREE));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    waddstr(win, " Free  ");

    /* Tree nodes */
    wattron(win, COLOR_PAIR(CP_MAP_META));
    waddch(win, ACS_CKBOARD);
    wattroff(win, COLOR_PAIR(CP_MAP_META));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    waddstr(win, " Tree  ");

    /* Dedup data blocks */
    wattron(win, COLOR_PAIR(CP_MAP_DEDUP));
    waddch(win, ACS_CKBOARD);
    wattroff(win, COLOR_PAIR(CP_MAP_DEDUP));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    waddstr(win, " Dedup ");

    /* Metadata / superblock / bitmap */
    wattron(win, COLOR_PAIR(CP_MAP_SUPER));
    waddch(win, ACS_CKBOARD);
    wattroff(win, COLOR_PAIR(CP_MAP_SUPER));
    wattron(win, COLOR_PAIR(CP_DESKTOP));
    waddstr(win, " Meta");
}

/**
 * Get the colour pair and character for a block type.
 */
static void block_type_to_attr(uint8_t bt, int *color_pair, chtype *ch)
{
    switch(bt)
    {
        case BT_USED:
            *color_pair = CP_MAP_USED;
            *ch = ACS_CKBOARD;
            break;
        case BT_TREE:
            *color_pair = CP_MAP_META;
            *ch = ACS_CKBOARD;
            break;
        case BT_DEDUP:
            *color_pair = CP_MAP_DEDUP;
            *ch = ACS_CKBOARD;
            break;
        case BT_META:
            *color_pair = CP_MAP_SUPER;
            *ch = ACS_CKBOARD;
            break;
        case BT_FREE:
        default:
            *color_pair = CP_MAP_FREE;
            *ch = ACS_BULLET;
            break;
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

    draw_legend(tui->win_map);

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

    /* Map area: rows 1..map_h-1, columns 0..map_w-1 */
    int map_start_y = 1;
    int usable_rows = map_h - map_start_y;
    if(usable_rows < 1) usable_rows = 1;
    int usable_cols = map_w;
    uint64_t usable_cells = (uint64_t)usable_rows * (uint64_t)usable_cols;

    /* How many blocks each cell represents */
    uint64_t blocks_per_cell = (total + usable_cells - 1) / usable_cells;
    if(blocks_per_cell < 1) blocks_per_cell = 1;

    uint64_t block_idx = 0;

    for(int row = 0; row < usable_rows && block_idx < total; row++)
    {
        wmove(tui->win_map, map_start_y + row, 0);

        for(int col = 0; col < usable_cols && block_idx < total; col++)
        {
            uint8_t dt = dominant_type(bt, total, block_idx, blocks_per_cell);

            int cp;
            chtype glyph;
            block_type_to_attr(dt, &cp, &glyph);

            wattron(tui->win_map, COLOR_PAIR(cp));
            waddch(tui->win_map, glyph);
            wattroff(tui->win_map, COLOR_PAIR(cp));

            block_idx += blocks_per_cell;
        }
    }
}
