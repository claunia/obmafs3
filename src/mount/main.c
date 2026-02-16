/*
 * mount.obmafs - FUSE mount helper for OBMAFS3 filesystems
 */
#define FUSE_USE_VERSION 31

#include "fuse_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct obmafs3_options {
    const char *device;
    int show_help;
};

#define OPTION(t, p) { t, offsetof(struct obmafs3_options, p), 1 }

static const struct fuse_opt option_spec[] = {
    OPTION("--device=%s", device),
    OPTION("-h",          show_help),
    OPTION("--help",      show_help),
    FUSE_OPT_END
};

static void show_help(const char *progname)
{
    printf("Usage: %s --device=<path> <mountpoint> [FUSE options]\n\n"
           "OBMAFS3 options:\n"
           "    --device=<path>    Path to the OBMAFS3 filesystem image\n"
           "\n", progname);
}

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct obmafs3_options opts;
    int rc;

    memset(&opts, 0, sizeof(opts));

    if (fuse_opt_parse(&args, &opts, option_spec, NULL) == -1)
        return 1;

    if (opts.show_help) {
        show_help(argv[0]);
        fuse_opt_add_arg(&args, "--help");
        args.argv[0][0] = '\0';
    }

    if (!opts.device && !opts.show_help) {
        fprintf(stderr, "Error: --device option is required\n");
        show_help(argv[0]);
        return 1;
    }

    if (opts.device) {
        rc = obmafs3_open(opts.device, &g_ctx);
        if (rc != OBMAFS3_OK) {
            fprintf(stderr, "Error: failed to open %s (rc=%d)\n",
                    opts.device, rc);
            return 1;
        }
    }

    rc = fuse_main(args.argc, args.argv, &obmafs3_fuse_ops, NULL);

    if (g_ctx)
        obmafs3_close(g_ctx);
    fuse_opt_free_args(&args);
    return rc;
}
