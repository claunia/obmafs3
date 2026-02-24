// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : FUSE interface for OBMAFS3
//
// --[ Description ] ----------------------------------------------------------
//
//     FUSE mount helper for OBMAFS3 filesystems.
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

#include "debug.h"
#include "fuse_ops.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct obmafs3_options
{
    const char *device;
    const char *disk_images;
    int         show_help;
    int         compression; /* -1 = not set (use default) */
    int         zstd_level;  /* -1 = not set (use default) */
};

#define OPTION(t, p) {t, offsetof(struct obmafs3_options, p), 1}

static const struct fuse_opt option_spec[] = {
    OPTION("--device=%s", device),
    OPTION("--disk-images=%s", disk_images),
    OPTION("-h", show_help),
    OPTION("--help", show_help),
    {"--compression=%d", offsetof(struct obmafs3_options, compression), 0},
    { "--zstd-level=%d", offsetof(struct obmafs3_options,  zstd_level), 0},
    FUSE_OPT_END
};

/**
 * Print usage information for the FUSE mount helper.
 *
 * @param progname  Program name to display in the usage line.
 */
static void show_help(const char *progname)
{
    printf("Usage: %s --device=<path> <mountpoint> [FUSE options]\n\n"
           "OBMAFS3 options:\n"
           "    --device=<path>        Path to the OBMAFS3 filesystem image\n"
           "    --compression=<0|1>    Enable (1) or disable (0) compression (default: 1)\n"
           "    --zstd-level=<1-15>    ZSTD compression level (default: 15)\n"
           "    --disk-images=<spec>   Semicolon-separated ext=sector_size pairs\n"
           "                           (default: dsk=512;iso=2048;img=512;IMA=512;adf=512;xdf=512;usb=512)\n"
           "\n",
           progname);
}

/**
 * Entry point for the OBMAFS3 FUSE mount helper.
 *
 * Parses command-line options, opens the filesystem image, applies
 * mount-time settings (compression, ZSTD level, disk-image extension
 * mappings), and enters the FUSE main loop.
 */
int main(int argc, char *argv[])
{
    obmafs3_debug_init(); /* Enable debug output if OBMAFS3_DEBUG is set */

    struct fuse_args       args = FUSE_ARGS_INIT(argc, argv);
    struct obmafs3_options opts;
    int                    rc;

    memset(&opts, 0, sizeof(opts));
    opts.compression = -1; /* sentinel: use default */
    opts.zstd_level  = -1; /* sentinel: use default */

    if(fuse_opt_parse(&args, &opts, option_spec, NULL) == -1) return 1;

    if(opts.show_help)
    {
        show_help(argv[0]);
        fuse_opt_add_arg(&args, "--help");
        args.argv[0][0] = '\0';
    }

    if(!opts.device && !opts.show_help)
    {
        fprintf(stderr, "Error: --device option is required\n");
        show_help(argv[0]);
        return 1;
    }

    if(opts.device)
    {
        rc = obmafs3_open(opts.device, &g_ctx);
        if(rc != OBMAFS3_OK)
        {
            fprintf(stderr, "Error: failed to open %s (rc=%d)\n", opts.device, rc);
            return 1;
        }

        /* Force read-only mount when the library flagged it (unknown rocompat flags) */
        if(g_ctx->read_only)
        {
            fprintf(stderr, "Mounting %s read-only due to unknown read-only compatible feature flags\n",
                    opts.device);
            fuse_opt_add_arg(&args, "-o");
            fuse_opt_add_arg(&args, "ro");
        }

        /* Apply mount options */
        if(opts.compression != -1) g_ctx->compression = (opts.compression != 0);
        if(opts.zstd_level != -1)
        {
            if(opts.zstd_level < 1 || opts.zstd_level > 15)
            {
                fprintf(stderr, "Error: --zstd-level must be between 1 and 15\n");
                obmafs3_close(g_ctx);
                g_ctx = NULL;
                return 1;
            }
            g_ctx->zstd_level = opts.zstd_level;
        }
    }

    /* Parse disk image extension mappings */
    {
        const char *spec =
            opts.disk_images ? opts.disk_images : "dsk=512;iso=2048;img=512;IMA=512;adf=512;xdf=512;usb=512";
        if(parse_disk_image_maps(spec) != 0)
        {
            fprintf(stderr, "Error: invalid --disk-images specification\n");
            if(g_ctx)
            {
                obmafs3_close(g_ctx);
                g_ctx = NULL;
            }
            return 1;
        }
    }

    /* Record PID before fuse_main so reinit can detect whether a fork
     * happened (daemonisation) vs. foreground mode (-f). */
    if(g_ctx) g_ctx->pre_fuse_pid = getpid();

    rc = fuse_main(args.argc, args.argv, &obmafs3_fuse_ops, NULL);

    if(g_ctx)
    {
        /* fuse_main restores the default signal handlers before returning,
         * so a stray ^C during obmafs3_close would kill the process mid-save.
         * Ignore SIGINT/SIGTERM/SIGHUP to let the close path complete. */
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        fprintf(stderr, "[obmafs3] shutting down — persisting keyset and metadata...\n");
        fflush(stderr);
        fprintf(stderr, "[obmafs3] calling obmafs3_close...\n");
        fflush(stderr);
        obmafs3_close(g_ctx);
        fprintf(stderr, "[obmafs3] shutdown complete.\n");
    }
    fuse_opt_free_args(&args);
    return rc;
}
