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
    wbkgd(tui->win_status, COLOR_PAIR(CP_STATUS_BAR));
    werase(tui->win_status);

    wattron(tui->win_status, COLOR_PAIR(CP_STATUS_BAR));
    mvwprintw(tui->win_status, 0, 1, "Ready  |  0%%");
    wattroff(tui->win_status, COLOR_PAIR(CP_STATUS_BAR));
    wnoutrefresh(tui->win_status);

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
/*  Main event loop                                                    */
/* ------------------------------------------------------------------ */

void defrag_tui_run(struct defrag_tui *tui)
{
    while(tui->running)
    {
        int ch = wgetch(tui->win_map);

        switch(ch)
        {
            case 'q':
            case 'Q':
                tui->running = 0;
                break;

            case KEY_RESIZE:
                /* Terminal was resized — recreate sub-windows. */
                destroy_subwindows(tui);
                create_subwindows(tui);
                defrag_tui_draw_chrome(tui);
                break;

            default:
                break;
        }
    }
}
