/*
 * mkobmafs - Create a new OBMAFS3 filesystem
 */
#include "obmafs.h"

#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <device-or-file>\n"
        "\n"
        "Options:\n"
        "  -s, --size <bytes>         Total filesystem size (default: file size, or 1 GiB)\n"
        "  -b, --block-size <bytes>   Block size (default: 4096)\n"
        "  -d, --dedup-size <bytes>   Dedup block size (default: 4096)\n"
        "  -l, --label <name>         Volume label (default: OBMAFS3)\n"
        "  -h, --help                 Show this help\n",
        prog);
}

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"size",       required_argument, NULL, 's'},
        {"block-size", required_argument, NULL, 'b'},
        {"dedup-size", required_argument, NULL, 'd'},
        {"label",      required_argument, NULL, 'l'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    uint64_t total_size      = 0;
    uint64_t block_size      = OBMAFS3_DEFAULT_BLOCK_SIZE;
    uint64_t dedup_block_size = OBMAFS3_DEFAULT_DEDUP_BLOCK_SIZE;
    const char *label        = "OBMAFS3";
    const char *path;
    int opt;

    while ((opt = getopt_long(argc, argv, "s:b:d:l:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': total_size      = strtoull(optarg, NULL, 0); break;
        case 'b': block_size      = strtoull(optarg, NULL, 0); break;
        case 'd': dedup_block_size = strtoull(optarg, NULL, 0); break;
        case 'l': label           = optarg;                    break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: no device or file specified\n");
        usage(argv[0]);
        return 1;
    }

    path = argv[optind];

    /* Determine size from existing file if not specified */
    if (total_size == 0) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            off_t end = lseek(fd, 0, SEEK_END);
            if (end > 0)
                total_size = (uint64_t)end;
            close(fd);
        }
        if (total_size == 0)
            total_size = 1ULL * 1024 * 1024 * 1024;  /* 1 GiB */
    }

    int rc = obmafs3_create(path, total_size, block_size,
                            dedup_block_size, label);
    if (rc != OBMAFS3_OK) {
        fprintf(stderr, "Error: failed to create filesystem (rc=%d)\n", rc);
        return 1;
    }

    printf("Created OBMAFS3 filesystem on %s\n", path);
    printf("  Total size:       %" PRIu64 " bytes\n", total_size);
    printf("  Block size:       %" PRIu64 " bytes\n", block_size);
    printf("  Dedup block size: %" PRIu64 " bytes\n", dedup_block_size);
    printf("  Label:            %s\n", label);

    return 0;
}
