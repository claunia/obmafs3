// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : ui.h
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

#ifndef IMPORT_AIF_UI_H
#define IMPORT_AIF_UI_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

/* ---- Globals ---- */
extern int g_color; /* 1 when stderr is a tty */

/* ---- ANSI colour helpers ---- */
#define C_RESET     (g_color ? "\033[0m" : "")
#define C_BOLD      (g_color ? "\033[1m" : "")
#define C_DIM       (g_color ? "\033[2m" : "")
#define C_RED       (g_color ? "\033[31m" : "")
#define C_GREEN     (g_color ? "\033[32m" : "")
#define C_YELLOW    (g_color ? "\033[33m" : "")
#define C_BLUE      (g_color ? "\033[34m" : "")
#define C_MAGENTA   (g_color ? "\033[35m" : "")
#define C_CYAN      (g_color ? "\033[36m" : "")
#define C_BOLD_RED  (g_color ? "\033[1;31m" : "")
#define C_BOLD_GRN  (g_color ? "\033[1;32m" : "")
#define C_BOLD_YLW  (g_color ? "\033[1;33m" : "")
#define C_BOLD_CYAN (g_color ? "\033[1;36m" : "")

/* ---- Unicode symbols with colour ---- */
#define SYM_OK    (g_color ? "\033[32m\u2714\033[0m" : "[OK]")
#define SYM_FAIL  (g_color ? "\033[31m\u2718\033[0m" : "[FAIL]")
#define SYM_WARN  (g_color ? "\033[33m\u26A0\033[0m" : "[WARN]")
#define SYM_INFO  (g_color ? "\033[36m\u2022\033[0m" : "*")
#define SYM_ARROW (g_color ? "\033[36m\u2192\033[0m" : "->")
#define SYM_DISC  (g_color ? "\033[35m\u25C9\033[0m" : "(o)")
#define SYM_FILE  (g_color ? "\033[34m\u2637\033[0m" : "[F]")
#define SYM_MUSIC (g_color ? "\033[35m\u266B\033[0m" : "[A]")
#define SYM_DATA  (g_color ? "\033[36m\u25A0\033[0m" : "[D]")

/* ---- Functions ---- */
void ui_init(void);
void ui_banner(void);
void ui_phase(int num, const char *label);
void ui_step(const char *label);
void ui_info(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_progress(const char *label, uint64_t done, uint64_t total);
void ui_progress_clear(void);
void ui_track_info(int seq, int64_t start, int64_t end, int64_t pregap, const char *mode, uint16_t ss);
void ui_summary(const char *src, const char *dst, double elapsed_s, uint64_t sectors, uint32_t sector_size);

#endif /* IMPORT_AIF_UI_H */
