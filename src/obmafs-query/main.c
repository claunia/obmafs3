// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 query tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Interactive metadata query tool for mounted OBMAFS3 filesystems.
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

/*
 * Communicates with a live OBMAFS3 FUSE mount entirely through ioctls.
 * Does not link against libobmafs.
 *
 * Usage: obmafs-query <mountpoint>
 *
 * Query syntax:
 *   <key> = "<value>"               Exact match
 *   <key> != "<value>"              Not equal
 *   <key> > "<value>"               Greater than (lexicographic)
 *   <key> < "<value>"               Less than
 *   <key> >= "<value>"              Greater or equal
 *   <key> <= "<value>"              Less or equal
 *   <key> CONTAINS "<value>"        Substring match
 *   <key> STARTSWITH "<value>"      Prefix match
 *   <key> EXISTS                    Key exists (any value)
 *
 *   Multiple conditions joined with AND or OR:
 *     artist = "Iron Maiden" AND year > "1985"
 */

#include <ctype.h>
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

/* Temp file used to obtain a FUSE file handle for ioctls */
#define QUERY_SENTINEL ".obmafs3_query_tmp"

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
    char    xattr_buf[32] = {0};
    ssize_t xlen          = getxattr(path, FSTYPE_XATTR_NAME, xattr_buf, sizeof(xattr_buf) - 1);
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
/*  Sentinel file management                                           */
/* ------------------------------------------------------------------ */

/**
 * Open a temporary sentinel file on the mount to obtain a FUSE file
 * descriptor suitable for ioctl calls.  The file is created, opened,
 * and immediately unlinked so it disappears after close.
 *
 * @param mountpoint  Path to the OBMAFS3 mount.
 * @return Open file descriptor (>= 0), or -1 on error.
 */
static int open_sentinel(const char *mountpoint)
{
    char sentinel_path[4096];
    snprintf(sentinel_path, sizeof(sentinel_path), "%s/%s", mountpoint, QUERY_SENTINEL);

    int fd = open(sentinel_path, O_CREAT | O_RDWR, 0600);
    if(fd < 0)
    {
        fprintf(stderr, "Error: cannot create sentinel file '%s': %s\n", sentinel_path, strerror(errno));
        return -1;
    }

    /* Unlink immediately — fd stays valid until close */
    unlink(sentinel_path);
    return fd;
}

/* ------------------------------------------------------------------ */
/*  Query parser                                                       */
/* ------------------------------------------------------------------ */

/** Skip whitespace, return pointer to next non-space character. */
static const char *skip_ws(const char *p)
{
    while(*p == ' ' || *p == '\t') p++;
    return p;
}

/**
 * Parse an unquoted token (key name or keyword like AND/OR/EXISTS/
 * CONTAINS/STARTSWITH).  Stops at whitespace, quote, or operator
 * characters.
 *
 * @param p     Input pointer (must point to start of token).
 * @param out   Output buffer.
 * @param outsz Size of output buffer.
 * @return Pointer past the parsed token, or NULL on error.
 */
static const char *parse_token(const char *p, char *out, size_t outsz)
{
    size_t i = 0;
    while(*p && *p != ' ' && *p != '\t' && *p != '"' && *p != '=' && *p != '!' && *p != '<' && *p != '>' && *p != '*')
    {
        if(i < outsz - 1) out[i++] = *p;
        p++;
    }

    /* Handle '*' (wildcard key for any-key queries) */
    if(*p == '*' && i == 0)
    {
        out[i++] = '*';
        p++;
    }
    out[i] = '\0';
    return i > 0 ? p : NULL;
}

/**
 * Parse a quoted string value: "..." with backslash escaping for
 * \" and \\.
 *
 * @param p     Input pointer (must point to the opening quote).
 * @param out   Output buffer.
 * @param outsz Size of output buffer.
 * @return Pointer past the closing quote, or NULL on error.
 */
static const char *parse_quoted(const char *p, char *out, size_t outsz)
{
    if(*p != '"') return NULL;
    p++; /* skip opening quote */

    size_t i = 0;
    while(*p && *p != '"')
    {
        if(*p == '\\' && (p[1] == '"' || p[1] == '\\'))
        {
            if(i < outsz - 1) out[i++] = p[1];
            p += 2;
        }
        else
        {
            if(i < outsz - 1) out[i++] = *p;
            p++;
        }
    }
    out[i] = '\0';

    if(*p != '"')
    {
        fprintf(stderr, "Error: unterminated quoted string\n");
        return NULL;
    }
    return p + 1; /* skip closing quote */
}

/**
 * Parse the operator after the key.  Accepts:
 *   =  !=  >  <  >=  <=  CONTAINS  STARTSWITH  EXISTS
 *
 * @param p     Input pointer (after key + whitespace).
 * @param op    Output operator value.
 * @param need_value  Output: 1 if a value operand is expected, 0 for EXISTS.
 * @return Pointer past the operator, or NULL on error.
 */
static const char *parse_operator(const char *p, uint8_t *op, int *need_value)
{
    *need_value = 1;

    /* Two-character operators first */
    if(p[0] == '!' && p[1] == '=') { *op = kQueryOpNotEqual; return p + 2; }
    if(p[0] == '>' && p[1] == '=') { *op = kQueryOpGreaterEq; return p + 2; }
    if(p[0] == '<' && p[1] == '=') { *op = kQueryOpLessEq; return p + 2; }

    /* Single-character operators */
    if(p[0] == '=') { *op = kQueryOpEqual; return p + 1; }
    if(p[0] == '>') { *op = kQueryOpGreater; return p + 1; }
    if(p[0] == '<') { *op = kQueryOpLess; return p + 1; }

    /* Keyword operators */
    char kw[32];
    const char *after = parse_token(p, kw, sizeof(kw));
    if(!after)
    {
        fprintf(stderr, "Error: expected operator after key\n");
        return NULL;
    }

    if(strcasecmp(kw, "CONTAINS") == 0)   { *op = kQueryOpContains; return after; }
    if(strcasecmp(kw, "STARTSWITH") == 0)  { *op = kQueryOpStartsWith; return after; }
    if(strcasecmp(kw, "EXISTS") == 0)      { *op = kQueryOpExists; *need_value = 0; return after; }

    fprintf(stderr, "Error: unknown operator '%s'\n", kw);
    return NULL;
}

/**
 * Parse one filter condition: <key> <op> ["<value>"]
 *
 * @param p    Input pointer.
 * @param flt  Output filter.
 * @return Pointer past the parsed filter, or NULL on error.
 */
static const char *parse_one_filter(const char *p, struct obmafs3_ioctl_query_filter *flt)
{
    memset(flt, 0, sizeof(*flt));

    p = skip_ws(p);
    if(!*p)
    {
        fprintf(stderr, "Error: expected a filter condition\n");
        return NULL;
    }

    /* Parse key */
    p = parse_token(p, flt->key, sizeof(flt->key));
    if(!p || flt->key[0] == '\0')
    {
        fprintf(stderr, "Error: expected a key name\n");
        return NULL;
    }

    p = skip_ws(p);

    /* Parse operator */
    int need_value;
    p = parse_operator(p, &flt->op, &need_value);
    if(!p) return NULL;

    if(!need_value) return p;  /* EXISTS — no value */

    p = skip_ws(p);

    /* Parse value — quoted or unquoted */
    if(*p == '"')
    {
        p = parse_quoted(p, flt->value, sizeof(flt->value));
    }
    else
    {
        /* Allow unquoted single-word values */
        p = parse_token(p, flt->value, sizeof(flt->value));
    }

    if(!p)
    {
        fprintf(stderr, "Error: expected a value after operator\n");
        return NULL;
    }

    return p;
}

/**
 * Parse a full query line into an ioctl metadata query argument.
 *
 * Grammar:
 *   query     := filter (combiner filter)*
 *   combiner  := AND | OR
 *   filter    := key operator [value]
 *
 * All combiners in a single query must be the same kind (all AND or all OR).
 *
 * @param input  The query string.
 * @param qa     Output ioctl argument (filters, combine, filter_count filled).
 * @return 0 on success, -1 on error (message printed to stderr).
 */
