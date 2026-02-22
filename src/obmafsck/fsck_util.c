// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_util.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Terminal colour, timing, phase headers, result helpers,
//     usage, ask_fix, and small utilities for obmafsck.
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
/*  Terminal colour & symbol helpers                                    */
/* ------------------------------------------------------------------ */

int g_use_color = 0; /* set to 1 when stderr+stdout are ttys */

/* Initialise colour support (call once early in main) */
void init_color(void)
{
    /* Respect NO_COLOR convention (https://no-color.org/) */
    if(getenv("NO_COLOR")) return;

    /* Only enable when both stdout and stderr are terminals */
    if(isatty(STDOUT_FILENO) && isatty(STDERR_FILENO))
        g_use_color = 1;
}

/* ------------------------------------------------------------------ */
/*  Timing helpers                                                     */
/* ------------------------------------------------------------------ */

struct timespec g_start_time; /* set at programme start   */
int             g_phase_num;  /* current phase number     */

/** Store the current monotonic time in @p ts. */
void timer_now(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }

/** Return elapsed seconds between @p start and @p end. */
double timer_elapsed(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) + (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

/** Format a duration in seconds to a human-readable string. */
void fmt_duration(double secs, char *buf, size_t len)
{
    if(secs < 0.001)
        snprintf(buf, len, "<1ms");
    else if(secs < 1.0)
        snprintf(buf, len, "%.0fms", secs * 1000.0);
    else if(secs < 60.0)
        snprintf(buf, len, "%.1fs", secs);
    else
    {
        int m = (int)(secs / 60.0);
        snprintf(buf, len, "%dm%04.1fs", m, secs - m * 60.0);
    }
}

/* ------------------------------------------------------------------ */
/*  Phase header helper                                                */
/* ------------------------------------------------------------------ */

static struct timespec g_phase_start;

/**
 * Print a numbered phase header.
 *
 * Output: "Phase N: Title"  (bold when colour is enabled)
 */
void phase_begin(const char *title)
{
    g_phase_num++;
    timer_now(&g_phase_start);
    printf("\n%s── Phase %d: %s%s\n", CLR_BOLD, g_phase_num, title, CLR_RESET);
}

/**
 * Print the elapsed time for the current phase (right-aligned, dim).
 */
void phase_end(void)
{
    struct timespec now;
    timer_now(&now);
    char dur[32];
    fmt_duration(timer_elapsed(&g_phase_start, &now), dur, sizeof(dur));
    printf("  %s(%s)%s\n", CLR_DIM, dur, CLR_RESET);
}

/* ------------------------------------------------------------------ */
/*  Formatted-result helpers                                           */
/* ------------------------------------------------------------------ */

/**
 * Print a successful check result:  "  Label:  ✔ value"
 */
void result_ok(const char *label, const char *fmt, ...)
{
    printf("  %-20s %s ", label, SYM_OK);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * Print a failed check result:  "  Label:  ✘ value"
 */
void result_bad(const char *label, const char *fmt, ...)
{
    printf("  %-20s %s ", label, SYM_BAD);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * Print a fixed/repaired result:  "  Label:  ✦ value"
 */
void result_fixed(const char *label, const char *fmt, ...)
{
    printf("  %-20s %s ", label, SYM_FIXED);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * Print a simple labelled value (no status symbol).
 */
void result_info(const char *label, const char *fmt, ...)
{
    printf("  %-20s ", label);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/*  Options                                                            */
/* ------------------------------------------------------------------ */

/**
 * Print usage information for obmafsck.
 *
 * @param prog  Program name to display in the usage line.
 */
void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <device-or-file>\n"
            "\n"
            "Options:\n"
            "  -y              Assume 'yes' to all repair questions\n"
            "  -n              Assume 'no' to all repair questions\n"
            "  -s, --scrub     Verify checksums of all data blocks\n"
            "  -d, --dedup-stats  Show deduplication and compression statistics\n"
            "  -D, --dedup-stats-only  Show dedup stats without integrity checks\n"
            "  -v, --verify-hashes  Verify dedup and CD hashes against stored data\n"
            "  -f, --defrag    Defragment B+Tree nodes into contiguous blocks\n"
            "  -h, --help      Show this help message\n",
            prog);
}

/* Return value: 1 = yes, 0 = no */
/**
 * Prompt the user to fix a problem, or decide automatically.
 *
 * @param auto_yes  If non-zero, always return 1 (yes).
 * @param auto_no   If non-zero, always return 0 (no).
 * @param prompt    Question text displayed to the user.
 * @return 1 if the fix should be applied, 0 otherwise.
 */
int ask_fix(int auto_yes, int auto_no, const char *prompt)
{
    if(auto_yes) return 1;
    if(auto_no) return 0;

    printf("  %s%s%s [y/n] ", CLR_BOLD_YLW, prompt, CLR_RESET);
    fflush(stdout);

    int ch = fgetc(stdin);
    /* consume rest of line */
    int c2;
    while((c2 = fgetc(stdin)) != '\n' && c2 != EOF);
    return (ch == 'y' || ch == 'Y');
}

/** Return 1 if @p v is a power of two. */
int is_power_of_two(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }
