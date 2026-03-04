// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 defragmenter TUI (defrag)
//
// --[ Description ] ----------------------------------------------------------
//
//     Entry point for the OBMAFS3 defragmenter.  Parses command-line
//     arguments, opens the filesystem, and launches the TUI.
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

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Usage / help                                                       */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <device-or-image>\n"
            "\n"
            "OBMAFS3 filesystem defragmenter (TUI).\n"
            "\n"
            "Options:\n"
            "  -h, --help    Show this help message and exit\n"
            "\n",
            prog);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {  NULL,           0, NULL,   0}
    };

    int opt;
    while((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1)
    {
        switch(opt)
        {
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if(optind >= argc)
    {
        fprintf(stderr, "Error: no device or image path specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *device_path = argv[optind];

    /* Enable UTF-8 wide-character output */
    setlocale(LC_ALL, "");

    /* ---- Open the device/image for offline, exclusive access ----
     * No node cache, keyset, DLC, warmup or housekeeping threads.
     * Just a raw fd + minimal obmafs3_ctx for block I/O helpers. */
    int fd = open(device_path, O_RDONLY);
    if(fd < 0)
    {
        fprintf(stderr, "Error: cannot open '%s': %s\n", device_path, strerror(errno));
        return 1;
    }

    /* Read and validate the superblock */
    struct obmafs3_sb sb;
    ssize_t rd = pread(fd, &sb, sizeof(sb), 0);
    if(rd < (ssize_t)sizeof(sb))
    {
        fprintf(stderr, "Error: cannot read superblock from '%s'\n", device_path);
        close(fd);
        return 1;
    }

    if(sb.magic != OBMAFS3_SB_MAGIC)
    {
        fprintf(stderr, "Error: '%s' does not contain a valid OBMAFS3 filesystem\n", device_path);
        close(fd);
        return 1;
    }

    /* Build a minimal ctx — no caches, no threads, no housekeeping */
    struct obmafs3_ctx *ctx = calloc(1, sizeof(*ctx));
    if(!ctx)
    {
        fprintf(stderr, "Error: out of memory\n");
        close(fd);
        return 1;
    }
    ctx->fd          = fd;
    ctx->sb          = sb;
    ctx->compression = 1;
    ctx->zstd_level  = 15;

    if(pthread_key_create(&ctx->tls_key, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_key_create failed\n");
        close(fd);
        free(ctx);
        return 1;
    }
    pthread_rwlock_init(&ctx->tree_lock, NULL);

    /* Allocate thread-local scratch buffers needed by library I/O helpers */
    struct obmafs3_thread_bufs *tb = obmafs3_get_thread_bufs(ctx);
    if(!tb || !tb->hdr_buf || !tb->node_buf || !tb->io_buf)
    {
        fprintf(stderr, "Error: out of memory allocating work buffers\n");
        close(fd);
        free(ctx);
        return 1;
    }

    /* ---- Launch TUI ---- */
    struct defrag_tui tui;

    if(defrag_tui_init(&tui) != 0)
    {
        fprintf(stderr, "Error: failed to initialise terminal UI.\n");
        close(fd);
        free(ctx);
        return 1;
    }

    tui.ctx = ctx;

    /* Draw the full screen chrome first, then show the dialog on top. */
    defrag_tui_draw_chrome(&tui);

    int dialog_result = defrag_tui_fsck_dialog(&tui);
    if(dialog_result == 1)
    {
        /* User chose "Exit" */
        defrag_tui_shutdown(&tui);
        close(fd);
        free(ctx);
        return 0;
    }

    /* User chose "OK" — enter normal event loop. */
    defrag_tui_run(&tui);

    if(tui.analysis.block_types)
        free(tui.analysis.block_types);

    defrag_tui_shutdown(&tui);
    close(fd);
    free(ctx);
    return 0;
}
