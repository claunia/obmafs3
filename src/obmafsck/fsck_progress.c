// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_progress.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Progress bar helpers for obmafsck.
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

/** Clear the current progress line on stderr. */
void bar_clear(void)
{
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
}

/**
 * Print a unified progress bar on stderr.
 *
 * Renders:
 *   \r  Walking Dedup tree  ████████░░░░░░░  53% · 768/1450
 *
 * Uses Unicode block-drawing characters when colour is enabled,
 * falling back to [===>   ] when it is not.
 *
 * @param prefix  Label text (e.g. "Walking Dedup tree").
 * @param done    Number of items completed.
 * @param total   Total number of items.
 */
void print_bar(const char *prefix, uint64_t done, uint64_t total)
{
    const int bar_width = 30;
    double    frac      = total > 0 ? (double)done / (double)total : 1.0;
    if(frac > 1.0) frac = 1.0;
    int pct = (int)(frac * 100.0);

    if(g_use_color)
    {
        /* Unicode block-drawing progress bar */
        /* Each cell can show 8 sub-positions via block chars ▏▎▍▌▋▊▉█ */
        static const char *blocks[] = { " ", "\u258F", "\u258E", "\u258D",
                                        "\u258C", "\u258B", "\u258A", "\u2589", "\u2588" };
        double filled_f = frac * bar_width;
        int    filled_i = (int)filled_f;
        int    sub      = (int)((filled_f - filled_i) * 8.0);

        fprintf(stderr, "\r  \033[36m%-24s\033[0m ", prefix);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled_i)
                fprintf(stderr, "\033[36m\u2588\033[0m");
            else if(i == filled_i)
                fprintf(stderr, "\033[36m%s\033[0m", blocks[sub]);
            else
                fprintf(stderr, "\033[2m\u2591\033[0m");
        }
        fprintf(stderr, " %3d%% \033[2m\u00B7\033[0m %" PRIu64 "/%" PRIu64 "  ", pct, done, total);
    }
    else
    {
        /* ASCII fallback */
        int filled = (int)(frac * bar_width);
        fprintf(stderr, "\r  %-24s [", prefix);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled)       fputc('=', stderr);
            else if(i == filled) fputc('>', stderr);
            else                 fputc(' ', stderr);
        }
        fprintf(stderr, "] %3d%% %" PRIu64 "/%" PRIu64 "   ", pct, done, total);
    }
    fflush(stderr);
}
