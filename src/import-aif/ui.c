// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : ui.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-aif
//
// --[ Description ] ----------------------------------------------------------
//
//     Terminal UI helpers: colours, progress bars, formatted output.
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

#include "ui.h"

#include <stdarg.h>
#include <string.h>
#include <unistd.h>

int g_color = 0;

void ui_init(void) { g_color = isatty(STDERR_FILENO); }

void ui_banner(void)
{
    if(g_color)
    {
        fprintf(stderr,
                "\n"
                "  \033[1;36m┌──────────────────────────────────────────┐\033[0m\n"
                "  \033[1;36m│\033[0m  \033[1mimport-aif\033[0m"
                "  \033[2m·\033[0m  OBMAFS3 image importer   \033[1;36m│\033[0m\n"
                "  \033[1;36m└──────────────────────────────────────────┘\033[0m\n"
                "\n");
    }
    else
    {
        fprintf(stderr,
                "\n"
                "  +------------------------------------------+\n"
                "  |  import-aif  ·  OBMAFS3 image importer   |\n"
                "  +------------------------------------------+\n"
                "\n");
    }
}

void ui_phase(int num, const char *label)
{
    fprintf(stderr, "\n  %s[%d]%s %s%s%s\n", C_BOLD_CYAN, num, C_RESET, C_BOLD, label, C_RESET);
}

void ui_step(const char *label) { fprintf(stderr, "  %s %s\n", SYM_INFO, label); }

void ui_info(const char *key, const char *fmt, ...)
{
    fprintf(stderr, "      %s%-22s%s ", C_DIM, key, C_RESET);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void ui_ok(const char *fmt, ...)
{
    fprintf(stderr, "  %s ", SYM_OK);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void ui_warn(const char *fmt, ...)
{
    fprintf(stderr, "  %s ", SYM_WARN);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void ui_error(const char *fmt, ...)
{
    fprintf(stderr, "  %s %s", SYM_FAIL, C_RED);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", C_RESET);
}

void ui_progress(const char *label, uint64_t done, uint64_t total)
{
    const int bar_width = 30;
    double    frac      = total > 0 ? (double)done / (double)total : 1.0;
    if(frac > 1.0) frac = 1.0;
    int pct = (int)(frac * 100.0);

    if(g_color)
    {
        static const char *blocks[] = {" ",      "\u258F", "\u258E", "\u258D", "\u258C",
                                       "\u258B", "\u258A", "\u2589", "\u2588"};
        double             filled_f = frac * bar_width;
        int                filled_i = (int)filled_f;
        int                sub      = (int)((filled_f - filled_i) * 8.0);

        fprintf(stderr, "\r      \033[36m%-20s\033[0m ", label);
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
        int filled = (int)(frac * bar_width);
        fprintf(stderr, "\r      %-20s [", label);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled)
                fputc('=', stderr);
            else if(i == filled)
                fputc('>', stderr);
            else
                fputc(' ', stderr);
        }
        fprintf(stderr, "] %3d%% %" PRIu64 "/%" PRIu64 "   ", pct, done, total);
    }
    fflush(stderr);
}

void ui_progress_clear(void)
{
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
}

void ui_track_info(int seq, int64_t start, int64_t end, int64_t pregap, const char *mode, uint16_t ss)
{
    const char *icon = SYM_DATA;
    if(strcmp(mode, "Audio") == 0) icon = SYM_MUSIC;

    fprintf(stderr, "      %s %sTrack %2d%s  %s%-11s%s  LBA %" PRId64 "%s..%s%" PRId64 "  %s(%u B/sector",
            icon, C_BOLD, seq, C_RESET, C_CYAN, mode, C_RESET, start, C_DIM, C_RESET, end, C_DIM, (unsigned)ss);
    if(pregap > 0) fprintf(stderr, ", pregap %" PRId64, pregap);
    fprintf(stderr, ")%s\n", C_RESET);
}

void ui_summary(const char *src, const char *dst, double elapsed_s, uint64_t sectors, uint32_t sector_size)
{
    int    mins = (int)(elapsed_s / 60.0);
    double secs = elapsed_s - mins * 60.0;

    fprintf(stderr, "\n");
    if(g_color)
    {
        fprintf(stderr,
                "  \033[1;32m┌──────────────────────────────────────────┐\033[0m\n"
                "  \033[1;32m│\033[0m  \033[1mImport complete\033[0m"
                "                         \033[1;32m│\033[0m\n"
                "  \033[1;32m└──────────────────────────────────────────┘\033[0m\n");
    }
    else
    {
        fprintf(stderr,
                "  +------------------------------------------+\n"
                "  |  Import complete                        |\n"
                "  +------------------------------------------+\n");
    }

    ui_info("Source:", "%s", src);
    ui_info("Destination:", "%s", dst);
    ui_info("Sectors:", "%" PRIu64, sectors);
    if(mins > 0)
        ui_info("Elapsed:", "%dm %.1fs", mins, secs);
    else
        ui_info("Elapsed:", "%.1fs", secs);
    if(elapsed_s > 0.0)
    {
        double bytes     = (double)sectors * (double)sector_size;
        double mib_per_s = bytes / (1024.0 * 1024.0) / elapsed_s;
        ui_info("Speed:", "%.2f MiB/s", mib_per_s);
    }
    fprintf(stderr, "\n");
}
