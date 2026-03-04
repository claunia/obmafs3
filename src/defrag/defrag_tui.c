// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : defrag_tui.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     ncurses TUI: initialisation, colour scheme, menu bar, status bar,
//     fsck warning dialog, and main event loop.
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
#include "obmafs.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Colour-pair initialisation (classic DOS / Norton-style palette)    */
/* ------------------------------------------------------------------ */

static void init_dos_colors(void)
{
    start_color();

    /* Ensure we get the standard 8 ANSI colours. */
    if(can_change_color())
    {
        /* Keep terminal defaults — most terminals already expose the
         * classic CGA palette as colours 0-7.                          */
    }

    /* Desktop: bright cyan text on blue background */
    init_pair(CP_DESKTOP,    COLOR_CYAN,    COLOR_BLUE);
    /* Menu bar: bright white text on black */
    init_pair(CP_MENU_BAR,   COLOR_WHITE,   COLOR_BLACK);
    /* Menu hot key: yellow on black */
    init_pair(CP_MENU_HOT,   COLOR_YELLOW,  COLOR_BLACK);
    /* Map: free = white on blue */
    init_pair(CP_MAP_FREE,   COLOR_WHITE,   COLOR_BLUE);
    /* Map: used = white on white (solid block) */
    init_pair(CP_MAP_USED,   COLOR_WHITE,   COLOR_WHITE);
    /* Map: fragmented = red on blue */
    init_pair(CP_MAP_FRAG,   COLOR_RED,     COLOR_BLUE);
    /* Map: currently moving = yellow on blue */
    init_pair(CP_MAP_MOVING, COLOR_YELLOW,  COLOR_BLUE);
    /* Dialog body: black text on white */
    init_pair(CP_DIALOG_BG,  COLOR_BLACK,   COLOR_WHITE);
    /* Dialog button: white on green */
    init_pair(CP_DIALOG_BTN, COLOR_WHITE,   COLOR_GREEN);
    /* Status bar: black text on cyan */
    init_pair(CP_STATUS_BAR, COLOR_BLACK,   COLOR_CYAN);
    /* Menu selected item: black on white */
    init_pair(CP_MENU_SEL,   COLOR_BLACK,   COLOR_WHITE);
    /* Progress bar fill: white on magenta */
    init_pair(CP_PROGRESS,   COLOR_WHITE,   COLOR_MAGENTA);
    /* Map: metadata / B+Tree nodes = magenta on blue */
    init_pair(CP_MAP_META,   COLOR_MAGENTA, COLOR_BLUE);
    /* Map: dedup data blocks = green on blue */
    init_pair(CP_MAP_DEDUP,  COLOR_GREEN,   COLOR_BLUE);
    /* Map: superblock / bitmap / tree headers = cyan on black */
    init_pair(CP_MAP_SUPER,  COLOR_CYAN,    COLOR_BLACK);
}

/* ------------------------------------------------------------------ */
/*  Window creation helpers                                            */
/* ------------------------------------------------------------------ */

/** (Re-)create the three sub-windows from the current terminal size. */
static void create_subwindows(struct defrag_tui *tui)
{
    getmaxyx(stdscr, tui->rows, tui->cols);

    int map_rows = tui->rows - MENU_BAR_ROWS - STATUS_BAR_ROWS;

    if(map_rows < 1) map_rows = 1;

    tui->win_menu   = newwin(MENU_BAR_ROWS, tui->cols, 0, 0);
    tui->win_map    = newwin(map_rows, tui->cols, MENU_BAR_ROWS, 0);
    tui->win_status = newwin(STATUS_BAR_ROWS, tui->cols, tui->rows - STATUS_BAR_ROWS, 0);
}

/** Destroy existing sub-windows (safe if NULL). */
static void destroy_subwindows(struct defrag_tui *tui)
{
    if(tui->win_menu)   { delwin(tui->win_menu);   tui->win_menu   = NULL; }
    if(tui->win_map)    { delwin(tui->win_map);    tui->win_map    = NULL; }
    if(tui->win_status) { delwin(tui->win_status); tui->win_status = NULL; }
}

/* ------------------------------------------------------------------ */
/*  Public: init / shutdown                                            */
/* ------------------------------------------------------------------ */