static int parse_query(const char *input, struct obmafs3_ioctl_metadata_query_arg *qa)
{
    memset(qa, 0, sizeof(*qa));

    const char *p = input;
    uint8_t     nf = 0;
    int         combine_set = 0;

    while(1)
    {
        if(nf >= OBMAFS3_QUERY_MAX_FILTERS)
        {
            fprintf(stderr, "Error: too many filters (max %d)\n", OBMAFS3_QUERY_MAX_FILTERS);
            return -1;
        }

        p = parse_one_filter(p, &qa->filters[nf]);
        if(!p) return -1;
        nf++;

        p = skip_ws(p);
        if(!*p) break;  /* end of input */

        /* Expect AND or OR */
        char kw[8];
        const char *after = parse_token(p, kw, sizeof(kw));
        if(!after)
        {
            fprintf(stderr, "Error: expected AND or OR between filters\n");
            return -1;
        }

        uint8_t this_combine;
        if(strcasecmp(kw, "AND") == 0)
            this_combine = kQueryCombineAnd;
        else if(strcasecmp(kw, "OR") == 0)
            this_combine = kQueryCombineOr;
        else
        {
            fprintf(stderr, "Error: expected AND or OR, got '%s'\n", kw);
            return -1;
        }

        if(combine_set && qa->combine != this_combine)
        {
            fprintf(stderr, "Error: cannot mix AND and OR in a single query\n");
            return -1;
        }
        qa->combine = this_combine;
        combine_set = 1;

        p = after;
    }

    qa->filter_count = nf;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Result set                                                         */
/* ------------------------------------------------------------------ */

/** Growable array of path strings collected from paginated queries. */
struct result_set
{
    char   **paths;  /**< Heap-allocated array of strdup'd paths */
    uint32_t count;  /**< Number of entries */
    uint32_t cap;    /**< Allocated capacity */
};

static void rs_init(struct result_set *rs)
{
    rs->paths = NULL;
    rs->count = 0;
    rs->cap   = 0;
}

static void rs_free(struct result_set *rs)
{
    for(uint32_t i = 0; i < rs->count; i++) free(rs->paths[i]);
    free(rs->paths);
    rs_init(rs);
}

static int rs_add(struct result_set *rs, const char *path)
{
    if(rs->count == rs->cap)
    {
        uint32_t newcap = rs->cap ? rs->cap * 2 : 64;
        char   **tmp    = realloc(rs->paths, newcap * sizeof(char *));
        if(!tmp) { fprintf(stderr, "Error: out of memory\n"); return -1; }
        rs->paths = tmp;
        rs->cap   = newcap;
    }
    rs->paths[rs->count] = strdup(path);
    if(!rs->paths[rs->count]) { fprintf(stderr, "Error: out of memory\n"); return -1; }
    rs->count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Export helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * Escape a string for JSON output.  Writes the escaped content
 * (without surrounding quotes) to @p fp.
 */
static void json_escape(FILE *fp, const char *s)
{
    for(; *s; s++)
    {
        switch(*s)
        {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\b': fputs("\\b", fp);  break;
            case '\f': fputs("\\f", fp);  break;
            case '\n': fputs("\\n", fp);  break;
            case '\r': fputs("\\r", fp);  break;
            case '\t': fputs("\\t", fp);  break;
            default:
                if((unsigned char)*s < 0x20)
                    fprintf(fp, "\\u%04x", (unsigned char)*s);
                else
                    fputc(*s, fp);
                break;
        }
    }
}

/**
 * Export the result set as a plain-text file (one path per line).
 *
 * @return 0 on success, -1 on error.
 */
static int export_txt(const struct result_set *rs, const char *filepath)
{
    FILE *fp = fopen(filepath, "w");
    if(!fp)
    {
        fprintf(stderr, "Error: cannot open '%s': %s\n", filepath, strerror(errno));
        return -1;
    }

    for(uint32_t i = 0; i < rs->count; i++) fprintf(fp, "%s\n", rs->paths[i]);

    fclose(fp);
    printf("Exported %u result(s) to %s\n", rs->count, filepath);
    return 0;
}

/**
 * Export the result set as a JSON file.
 *
 * Format:
 *   { "count": N, "results": [ "path1", "path2", ... ] }
 *
 * @return 0 on success, -1 on error.
 */
static int export_json(const struct result_set *rs, const char *filepath)
{
    FILE *fp = fopen(filepath, "w");
    if(!fp)
    {
        fprintf(stderr, "Error: cannot open '%s': %s\n", filepath, strerror(errno));
        return -1;
    }

    fprintf(fp, "{\n  \"count\": %u,\n  \"results\": [", rs->count);

    for(uint32_t i = 0; i < rs->count; i++)
    {
        fprintf(fp, "%s\n    \"", i ? "," : "");
        json_escape(fp, rs->paths[i]);
        fputc('"', fp);
    }

    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
    printf("Exported %u result(s) to %s\n", rs->count, filepath);
    return 0;
}

/**
 * Prompt the user to export results.  Accepts:
 *   txt <path>    — export as plain text
 *   json <path>   — export as JSON
 *   (empty)       — skip
 */
static void offer_export(const struct result_set *rs)
{
    char line[4096];

    printf("Export? [txt <path> / json <path> / enter to skip]: ");
    fflush(stdout);

    if(!fgets(line, sizeof(line), stdin)) return;

    /* Strip trailing newline */
    size_t len = strlen(line);
    while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    const char *p = skip_ws(line);
    if(*p == '\0') return;  /* skip */

    char fmt[8];
    const char *after = parse_token(p, fmt, sizeof(fmt));
    if(!after) return;

    after = skip_ws(after);
    if(*after == '\0')
    {
        fprintf(stderr, "Error: expected a file path after '%s'\n", fmt);
        return;
    }

    /* The rest of the line is the file path (may contain spaces) */
    char filepath[4096];
    size_t flen = strlen(after);
    if(flen >= sizeof(filepath)) flen = sizeof(filepath) - 1;
    memcpy(filepath, after, flen);
    filepath[flen] = '\0';

    /* Strip trailing whitespace from path */
    while(flen > 0 && (filepath[flen - 1] == ' ' || filepath[flen - 1] == '\t')) filepath[--flen] = '\0';

    if(strcasecmp(fmt, "txt") == 0)
        export_txt(rs, filepath);
    else if(strcasecmp(fmt, "json") == 0)
        export_json(rs, filepath);
    else
        fprintf(stderr, "Error: unknown format '%s' (use 'txt' or 'json')\n", fmt);
}

/* ------------------------------------------------------------------ */
/*  Query execution                                                    */
/* ------------------------------------------------------------------ */

/**
 * Execute a parsed query via the OBMAFS3_IOC_QUERY_METADATA ioctl.
 * Paginates automatically, prints all matching paths, and offers
 * to export the results as .txt or .json.
 *
 * @param fd  File descriptor on the OBMAFS3 mount (sentinel file).
 * @param qa  Parsed query argument (filters/combine/filter_count set).
 */
static void execute_query(int fd, struct obmafs3_ioctl_metadata_query_arg *qa)
{
    struct result_set rs;
    rs_init(&rs);

    uint32_t offset = 0;

    while(1)
    {
        qa->offset = offset;
        qa->count  = 0;

        if(ioctl(fd, OBMAFS3_IOC_QUERY_METADATA, qa) != 0)
        {
            fprintf(stderr, "Error: ioctl QUERY_METADATA failed: %s\n", strerror(errno));
            rs_free(&rs);
            return;
        }

        if(qa->count == 0) break;

        for(uint32_t i = 0; i < qa->count; i++)
        {
            printf("  %s\n", qa->paths[i]);
            rs_add(&rs, qa->paths[i]);
        }

        offset += qa->count;

        /* If fewer than max results returned, we've reached the end */
        if(qa->count < METADATA_QUERY_MAX_RESULTS) break;
    }

    printf("\n%u result(s)\n", rs.count);

    if(rs.count > 0) offer_export(&rs);

    rs_free(&rs);
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
           "Query syntax:\n"
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
           "  Use * as the key to match across all keys:\n"
           "    * CONTAINS \"maiden\"               Any key's value contains\n"
           "    * = \"Rock\"                        Any key's value equals\n"
           "    * STARTSWITH \"Iron\"               Any key's value starts with\n"
           "\n"
           "  Multiple conditions joined with AND or OR (cannot mix):\n"
           "    artist = \"Iron Maiden\" AND year > \"1985\"\n"
           "    genre = \"Rock\" OR genre = \"Metal\"\n"
           "\n"
           "  Values may be quoted (\"...\") or unquoted single words.\n"
           "  Use \\\" for literal quotes inside quoted strings.\n"
           "  Maximum %d filters per query.\n"
           "\n"
           "Export:\n"
           "  After each query with results you will be offered to export.\n"
           "  Enter 'txt <path>' or 'json <path>' at the export prompt,\n"
           "  or press enter to skip.\n"
           "\n",
           OBMAFS3_QUERY_MAX_FILTERS);
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

static void repl(const char *mountpoint, int query_fd)
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

        /* Skip leading whitespace */
        const char *cmd = line;
        while(*cmd == ' ' || *cmd == '\t') cmd++;

        /* Skip empty lines */
        if(*cmd == '\0') continue;

        if(strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0)
            break;

        if(strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
        {
            print_help();
            continue;
        }

        /* Parse and execute as a query */
        struct obmafs3_ioctl_metadata_query_arg qa;
        if(parse_query(cmd, &qa) == 0)
            execute_query(query_fd, &qa);
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

    int query_fd = open_sentinel(mountpoint);
    if(query_fd < 0) return 1;

    repl(mountpoint, query_fd);

    close(query_fd);
    return 0;
}
