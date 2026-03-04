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

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Block-map drawing                                                  */
/* ------------------------------------------------------------------ */

/**
 * Draw the block-map area.
 *
 * Currently this is a placeholder that draws the legend and an empty
 * blue desktop.  When the defrag engine is implemented, each character
 * cell will represent a group of blocks coloured according to their
 * state (free, used, fragmented, metadata, moving).
 */
void defrag_map_draw(struct defrag_tui *tui)
{
    int map_h, map_w;
    getmaxyx(tui->win_map, map_h, map_w);

    /* Draw legend at the top of the map area */
    int legend_y = 1;
    int legend_x = 2;

    wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP) | A_BOLD);
    mvwprintw(tui->win_map, legend_y, legend_x, "Legend:");
    wattroff(tui->win_map, A_BOLD);

    legend_x += 9;

    /* Used block */
    wattron(tui->win_map, COLOR_PAIR(CP_MAP_USED));
    mvwaddch(tui->win_map, legend_y, legend_x, ACS_CKBOARD);
    wattroff(tui->win_map, COLOR_PAIR(CP_MAP_USED));
    wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP));
    waddstr(tui->win_map, " Used  ");

    /* Free block */
    wattron(tui->win_map, COLOR_PAIR(CP_MAP_FREE));
    waddch(tui->win_map, ACS_BULLET);
    wattroff(tui->win_map, COLOR_PAIR(CP_MAP_FREE));
    wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP));
    waddstr(tui->win_map, " Free  ");

    /* Fragmented block */
    wattron(tui->win_map, COLOR_PAIR(CP_MAP_FRAG));
    waddch(tui->win_map, ACS_CKBOARD);
    wattroff(tui->win_map, COLOR_PAIR(CP_MAP_FRAG));
    wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP));
    waddstr(tui->win_map, " Frag  ");

    /* Metadata block */
    wattron(tui->win_map, COLOR_PAIR(CP_MAP_META));
    waddch(tui->win_map, ACS_CKBOARD);
    wattroff(tui->win_map, COLOR_PAIR(CP_MAP_META));
    wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP));
    waddstr(tui->win_map, " Meta  ");

    /* Moving block */
    wattron(tui->win_map, COLOR_PAIR(CP_MAP_MOVING));
    waddch(tui->win_map, ACS_CKBOARD);
    wattroff(tui->win_map, COLOR_PAIR(CP_MAP_MOVING));
    wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP));
    waddstr(tui->win_map, " Moving");

    /* --- Placeholder text centred in the map area --- */
    const char *placeholder = "[ Block map will appear here ]";
    int         ph_len      = (int)strlen(placeholder);
    int         ph_y        = map_h / 2;
    int         ph_x        = (map_w - ph_len) / 2;

    if(ph_x < 0) ph_x = 0;

    wattron(tui->win_map, COLOR_PAIR(CP_DESKTOP) | A_DIM);
    mvwprintw(tui->win_map, ph_y, ph_x, "%s", placeholder);
    wattroff(tui->win_map, A_DIM);

    (void)map_h; /* suppress unused warning when legend_y is the only use */
}