int defrag_tui_init(struct defrag_tui *tui)
{
    memset(tui, 0, sizeof(*tui));

    if(!initscr()) return -1;

    cbreak();
    noecho();
    curs_set(0);          /* hide cursor */
    keypad(stdscr, TRUE); /* enable function & arrow keys */
    set_escdelay(25);     /* snappy ESC handling */

    if(has_colors())
        init_dos_colors();

    create_subwindows(tui);

    tui->running = 1;
    return 0;
}

void defrag_tui_shutdown(struct defrag_tui *tui)
{
    destroy_subwindows(tui);
    endwin();
}

/* ------------------------------------------------------------------ */
/*  Draw static chrome                                                 */
/* ------------------------------------------------------------------ */

void defrag_tui_draw_chrome(struct defrag_tui *tui)
{
    /* ---- Menu bar ---- */
    wbkgd(tui->win_menu, COLOR_PAIR(CP_MENU_BAR));
    werase(tui->win_menu);

    wattron(tui->win_menu, A_BOLD | COLOR_PAIR(CP_MENU_BAR));

    /* "File" menu label */
    wmove(tui->win_menu, 0, 1);
    wattron(tui->win_menu, COLOR_PAIR(CP_MENU_HOT));
    waddch(tui->win_menu, 'F');
    wattroff(tui->win_menu, COLOR_PAIR(CP_MENU_HOT));
    wattron(tui->win_menu, COLOR_PAIR(CP_MENU_BAR));
    waddstr(tui->win_menu, "ile");

    /* "Action" menu label */
    wmove(tui->win_menu, 0, 7);
    wattron(tui->win_menu, COLOR_PAIR(CP_MENU_HOT));
    waddch(tui->win_menu, 'A');
    wattroff(tui->win_menu, COLOR_PAIR(CP_MENU_HOT));
    wattron(tui->win_menu, COLOR_PAIR(CP_MENU_BAR));
    waddstr(tui->win_menu, "ction");

    /* "Help" menu label */
    wmove(tui->win_menu, 0, 15);
    wattron(tui->win_menu, COLOR_PAIR(CP_MENU_HOT));
    waddch(tui->win_menu, 'H');
    wattroff(tui->win_menu, COLOR_PAIR(CP_MENU_HOT));
    wattron(tui->win_menu, COLOR_PAIR(CP_MENU_BAR));
    waddstr(tui->win_menu, "elp");

    wattroff(tui->win_menu, A_BOLD);
    wnoutrefresh(tui->win_menu);

    /* ---- Map area (blue desktop) ---- */
    wbkgd(tui->win_map, COLOR_PAIR(CP_DESKTOP));
    werase(tui->win_map);
    defrag_map_draw(tui);
    wnoutrefresh(tui->win_map);

    /* ---- Status bar ---- */
    defrag_tui_update_status(tui);

    doupdate();
}

/* ------------------------------------------------------------------ */
/*  "Run fsck first" dialog                                            */
/* ------------------------------------------------------------------ */

