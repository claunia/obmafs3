/*
 * obmafs-query - Interactive metadata query tool for mounted OBMAFS3
 *                filesystems.
 *
 * Communicates with a live OBMAFS3 FUSE mount entirely through ioctls.
 * Does not link against libobmafs.
 *
 * Usage: obmafs-query <mountpoint>
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "obmafs3_ioctl.h"

/* FUSE_SUPER_MAGIC as reported by statfs(2) on Linux */
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif

/* Filesystem identity xattr on the root directory */
#define FSTYPE_XATTR_NAME  "system.obmafs3.fstype"
#define FSTYPE_XATTR_VALUE "obmafs3"

#define PROMPT "> "

/* ------------------------------------------------------------------ */
/*  Mount-point validation                                             */
/* ------------------------------------------------------------------ */

/**
 * Verify that @p path is a mounted OBMAFS3 filesystem.
 *
 * Checks:
 * 1. Path is a directory on a FUSE filesystem (statfs f_type).
 * 2. Root directory exposes the system.obmafs3.fstype xattr with
 *    the expected value.
 *
 * @return 0 on success, -1 on failure (message printed to stderr).
 */
static int validate_mountpoint(const char *path)
{
    struct stat st;

    if(stat(path, &st) != 0)
    {
        fprintf(stderr, "Error: cannot stat '%s': %s\n", path, strerror(errno));
        return -1;
    }

    if(!S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "Error: '%s' is not a directory\n", path);
        return -1;
    }

    struct statfs sfs;
    if(statfs(path, &sfs) != 0)
    {
        fprintf(stderr, "Error: statfs('%s') failed: %s\n", path, strerror(errno));
        return -1;
    }

    if(sfs.f_type != FUSE_SUPER_MAGIC)
    {
        fprintf(stderr, "Error: '%s' is not a FUSE filesystem (f_type=0x%lx)\n", path, (unsigned long)sfs.f_type);
        return -1;
    }

    /* Probe for the OBMAFS3 filesystem identity xattr on the root */
    char xattr_buf[32] = {0};
    ssize_t xlen = getxattr(path, FSTYPE_XATTR_NAME, xattr_buf, sizeof(xattr_buf) - 1);
    if(xlen < 0)
    {
        fprintf(stderr, "Error: '%s' is not an OBMAFS3 filesystem (missing %s xattr)\n", path, FSTYPE_XATTR_NAME);
        return -1;
    }
    xattr_buf[xlen] = '\0';
    if(strcmp(xattr_buf, FSTYPE_XATTR_VALUE) != 0)
    {
        fprintf(stderr, "Error: '%s' is not an OBMAFS3 filesystem (unexpected fstype: %s)\n", path, xattr_buf);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Help text                                                          */
/* ------------------------------------------------------------------ */

static void print_help(void)
{
    printf("obmafs-query — Interactive metadata query tool for OBMAFS3\n"
           "\n"
           "Commands:\n"
           "  help               Show this help message\n"
           "  quit               Exit the query tool\n"
           "\n"
           "Query syntax (not yet implemented):\n"
           "  <key> = \"<value>\"                  Exact match\n"
           "  <key> != \"<value>\"                 Not equal\n"
           "  <key> > \"<value>\"                  Greater than (lexicographic)\n"
           "  <key> < \"<value>\"                  Less than (lexicographic)\n"
           "  <key> >= \"<value>\"                 Greater or equal\n"
           "  <key> <= \"<value>\"                 Less or equal\n"
           "  <key> CONTAINS \"<value>\"           Substring match\n"
           "  <key> STARTSWITH \"<value>\"         Prefix match\n"
           "  <key> EXISTS                       Key exists (any value)\n"
           "\n"
           "  Multiple conditions joined with AND or OR:\n"
           "    artist = \"Iron Maiden\" AND year > \"1985\"\n"
           "\n");
}

/* ------------------------------------------------------------------ */
/*  Usage                                                              */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <mountpoint>\n", prog);
}

/* ------------------------------------------------------------------ */
/*  Read-eval-print loop                                               */
/* ------------------------------------------------------------------ */

static void repl(const char *mountpoint)
{
    char line[4096];

    printf("obmafs-query: connected to %s\n", mountpoint);
    printf("Type 'help' for available commands, 'quit' to exit.\n\n");

    while(1)
    {
        printf(PROMPT);
        fflush(stdout);

        if(!fgets(line, sizeof(line), stdin))
        {
            /* EOF (Ctrl-D) — treat as quit */
            printf("\n");
            break;
        }

        /* Strip trailing newline / carriage return */
        size_t len = strlen(line);
        while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        /* Skip empty lines */
        if(len == 0) continue;

        /* Skip leading whitespace for command matching */
        const char *cmd = line;
        while(*cmd == ' ' || *cmd == '\t') cmd++;

        if(strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0)
            break;

        if(strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
        {
            print_help();
            continue;
        }

        /* TODO: parse and execute queries */
        fprintf(stderr, "Unknown command: %s\n", cmd);
        fprintf(stderr, "Type 'help' for available commands.\n");
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if(argc != 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        usage(argv[0]);
        return argc != 2 ? 1 : 0;
    }

    const char *mountpoint = argv[1];

    if(validate_mountpoint(mountpoint) != 0) return 1;

    repl(mountpoint);

    return 0;
}