int defrag_tui_fsck_dialog(struct defrag_tui *tui)
{
    const char *msg = "You should do an fsck before starting";
    int         msg_len  = (int)strlen(msg);
    int         dlg_w    = msg_len + 6;       /* 3-char padding each side */
    int         dlg_h    = 7;
    int         dlg_y    = (tui->rows - dlg_h) / 2;
    int         dlg_x    = (tui->cols - dlg_w) / 2;

    if(dlg_w > tui->cols) dlg_w = tui->cols;
    if(dlg_y < 0) dlg_y = 0;
    if(dlg_x < 0) dlg_x = 0;

    WINDOW *dlg = newwin(dlg_h, dlg_w, dlg_y, dlg_x);
    wbkgd(dlg, COLOR_PAIR(CP_DIALOG_BG));
    werase(dlg);

    /* Border (single-line box) */
    box(dlg, 0, 0);

    /* Title bar */
    wattron(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BG));
    mvwprintw(dlg, 0, (dlg_w - 9) / 2, " Warning ");
    wattroff(dlg, A_BOLD);

    /* Message text */
    wattron(dlg, COLOR_PAIR(CP_DIALOG_BG));
    mvwprintw(dlg, 2, (dlg_w - msg_len) / 2, "%s", msg);

    /* Button labels */
    const char *btn_ok   = "[ OK ]";
    const char *btn_exit = "[ Exit ]";
    int btn_ok_len   = (int)strlen(btn_ok);
    int btn_exit_len = (int)strlen(btn_exit);
    int btns_total   = btn_ok_len + 4 + btn_exit_len; /* 4 = gap */
    int btn_start    = (dlg_w - btns_total) / 2;
    int btn_row      = dlg_h - 2;

    int selected = 0; /* 0 = OK, 1 = Exit */

    keypad(dlg, TRUE);

    for(;;)
    {
        /* Draw OK button */
        if(selected == 0)
            wattron(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BTN));
        else
            wattron(dlg, COLOR_PAIR(CP_DIALOG_BG));
        mvwprintw(dlg, btn_row, btn_start, "%s", btn_ok);
        wattroff(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BTN));

        /* Draw Exit button */
        if(selected == 1)
            wattron(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BTN));
        else
            wattron(dlg, COLOR_PAIR(CP_DIALOG_BG));
        mvwprintw(dlg, btn_row, btn_start + btn_ok_len + 4, "%s", btn_exit);
        wattroff(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BTN));

        wrefresh(dlg);

        int ch = wgetch(dlg);
        switch(ch)
        {
            case KEY_LEFT:
            case '\t':
                selected = (selected + 1) % 2;
                break;
            case KEY_RIGHT:
                selected = (selected + 1) % 2;
                break;
            case '\n':
            case '\r':
            case KEY_ENTER:
            {
                delwin(dlg);
                /* Redraw underlying chrome */
                touchwin(tui->win_menu);
                touchwin(tui->win_map);
                touchwin(tui->win_status);
                wnoutrefresh(tui->win_menu);
                wnoutrefresh(tui->win_map);
                wnoutrefresh(tui->win_status);
                doupdate();
                return selected; /* 0 = OK, 1 = Exit */
            }
            case 27: /* ESC — treat as Exit */
                delwin(dlg);
                return 1;
            default:
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Analysis thread wrapper                                            */
/* ------------------------------------------------------------------ */

static void *analysis_thread_fn(void *arg)
{
    struct analysis_state *state = (struct analysis_state *)arg;
    defrag_analysis_run(state);
    return NULL;
}

int defrag_tui_start_analysis(struct defrag_tui *tui)
{
    if(atomic_load(&tui->analysis.finished) == 0 && tui->analysis_thread_started)
        return -1; /* already running */

    /* Reset state */
    if(tui->analysis.block_types)
    {
        free(tui->analysis.block_types);
        tui->analysis.block_types = NULL;
    }
    memset(&tui->analysis.result, 0, sizeof(tui->analysis.result));
    atomic_store(&tui->analysis.done_blocks, 0);
    atomic_store(&tui->analysis.phase, 0);
    atomic_store(&tui->analysis.finished, 0);
    atomic_store(&tui->analysis.error, 0);
    tui->analysis.ctx = tui->ctx;
    tui->analysis.total_blocks = tui->ctx->sb.total_bytes / tui->ctx->sb.block_size;

    int rc = pthread_create(&tui->analysis_thread, NULL, analysis_thread_fn, &tui->analysis);
    if(rc != 0) return -1;
    tui->analysis_thread_started = 1;
    tui->summary_shown = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Status bar update                                                  */
/* ------------------------------------------------------------------ */

void defrag_tui_update_status(struct defrag_tui *tui)
{
    werase(tui->win_status);
    wbkgd(tui->win_status, COLOR_PAIR(CP_STATUS_BAR));
    wattron(tui->win_status, COLOR_PAIR(CP_STATUS_BAR));

    if(tui->analysis_thread_started && !atomic_load(&tui->analysis.finished))
    {
        int phase = atomic_load(&tui->analysis.phase);
        uint64_t done = atomic_load(&tui->analysis.done_blocks);
        uint64_t total = tui->analysis.total_blocks;
        double pct = total > 0 ? ((double)done / (double)total) * 100.0 : 0.0;
        if(pct > 100.0) pct = 100.0;

        const char *phase_label = (phase >= 0 && phase < ANALYSIS_NUM_PHASES)
                                      ? analysis_phase_labels[phase]
                                      : "Working";

        /* Draw progress bar */
        int bar_start = 1;
        int label_len = (int)strlen(phase_label) + 16; /* "Phase: ... XX.X%" */
        int bar_width = tui->cols - label_len - 4;
        if(bar_width < 10) bar_width = 10;

        mvwprintw(tui->win_status, 0, bar_start, "%s  %5.1f%%  ", phase_label, pct);

        int fill = (int)(pct / 100.0 * bar_width);
        int pos;
        wmove(tui->win_status, 0, bar_start + label_len);
        waddch(tui->win_status, '[');
        wattron(tui->win_status, COLOR_PAIR(CP_PROGRESS));
        for(pos = 0; pos < fill && pos < bar_width; pos++)
            waddch(tui->win_status, ' ');
        wattroff(tui->win_status, COLOR_PAIR(CP_PROGRESS));
        wattron(tui->win_status, COLOR_PAIR(CP_STATUS_BAR));
        for(; pos < bar_width; pos++)
            waddch(tui->win_status, ACS_BULLET);
        waddch(tui->win_status, ']');
    }
    else if(tui->analysis_thread_started && atomic_load(&tui->analysis.finished))
    {
        mvwprintw(tui->win_status, 0, 1, "Analysis complete  |  Press S for summary  |  Q to quit");
    }
    else
    {
        mvwprintw(tui->win_status, 0, 1, "Ready  |  A = Analyse  |  Q = Quit");
    }

    wattroff(tui->win_status, COLOR_PAIR(CP_STATUS_BAR));
    wnoutrefresh(tui->win_status);
}

/* ------------------------------------------------------------------ */
/*  Summary dialog                                                     */
/* ------------------------------------------------------------------ */

void defrag_tui_summary_dialog(struct defrag_tui *tui)
{
    struct analysis_result *r = &tui->analysis.result;

    /* Calculate dialog dimensions */
    int content_lines = 8 + r->tree_count + 4; /* header + block stats + trees + dedup + buttons */
    int dlg_h = content_lines + 4; /* border + padding */
    int dlg_w = 60;
    if(dlg_h > tui->rows - 2) dlg_h = tui->rows - 2;
    if(dlg_w > tui->cols - 4) dlg_w = tui->cols - 4;

    int dlg_y = (tui->rows - dlg_h) / 2;
    int dlg_x = (tui->cols - dlg_w) / 2;
    if(dlg_y < 0) dlg_y = 0;
    if(dlg_x < 0) dlg_x = 0;

    WINDOW *dlg = newwin(dlg_h, dlg_w, dlg_y, dlg_x);
    wbkgd(dlg, COLOR_PAIR(CP_DIALOG_BG));
    werase(dlg);
    box(dlg, 0, 0);
    keypad(dlg, TRUE);

    /* Title */
    wattron(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BG));
    mvwprintw(dlg, 0, (dlg_w - 20) / 2, " Analysis Results ");
    wattroff(dlg, A_BOLD);

    int y = 2;
    int inner_w = dlg_w - 4;

    wattron(dlg, COLOR_PAIR(CP_DIALOG_BG));

    /* Block statistics */
    mvwprintw(dlg, y++, 2, "Total blocks:  %" PRIu64, r->total_blocks);
    mvwprintw(dlg, y++, 2, "Free blocks:   %" PRIu64 "  (%.1f%%)",
              r->free_blocks,
              r->total_blocks > 0 ? (double)r->free_blocks / (double)r->total_blocks * 100.0 : 0.0);
    mvwprintw(dlg, y++, 2, "Used blocks:   %" PRIu64 "  (%.1f%%)",
              r->used_blocks,
              r->total_blocks > 0 ? (double)r->used_blocks / (double)r->total_blocks * 100.0 : 0.0);
    mvwprintw(dlg, y++, 2, "Tree nodes:    %" PRIu64 "  (%.1f%%)",
              r->tree_blocks,
              r->total_blocks > 0 ? (double)r->tree_blocks / (double)r->total_blocks * 100.0 : 0.0);
    mvwprintw(dlg, y++, 2, "Dedup blocks:  %" PRIu64 "  (%.1f%%)",
              r->dedup_blocks,
              r->total_blocks > 0 ? (double)r->dedup_blocks / (double)r->total_blocks * 100.0 : 0.0);
    mvwprintw(dlg, y++, 2, "Metadata:      %" PRIu64, r->meta_blocks);
    y++;

    /* Free space fragmentation */
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, y++, 2, "Free space fragmentation: %.1f%%", r->free_frag_pct);
    wattroff(dlg, A_BOLD);
    y++;

    /* Per-tree fragmentation */
    if(r->tree_count > 0)
    {
        wattron(dlg, A_BOLD);
        mvwprintw(dlg, y++, 2, "Tree fragmentation:");
        wattroff(dlg, A_BOLD);

        for(int i = 0; i < r->tree_count && y < dlg_h - 3; i++)
        {
            struct tree_frag_stats *ts = &r->trees[i];
            mvwprintw(dlg, y++, 4, "%-16s %5.1f%%  (%" PRIu64 " nodes)",
                      ts->name, ts->frag_pct, ts->node_count);
        }
        y++;
    }

    /* Dedup data fragmentation */
    if(r->dedup_blocks > 0 && y < dlg_h - 2)
    {
        wattron(dlg, A_BOLD);
        mvwprintw(dlg, y++, 2, "Dedup data fragmentation: %.1f%%", r->dedup_frag_pct);
        wattroff(dlg, A_BOLD);
    }

    /* OK button */
    int btn_row = dlg_h - 2;
    const char *btn = "[ OK ]";
    int btn_x = (dlg_w - (int)strlen(btn)) / 2;
    wattron(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BTN));
    mvwprintw(dlg, btn_row, btn_x, "%s", btn);
    wattroff(dlg, A_BOLD | COLOR_PAIR(CP_DIALOG_BTN));

    wrefresh(dlg);

    /* Wait for any key to dismiss */
    for(;;)
    {
        int ch = wgetch(dlg);
        if(ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 27 || ch == 'q' || ch == 'Q')
            break;
    }

    delwin(dlg);

    /* Redraw underlying chrome */
    touchwin(tui->win_menu);
    touchwin(tui->win_map);
    touchwin(tui->win_status);
    wnoutrefresh(tui->win_menu);
    wnoutrefresh(tui->win_map);
    wnoutrefresh(tui->win_status);
    doupdate();
}

/* ------------------------------------------------------------------ */
/*  Main event loop                                                    */
/* ------------------------------------------------------------------ */

void defrag_tui_run(struct defrag_tui *tui)
{
    /* Use a short timeout so we can refresh during analysis */
    wtimeout(tui->win_map, 150);

    while(tui->running)
    {
        int ch = wgetch(tui->win_map);

        switch(ch)
        {
            case 'q':
            case 'Q':
                /* If analysis is running, wait for it to finish */
                if(tui->analysis_thread_started && !atomic_load(&tui->analysis.finished))
                {
                    /* Signal abort — for now just wait */
                    pthread_join(tui->analysis_thread, NULL);
                    tui->analysis_thread_started = 0;
                }
                tui->running = 0;
                break;

            case 'a':
            case 'A':
                if(!tui->analysis_thread_started || atomic_load(&tui->analysis.finished))
                {
                    if(tui->analysis_thread_started)
                    {
                        pthread_join(tui->analysis_thread, NULL);
                        tui->analysis_thread_started = 0;
                    }
                    defrag_tui_start_analysis(tui);
                }
                break;

            case 's':
            case 'S':
                if(tui->analysis_thread_started && atomic_load(&tui->analysis.finished))
                    defrag_tui_summary_dialog(tui);
                break;

            case KEY_RESIZE:
                /* Terminal was resized — recreate sub-windows. */
                destroy_subwindows(tui);
                create_subwindows(tui);
                defrag_tui_draw_chrome(tui);
                break;

            case ERR:
                /* Timeout — fall through to refresh */
                break;

            default:
                break;
        }

        /* Live refresh during/after analysis */
        if(tui->analysis_thread_started)
        {
            /* Check if analysis just finished */
            int finished = atomic_load(&tui->analysis.finished);

            /* Redraw map */
            werase(tui->win_map);
            wbkgd(tui->win_map, COLOR_PAIR(CP_DESKTOP));
            defrag_map_draw(tui);
            wnoutrefresh(tui->win_map);

            /* Redraw status */
            defrag_tui_update_status(tui);

            doupdate();

            /* Auto-show summary once when analysis completes */
            if(finished && !atomic_load(&tui->analysis.error) && !tui->summary_shown)
            {
                tui->summary_shown = 1;

                /* Join the thread first */
                pthread_join(tui->analysis_thread, NULL);
                /* Keep analysis_thread_started = 1 so we know results are available */

                defrag_tui_summary_dialog(tui);
            }
        }
    }
}
