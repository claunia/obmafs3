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
 * Usage: obmafs-query [options] <mountpoint>
 *
 * Options:
 *   -q, --query <query>    Run a single query and exit (batch mode)
 *   -f, --format <fmt>     Output format: txt (default), json, table, or csv
 *   -o, --output <path>    Write results to file instead of stdout
 *   -l, --limit <N>        Return at most N results
 *   -h, --help             Show help
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
 *   <key> ENDSWITH "<value>"        Suffix match
 *   <key> IN "<val1>,<val2>,..."     Match any in comma-separated list
 *   <key> BETWEEN "<low>" "<high>"  Inclusive range (lexicographic)
 *   <key> GLOB "<pattern>"          Wildcard match (*, ?, [abc])
 *   <key> EXISTS                    Key exists (any value)
 *
 *   Case-insensitive:
 *   <key> I= "<value>"              Equal (ignoring case)
 *   <key> I!= "<value>"             Not equal (ignoring case)
 *   <key> ICONTAINS "<value>"       Substring (ignoring case)
 *   <key> ISTARTSWITH "<value>"     Prefix (ignoring case)
 *   <key> IENDSWITH "<value>"       Suffix (ignoring case)
 *   <key> IIN "<val1>,<val2>,..."    IN match (ignoring case)
 *   <key> IGLOB "<pattern>"         Wildcard match (ignoring case)
 *
 *   Numeric (values parsed as integers):
 *   <key> N= "<value>"              Numeric equal
 *   <key> N!= "<value>"             Numeric not equal
 *   <key> N> "<value>"              Numeric greater than
 *   <key> N< "<value>"              Numeric less than
 *   <key> N>= "<value>"             Numeric greater or equal
 *   <key> N<= "<value>"             Numeric less or equal
 *   <key> NBETWEEN "<low>" "<high>" Inclusive numeric range
 *
 *   Regex (POSIX extended regular expressions):
 *   <key> REGEX "<pattern>"         Regex match (case-sensitive)
 *   <key> IREGEX "<pattern>"        Regex match (case-insensitive)
 *
 *   Negation (NOT prefix):
 *   NOT <key> = "<value>"            Invert a single filter
 *   NOT <key> EXISTS                 Files without this key
 *
 *   Multiple conditions joined with AND or OR:
 *     artist I= "iron maiden" AND year N> "1985"
 *     NOT genre = "Pop" AND year N> "1985"
 *
 *   Parentheses for mixed AND/OR (one group allowed):
 *     (genre = "Rock" OR genre = "Metal") AND year N> "1985"
 *     artist = "Iron Maiden" OR (year N>= "1980" AND year N<= "1990")
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* Forward declarations */
static int key_in_filter(const char *key, const char *key_filter);

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
    while(*p && *p != ' ' && *p != '\t' && *p != '"' && *p != '=' && *p != '!' && *p != '<' && *p != '>' && *p != '*' &&
          *p != '(' && *p != ')')
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
 *   I=  I!=  ICONTAINS  ISTARTSWITH         (case-insensitive)
 *   N=  N!=  N>  N<  N>=  N<=               (numeric)
 *
 * @param p     Input pointer (after key + whitespace).
 * @param op    Output operator value.
 * @param need_value  Output: 1 if a value operand is expected, 0 for EXISTS.
 * @return Pointer past the operator, or NULL on error.
 */
static const char *parse_operator(const char *p, uint8_t *op, int *need_value)
{
    *need_value = 1;

    /* Numeric operators: N=  N!=  N>=  N<=  N>  N< */
    if((p[0] == 'N' || p[0] == 'n') && (p[1] == '=' || p[1] == '!' || p[1] == '>' || p[1] == '<'))
    {
        if(p[1] == '!' && p[2] == '=') { *op = kQueryOpNumNotEqual;  return p + 3; }
        if(p[1] == '>' && p[2] == '=') { *op = kQueryOpNumGreaterEq; return p + 3; }
        if(p[1] == '<' && p[2] == '=') { *op = kQueryOpNumLessEq;    return p + 3; }
        if(p[1] == '=')                { *op = kQueryOpNumEqual;      return p + 2; }
        if(p[1] == '>')                { *op = kQueryOpNumGreater;    return p + 2; }
        if(p[1] == '<')                { *op = kQueryOpNumLess;       return p + 2; }
    }

    /* Case-insensitive operators: I=  I!= */
    if((p[0] == 'I' || p[0] == 'i') && (p[1] == '=' || p[1] == '!'))
    {
        if(p[1] == '!' && p[2] == '=') { *op = kQueryOpINotEqual; return p + 3; }
        if(p[1] == '=')                { *op = kQueryOpIEqual;     return p + 2; }
    }

    /* Two-character operators first */
    if(p[0] == '!' && p[1] == '=')
    {
        *op = kQueryOpNotEqual;
        return p + 2;
    }
    if(p[0] == '>' && p[1] == '=')
    {
        *op = kQueryOpGreaterEq;
        return p + 2;
    }
    if(p[0] == '<' && p[1] == '=')
    {
        *op = kQueryOpLessEq;
        return p + 2;
    }

    /* Single-character operators */
    if(p[0] == '=')
    {
        *op = kQueryOpEqual;
        return p + 1;
    }
    if(p[0] == '>')
    {
        *op = kQueryOpGreater;
        return p + 1;
    }
    if(p[0] == '<')
    {
        *op = kQueryOpLess;
        return p + 1;
    }

    /* Keyword operators */
    char        kw[32];
    const char *after = parse_token(p, kw, sizeof(kw));
    if(!after)
    {
        fprintf(stderr, "Error: expected operator after key\n");
        return NULL;
    }

    if(strcasecmp(kw, "CONTAINS") == 0)
    {
        *op = kQueryOpContains;
        return after;
    }
    if(strcasecmp(kw, "STARTSWITH") == 0)
    {
        *op = kQueryOpStartsWith;
        return after;
    }
    if(strcasecmp(kw, "EXISTS") == 0)
    {
        *op         = kQueryOpExists;
        *need_value = 0;
        return after;
    }
    if(strcasecmp(kw, "ICONTAINS") == 0)
    {
        *op = kQueryOpIContains;
        return after;
    }
    if(strcasecmp(kw, "ISTARTSWITH") == 0)
    {
        *op = kQueryOpIStartsWith;
        return after;
    }
    if(strcasecmp(kw, "ENDSWITH") == 0)
    {
        *op = kQueryOpEndsWith;
        return after;
    }
    if(strcasecmp(kw, "IENDSWITH") == 0)
    {
        *op = kQueryOpIEndsWith;
        return after;
    }
    if(strcasecmp(kw, "IN") == 0)
    {
        *op = kQueryOpIn;
        return after;
    }
    if(strcasecmp(kw, "IIN") == 0)
    {
        *op = kQueryOpIIn;
        return after;
    }
    if(strcasecmp(kw, "BETWEEN") == 0)
    {
        *op         = kQueryOpBetween;
        *need_value = 2;
        return after;
    }
    if(strcasecmp(kw, "NBETWEEN") == 0)
    {
        *op         = kQueryOpNumBetween;
        *need_value = 2;
        return after;
    }
    if(strcasecmp(kw, "GLOB") == 0)
    {
        *op = kQueryOpGlob;
        return after;
    }
    if(strcasecmp(kw, "IGLOB") == 0)
    {
        *op = kQueryOpIGlob;
        return after;
    }
    if(strcasecmp(kw, "REGEX") == 0)
    {
        *op = kQueryOpRegex;
        return after;
    }
    if(strcasecmp(kw, "IREGEX") == 0)
    {
        *op = kQueryOpIRegex;
        return after;
    }

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

    if(!need_value) return p; /* EXISTS — no value */

    p = skip_ws(p);

    /* Parse value — quoted or unquoted */
    if(*p == '"') { p = parse_quoted(p, flt->value, sizeof(flt->value)); }
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

    /* BETWEEN/NBETWEEN: parse second value and append with comma separator */
    if(need_value == 2)
    {
        p = skip_ws(p);
        char val2[METADATA_VALUE_MAX];
        if(*p == '"') { p = parse_quoted(p, val2, sizeof(val2)); }
        else           { p = parse_token(p, val2, sizeof(val2)); }
        if(!p)
        {
            fprintf(stderr, "Error: BETWEEN requires two values\n");
            return NULL;
        }
        size_t v1len = strlen(flt->value);
        if(v1len + 1 + strlen(val2) < sizeof(flt->value))
        {
            flt->value[v1len] = ',';
            memcpy(flt->value + v1len + 1, val2, strlen(val2) + 1);
        }
    }

    return p;
}

/**
 * Parse a full query line into an ioctl metadata query argument.
 *
 * Grammar:
 *   query  := piece (combiner piece)*
 *   piece  := [NOT] filter | '(' [NOT] filter (combiner [NOT] filter)* ')'
 *   filter := key operator [value]
 *
 * Filters outside parentheses are group 0 (combined with @c combine).
 * Filters inside parentheses are group 1 (combined with @c combine1).
 * The combiner between a parenthesized and a non-parenthesized piece
 * sets @c group_combine.  Only one parenthesized group is allowed.
 *
 * @param input  The query string.
 * @param qa     Output ioctl argument.
 * @return 0 on success, -1 on error (message printed to stderr).
 */
static int parse_query(const char *input, struct obmafs3_ioctl_metadata_query_arg *qa)
{
    memset(qa, 0, sizeof(*qa));

    const char *p            = input;
    uint8_t     nf           = 0;
    int         in_parens    = 0;
    int         have_parens  = 0;
    int         combine0_set = 0;
    int         combine1_set = 0;
    int         gcombine_set = 0;
    int         prev_close   = 0;

    while(1)
    {
        if(nf >= OBMAFS3_QUERY_MAX_FILTERS)
        {
            fprintf(stderr, "Error: too many filters (max %d)\n", OBMAFS3_QUERY_MAX_FILTERS);
            return -1;
        }

        p = skip_ws(p);
        if(!*p)
        {
            if(nf == 0)
            {
                fprintf(stderr, "Error: expected a filter condition\n");
                return -1;
            }
            break;
        }

        /* Opening parenthesis → start group 1 */
        if(*p == '(')
        {
            if(in_parens)
            {
                fprintf(stderr, "Error: nested parentheses are not supported\n");
                return -1;
            }
            if(have_parens)
            {
                fprintf(stderr, "Error: only one parenthesized group is allowed\n");
                return -1;
            }
            in_parens   = 1;
            have_parens = 1;
            p++;
            p = skip_ws(p);
        }

        /* NOT prefix */
        int negate = 0;
        {
            char        kw[8];
            const char *after = parse_token(p, kw, sizeof(kw));
            if(after && strcasecmp(kw, "NOT") == 0)
            {
                const char *peek = skip_ws(after);
                /* Only treat as NOT prefix if not followed by an operator char */
                if(*peek && *peek != '=' && *peek != '!' && *peek != '>' && *peek != '<' && *peek != ')')
                {
                    negate = 1;
                    p      = peek;
                }
            }
        }

        /* Parse filter (memsets flt to 0, so set negate/group after) */
        p = parse_one_filter(p, &qa->filters[nf]);
        if(!p) return -1;
        qa->filters[nf].negate = negate ? 1 : 0;
        qa->filters[nf].group  = in_parens ? 1 : 0;
        nf++;

        p = skip_ws(p);

        /* Closing parenthesis */
        prev_close = 0;
        if(*p == ')')
        {
            if(!in_parens)
            {
                fprintf(stderr, "Error: unexpected ')'\n");
                return -1;
            }
            in_parens  = 0;
            prev_close = 1;
            p++;
            p = skip_ws(p);
        }

        if(!*p) break; /* end of input */

        /* Parse combiner: AND | OR */
        char        kw[8];
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

        /* Peek ahead to see if the next piece starts with '(' */
        const char *peek         = skip_ws(after);
        int         next_is_open = (*peek == '(');

        /* Classify the combiner */
        if(prev_close || next_is_open)
        {
            /* Between groups → group_combine */
            if(gcombine_set && qa->group_combine != this_combine)
            {
                fprintf(stderr, "Error: cannot mix AND and OR between groups\n");
                return -1;
            }
            qa->group_combine = this_combine;
            gcombine_set      = 1;
        }
        else if(in_parens)
        {
            /* Inside parentheses → combine1 (group 1) */
            if(combine1_set && qa->combine1 != this_combine)
            {
                fprintf(stderr, "Error: cannot mix AND and OR inside parentheses\n");
                return -1;
            }
            qa->combine1 = this_combine;
            combine1_set = 1;
        }
        else
        {
            /* Between non-parenthesized pieces → combine (group 0) */
            if(combine0_set && qa->combine != this_combine)
            {
                fprintf(stderr, "Error: cannot mix AND and OR outside parentheses\n");
                return -1;
            }
            qa->combine = this_combine;
            combine0_set = 1;
        }

        p = after;
    }

    if(in_parens)
    {
        fprintf(stderr, "Error: unclosed parenthesis\n");
        return -1;
    }

    /* Default: if combine0 not explicitly set but group_combine is, inherit it */
    if(!combine0_set && gcombine_set) qa->combine = qa->group_combine;

    qa->filter_count = nf;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Result set                                                         */
/* ------------------------------------------------------------------ */

/** A single metadata key/value pair. */
struct meta_pair
{
    char key[METADATA_KEY_MAX];
    char value[METADATA_VALUE_MAX];
};

/** Per-result metadata (growable array of key/value pairs). */
struct meta_list
{
    struct meta_pair *pairs;
    uint32_t          count;
    uint32_t          cap;
};

static void ml_init(struct meta_list *ml)
{
    ml->pairs = NULL;
    ml->count = 0;
    ml->cap   = 0;
}

static void ml_free(struct meta_list *ml)
{
    free(ml->pairs);
    ml_init(ml);
}

static int ml_add(struct meta_list *ml, const char *key, const char *value)
{
    if(ml->count >= ml->cap)
    {
        uint32_t nc = ml->cap ? ml->cap * 2 : 8;
        struct meta_pair *tmp = realloc(ml->pairs, nc * sizeof(struct meta_pair));
        if(!tmp) return -1;
        ml->pairs = tmp;
        ml->cap   = nc;
    }
    strncpy(ml->pairs[ml->count].key, key, METADATA_KEY_MAX - 1);
    ml->pairs[ml->count].key[METADATA_KEY_MAX - 1] = '\0';
    strncpy(ml->pairs[ml->count].value, value, METADATA_VALUE_MAX - 1);
    ml->pairs[ml->count].value[METADATA_VALUE_MAX - 1] = '\0';
    ml->count++;
    return 0;
}

/** Growable array of path strings collected from paginated queries. */
struct result_set
{
    char            **paths;    /**< Heap-allocated array of strdup'd paths */
    struct meta_list *metadata; /**< Per-result metadata (NULL if not fetched) */
    uint32_t          count;    /**< Number of entries */
    uint32_t          cap;      /**< Allocated capacity */
};

static void rs_init(struct result_set *rs)
{
    rs->paths    = NULL;
    rs->metadata = NULL;
    rs->count    = 0;
    rs->cap      = 0;
}

static void rs_free(struct result_set *rs)
{
    for(uint32_t i = 0; i < rs->count; i++) free(rs->paths[i]);
    free(rs->paths);
    if(rs->metadata)
    {
        for(uint32_t i = 0; i < rs->count; i++) ml_free(&rs->metadata[i]);
        free(rs->metadata);
    }
    rs_init(rs);
}

static int rs_add(struct result_set *rs, const char *path)
{
    if(rs->count == rs->cap)
    {
        uint32_t newcap = rs->cap ? rs->cap * 2 : 64;
        char   **tmp    = realloc(rs->paths, newcap * sizeof(char *));
        if(!tmp)
        {
            fprintf(stderr, "Error: out of memory\n");
            return -1;
        }
        rs->paths = tmp;
        rs->cap   = newcap;
    }
    rs->paths[rs->count] = strdup(path);
    if(!rs->paths[rs->count])
    {
        fprintf(stderr, "Error: out of memory\n");
        return -1;
    }
    rs->count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Metadata fetch                                                     */
/* ------------------------------------------------------------------ */

/**
 * Fetch all metadata key/value pairs for a file by opening it and
 * issuing LIST_METADATA + GET_METADATA ioctls.
 *
 * @param mountpoint  Filesystem mount path.
 * @param rel_path    Path relative to the mount root (starts with /).
 * @param ml          Output meta_list (must be ml_init'd by caller).
 * @return 0 on success, -1 on failure.
 */
static int fetch_file_metadata(const char *mountpoint, const char *rel_path, struct meta_list *ml)
{
    char fullpath[4096];
    snprintf(fullpath, sizeof(fullpath), "%s%s", mountpoint, rel_path);

    int fd = open(fullpath, O_RDONLY);
    if(fd < 0) return -1;

    /* List all keys (paginated, up to 256 keys) */
    char    keys[256][METADATA_KEY_MAX];
    uint32_t nkeys = 0;

    uint32_t list_offset = 0;
    while(nkeys < 256)
    {
        struct obmafs3_ioctl_metadata_list_arg la;
        memset(&la, 0, sizeof(la));
        la.offset = list_offset;

        if(ioctl(fd, OBMAFS3_IOC_LIST_METADATA, &la) != 0) break;
        if(la.count == 0) break;

        for(uint32_t i = 0; i < la.count && nkeys < 256; i++)
        {
            memcpy(keys[nkeys], la.keys[i], METADATA_KEY_MAX);
            nkeys++;
        }
        list_offset += la.count;
        if(la.count < 16) break;
    }

    /* Get value for each key */
    for(uint32_t i = 0; i < nkeys; i++)
    {
        struct obmafs3_ioctl_metadata_get_arg ga;
        memset(&ga, 0, sizeof(ga));
        memcpy(ga.key, keys[i], METADATA_KEY_MAX);

        if(ioctl(fd, OBMAFS3_IOC_GET_METADATA, &ga) == 0)
            ml_add(ml, ga.key, ga.value);
    }

    close(fd);
    return 0;
}

/**
 * Fetch metadata for all results in a result set.
 *
 * @param mountpoint  Filesystem mount path.
 * @param rs          Result set (must have count > 0).
 */
static void rs_fetch_metadata(const char *mountpoint, struct result_set *rs)
{
    if(rs->count == 0) return;
    rs->metadata = calloc(rs->count, sizeof(struct meta_list));
    if(!rs->metadata) return;

    for(uint32_t i = 0; i < rs->count; i++)
    {
        ml_init(&rs->metadata[i]);
        fetch_file_metadata(mountpoint, rs->paths[i], &rs->metadata[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Result sorting                                                     */
/* ------------------------------------------------------------------ */

/** Context passed to the qsort comparator via a file-scope global. */
static struct
{
    const struct result_set *rs;
    const char              *sort_key;
    int                      numeric;
    int                      reverse;
} g_sort_ctx;

/**
 * Find the value of @p key in a meta_list.
 * Returns "" if not found (sorts empty values first in ascending order).
 */
static const char *ml_find(const struct meta_list *ml, const char *key)
{
    if(!ml) return "";
    for(uint32_t i = 0; i < ml->count; i++)
        if(strncmp(ml->pairs[i].key, key, METADATA_KEY_MAX) == 0)
            return ml->pairs[i].value;
    return "";
}

static int rs_sort_cmp(const void *a, const void *b)
{
    uint32_t ia = *(const uint32_t *)a;
    uint32_t ib = *(const uint32_t *)b;
    int      cmp;

    if(strcmp(g_sort_ctx.sort_key, "path") == 0)
    {
        cmp = strcmp(g_sort_ctx.rs->paths[ia], g_sort_ctx.rs->paths[ib]);
    }
    else if(g_sort_ctx.numeric)
    {
        int64_t va = strtoll(ml_find(&g_sort_ctx.rs->metadata[ia], g_sort_ctx.sort_key), NULL, 10);
        int64_t vb = strtoll(ml_find(&g_sort_ctx.rs->metadata[ib], g_sort_ctx.sort_key), NULL, 10);
        cmp        = (va > vb) - (va < vb);
    }
    else
    {
        const char *va = ml_find(&g_sort_ctx.rs->metadata[ia], g_sort_ctx.sort_key);
        const char *vb = ml_find(&g_sort_ctx.rs->metadata[ib], g_sort_ctx.sort_key);
        cmp            = strncmp(va, vb, METADATA_VALUE_MAX);
    }

    return g_sort_ctx.reverse ? -cmp : cmp;
}

/**
 * Sort a result set by a metadata key or by path.
 *
 * @param rs        Result set (must have metadata fetched unless
 *                  sort_key is "path").
 * @param sort_key  Metadata key to sort by, or "path" for path order.
 *                  Prefix with "N:" for numeric sort (e.g. "N:year").
 * @param reverse   Non-zero for descending order.
 */
static void rs_sort(struct result_set *rs, const char *sort_key, int reverse)
{
    if(rs->count <= 1) return;

    int         numeric  = 0;
    const char *real_key = sort_key;
    if(strncasecmp(sort_key, "N:", 2) == 0)
    {
        numeric  = 1;
        real_key = sort_key + 2;
    }

    /* Build an index array, sort it, then reorder paths+metadata in-place */
    uint32_t *idx = malloc(rs->count * sizeof(uint32_t));
    if(!idx) return;
    for(uint32_t i = 0; i < rs->count; i++) idx[i] = i;

    g_sort_ctx.rs       = rs;
    g_sort_ctx.sort_key = real_key;
    g_sort_ctx.numeric  = numeric;
    g_sort_ctx.reverse  = reverse;

    qsort(idx, rs->count, sizeof(uint32_t), rs_sort_cmp);

    /* Reorder paths and metadata according to sorted index */
    char            **new_paths = malloc(rs->count * sizeof(char *));
    struct meta_list *new_meta  = rs->metadata ? malloc(rs->count * sizeof(struct meta_list)) : NULL;
    if(!new_paths) { free(idx); return; }

    for(uint32_t i = 0; i < rs->count; i++)
    {
        new_paths[i] = rs->paths[idx[i]];
        if(new_meta) new_meta[i] = rs->metadata[idx[i]];
    }

    free(rs->paths);
    rs->paths = new_paths;
    if(new_meta)
    {
        free(rs->metadata);
        rs->metadata = new_meta;
    }
    free(idx);
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
            case '"':
                fputs("\\\"", fp);
                break;
            case '\\':
                fputs("\\\\", fp);
                break;
            case '\b':
                fputs("\\b", fp);
                break;
            case '\f':
                fputs("\\f", fp);
                break;
            case '\n':
                fputs("\\n", fp);
                break;
            case '\r':
                fputs("\\r", fp);
                break;
            case '\t':
                fputs("\\t", fp);
                break;
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

    for(uint32_t i = 0; i < rs->count; i++)
    {
        fprintf(fp, "%s\n", rs->paths[i]);
        if(rs->metadata && rs->metadata[i].count > 0)
        {
            for(uint32_t j = 0; j < rs->metadata[i].count; j++)
                fprintf(fp, "  %s = %s\n", rs->metadata[i].pairs[j].key, rs->metadata[i].pairs[j].value);
        }
    }

    fclose(fp);
    printf("Exported %u result(s) to %s\n", rs->count, filepath);
    return 0;
}

/**
 * Write a single JSON result entry to @p fp.  If metadata is available,
 * writes an object with "path" and "metadata"; otherwise a plain string.
 */
static void json_write_result(FILE *fp, const char *path, const struct meta_list *ml)
{
    if(ml && ml->count > 0)
    {
        fprintf(fp, "{\"path\": \"");
        json_escape(fp, path);
        fprintf(fp, "\", \"metadata\": {");
        for(uint32_t j = 0; j < ml->count; j++)
        {
            if(j > 0) fputc(',', fp);
            fprintf(fp, " \"");
            json_escape(fp, ml->pairs[j].key);
            fprintf(fp, "\": \"");
            json_escape(fp, ml->pairs[j].value);
            fputc('"', fp);
        }
        fprintf(fp, " }}");
    }
    else
    {
        fputc('"', fp);
        json_escape(fp, path);
        fputc('"', fp);
    }
}

/**
 * Export the result set as a JSON file.
 *
 * Without metadata:
 *   { "count": N, "results": [ "path1", "path2", ... ] }
 *
 * With metadata:
 *   { "count": N, "results": [
 *       { "path": "...", "metadata": { "key": "val", ... } }, ...
 *   ] }
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
        fprintf(fp, "%s\n    ", i ? "," : "");
        json_write_result(fp, rs->paths[i], rs->metadata ? &rs->metadata[i] : NULL);
    }

    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
    printf("Exported %u result(s) to %s\n", rs->count, filepath);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Table output                                                       */
/* ------------------------------------------------------------------ */

#define TABLE_MAX_COLS 32

/**
 * Collect the union of all metadata key names across all results.
 * Returns heap-allocated array of strdup'd keys; caller frees.
 */
static uint32_t collect_column_keys(const struct result_set *rs, char ***out_keys)
{
    char    **keys = NULL;
    uint32_t  n    = 0;
    uint32_t  cap  = 0;

    for(uint32_t i = 0; i < rs->count && rs->metadata; i++)
    {
        for(uint32_t j = 0; j < rs->metadata[i].count; j++)
        {
            /* Check if key already collected */
            int found = 0;
            for(uint32_t k = 0; k < n; k++)
            {
                if(strcmp(keys[k], rs->metadata[i].pairs[j].key) == 0)
                {
                    found = 1;
                    break;
                }
            }
            if(found || n >= TABLE_MAX_COLS) continue;

            if(n >= cap)
            {
                cap = cap ? cap * 2 : 8;
                char **tmp = realloc(keys, cap * sizeof(char *));
                if(!tmp) break;
                keys = tmp;
            }
            keys[n++] = strdup(rs->metadata[i].pairs[j].key);
        }
    }

    *out_keys = keys;
    return n;
}

/**
 * Write a result set as an aligned table to @p fp.
 * Columns: PATH, then each metadata key found across all results.
 */
static void write_table(FILE *fp, const struct result_set *rs)
{
    char    **col_keys;
    uint32_t  ncols = collect_column_keys(rs, &col_keys);

    /* Column widths: col_widths[0] = PATH, col_widths[1..ncols] = metadata keys */
    uint32_t total_cols = 1 + ncols;
    size_t  *widths     = calloc(total_cols, sizeof(size_t));
    if(!widths)
    {
        for(uint32_t i = 0; i < ncols; i++) free(col_keys[i]);
        free(col_keys);
        return;
    }

    /* Header widths */
    widths[0] = 4; /* "PATH" */
    for(uint32_t c = 0; c < ncols; c++)
    {
        size_t klen = strlen(col_keys[c]);
        widths[1 + c] = klen;
    }

    /* Data widths */
    for(uint32_t i = 0; i < rs->count; i++)
    {
        size_t plen = strlen(rs->paths[i]);
        if(plen > widths[0]) widths[0] = plen;

        for(uint32_t c = 0; c < ncols; c++)
        {
            const char *val = rs->metadata ? ml_find(&rs->metadata[i], col_keys[c]) : "";
            size_t      vlen = strlen(val);
            if(vlen > widths[1 + c]) widths[1 + c] = vlen;
        }
    }

    /* Print header */
    fprintf(fp, "%-*s", (int)widths[0], "PATH");
    for(uint32_t c = 0; c < ncols; c++)
        fprintf(fp, "  %-*s", (int)widths[1 + c], col_keys[c]);
    fputc('\n', fp);

    /* Separator */
    for(uint32_t c = 0; c < total_cols; c++)
    {
        if(c > 0) fputs("  ", fp);
        for(size_t s = 0; s < widths[c]; s++) fputc('-', fp);
    }
    fputc('\n', fp);

    /* Rows */
    for(uint32_t i = 0; i < rs->count; i++)
    {
        fprintf(fp, "%-*s", (int)widths[0], rs->paths[i]);
        for(uint32_t c = 0; c < ncols; c++)
        {
            const char *val = rs->metadata ? ml_find(&rs->metadata[i], col_keys[c]) : "";
            fprintf(fp, "  %-*s", (int)widths[1 + c], val);
        }
        fputc('\n', fp);
    }

    free(widths);
    for(uint32_t i = 0; i < ncols; i++) free(col_keys[i]);
    free(col_keys);
}

static int export_table(const struct result_set *rs, const char *filepath)
{
    FILE *fp = fopen(filepath, "w");
    if(!fp)
    {
        fprintf(stderr, "Error: cannot open '%s': %s\n", filepath, strerror(errno));
        return -1;
    }
    write_table(fp, rs);
    fclose(fp);
    printf("Exported %u result(s) to %s\n", rs->count, filepath);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  CSV output                                                         */
/* ------------------------------------------------------------------ */

/** Write a CSV-escaped field to @p fp.  Quotes the field if it contains
 *  a comma, double-quote, or newline. */
static void csv_field(FILE *fp, const char *s)
{
    int need_quote = 0;
    for(const char *p = s; *p; p++)
    {
        if(*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
        {
            need_quote = 1;
            break;
        }
    }
    if(need_quote)
    {
        fputc('"', fp);
        for(const char *p = s; *p; p++)
        {
            if(*p == '"') fputc('"', fp); /* double the quote */
            fputc(*p, fp);
        }
        fputc('"', fp);
    }
    else
    {
        fputs(s, fp);
    }
}

/**
 * Write a result set as CSV to @p fp.
 * Columns: PATH, then each metadata key found across all results.
 */
static void write_csv(FILE *fp, const struct result_set *rs)
{
    char    **col_keys;
    uint32_t  ncols = collect_column_keys(rs, &col_keys);

    /* Header row */
    csv_field(fp, "PATH");
    for(uint32_t c = 0; c < ncols; c++)
    {
        fputc(',', fp);
        csv_field(fp, col_keys[c]);
    }
    fputc('\n', fp);

    /* Data rows */
    for(uint32_t i = 0; i < rs->count; i++)
    {
        csv_field(fp, rs->paths[i]);
        for(uint32_t c = 0; c < ncols; c++)
        {
            fputc(',', fp);
            const char *val = rs->metadata ? ml_find(&rs->metadata[i], col_keys[c]) : "";
            csv_field(fp, val);
        }
        fputc('\n', fp);
    }

    for(uint32_t i = 0; i < ncols; i++) free(col_keys[i]);
    free(col_keys);
}

static int export_csv(const struct result_set *rs, const char *filepath)
{
    FILE *fp = fopen(filepath, "w");
    if(!fp)
    {
        fprintf(stderr, "Error: cannot open '%s': %s\n", filepath, strerror(errno));
        return -1;
    }
    write_csv(fp, rs);
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

    printf("Export? [txt <path> / json <path> / table <path> / csv <path> / enter to skip]: ");
    fflush(stdout);

    if(!fgets(line, sizeof(line), stdin)) return;

    /* Strip trailing newline */
    size_t len = strlen(line);
    while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    const char *p = skip_ws(line);
    if(*p == '\0') return; /* skip */

    char        fmt[8];
    const char *after = parse_token(p, fmt, sizeof(fmt));
    if(!after) return;

    after = skip_ws(after);
    if(*after == '\0')
    {
        fprintf(stderr, "Error: expected a file path after '%s'\n", fmt);
        return;
    }

    /* The rest of the line is the file path (may contain spaces) */
    char   filepath[4096];
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
    else if(strcasecmp(fmt, "table") == 0)
        export_table(rs, filepath);
    else if(strcasecmp(fmt, "csv") == 0)
        export_csv(rs, filepath);
    else
        fprintf(stderr, "Error: unknown format '%s' (use 'txt', 'json', 'table', or 'csv')\n", fmt);
}

/* ------------------------------------------------------------------ */
/*  Query execution                                                    */
/* ------------------------------------------------------------------ */

/**
 * Execute a parsed query via the OBMAFS3_IOC_QUERY_METADATA ioctl.
 * Paginates automatically and collects all matching paths into @p rs.
 *
 * @param fd           File descriptor on the OBMAFS3 mount (sentinel file).
 * @param qa           Parsed query argument (filters/combine/filter_count set).
 * @param rs           Output result set (must be rs_init'd by caller).
 * @param max_results  Maximum results to collect, or 0 for unlimited.
 * @return 0 on success, -1 on ioctl error.
 */
static int execute_query_collect(int fd, struct obmafs3_ioctl_metadata_query_arg *qa, struct result_set *rs,
                                 uint32_t max_results)
{
    uint32_t offset = 0;

    /* First call: cursor_id starts at 0 (new query).
     * Subsequent calls reuse the cursor_id returned by the server,
     * avoiding redundant B+Tree scans and set algebra. */
    qa->cursor_id = 0;

    while(1)
    {
        qa->offset = offset;
        qa->count  = 0;
        qa->total  = 0;

        if(ioctl(fd, OBMAFS3_IOC_QUERY_METADATA, qa) != 0)
        {
            fprintf(stderr, "Error: ioctl QUERY_METADATA failed: %s\n", strerror(errno));
            return -1;
        }

        if(qa->count == 0) break;

        for(uint32_t i = 0; i < qa->count; i++)
        {
            rs_add(rs, qa->paths[i]);
            if(max_results > 0 && rs->count >= max_results) goto collect_done;
        }

        offset += qa->count;

        /* Stop when we've collected all results */
        if(offset >= qa->total) break;

        /* cursor_id is now set by the server; subsequent calls skip the query */
    }
collect_done:
    return 0;
}

/**
 * Execute a count-only query: sets offset to UINT32_MAX so the
 * kernel/library skips path resolution entirely.  Returns the
 * total number of matching inodes.
 *
 * @param fd  File descriptor on the OBMAFS3 mount.
 * @param qa  Parsed query argument.
 * @return Total count, or (uint32_t)-1 on error.
 */
static uint32_t execute_query_count(int fd, struct obmafs3_ioctl_metadata_query_arg *qa)
{
    qa->offset    = UINT32_MAX;
    qa->count     = 0;
    qa->total     = 0;
    qa->cursor_id = 0;

    if(ioctl(fd, OBMAFS3_IOC_QUERY_METADATA, qa) != 0)
    {
        fprintf(stderr, "Error: ioctl QUERY_METADATA failed: %s\n", strerror(errno));
        return (uint32_t)-1;
    }

    return qa->total;
}

/* ------------------------------------------------------------------ */
/*  Query explain                                                      */
/* ------------------------------------------------------------------ */

static const char *op_name(uint8_t op)
{
    switch(op)
    {
        case kQueryOpEqual:       return "=";
        case kQueryOpNotEqual:    return "!=";
        case kQueryOpGreater:     return ">";
        case kQueryOpLess:        return "<";
        case kQueryOpGreaterEq:   return ">=";
        case kQueryOpLessEq:      return "<=";
        case kQueryOpContains:    return "CONTAINS";
        case kQueryOpStartsWith:  return "STARTSWITH";
        case kQueryOpEndsWith:    return "ENDSWITH";
        case kQueryOpExists:      return "EXISTS";
        case kQueryOpIEqual:      return "I=";
        case kQueryOpINotEqual:   return "I!=";
        case kQueryOpIContains:   return "ICONTAINS";
        case kQueryOpIStartsWith: return "ISTARTSWITH";
        case kQueryOpIEndsWith:   return "IENDSWITH";
        case kQueryOpNumEqual:    return "N=";
        case kQueryOpNumNotEqual: return "N!=";
        case kQueryOpNumGreater:  return "N>";
        case kQueryOpNumLess:     return "N<";
        case kQueryOpNumGreaterEq: return "N>=";
        case kQueryOpNumLessEq:   return "N<=";
        case kQueryOpRegex:       return "REGEX";
        case kQueryOpIRegex:      return "IREGEX";
        case kQueryOpIn:          return "IN";
        case kQueryOpIIn:         return "IIN";
        case kQueryOpBetween:     return "BETWEEN";
        case kQueryOpNumBetween:  return "NBETWEEN";
        case kQueryOpGlob:        return "GLOB";
        case kQueryOpIGlob:       return "IGLOB";
        default:                  return "?";
    }
}

static const char *combine_name(uint8_t c) { return c == kQueryCombineAnd ? "AND" : "OR"; }

/**
 * Run an explain on a parsed query: show per-filter match counts,
 * per-group totals, and the final combined result count.
 */
static void execute_explain(int fd, struct obmafs3_ioctl_metadata_query_arg *qa)
{
    printf("\nQuery plan:\n");

    /* Per-filter counts: issue each filter as a standalone count-only query */
    uint32_t per_filter[OBMAFS3_QUERY_MAX_FILTERS];
    for(uint8_t f = 0; f < qa->filter_count; f++)
    {
        struct obmafs3_ioctl_metadata_query_arg solo;
        memset(&solo, 0, sizeof(solo));
        solo.filter_count = 1;
        solo.combine      = kQueryCombineAnd;
        solo.filters[0]   = qa->filters[f];
        /* Clear negate for the raw count — we show both raw and negated */
        solo.filters[0].negate = 0;
        solo.filters[0].group  = 0;

        uint32_t raw = execute_query_count(fd, &solo);

        const char *key_str = qa->filters[f].key;
        const char *op_str  = op_name(qa->filters[f].op);
        const char *val_str = qa->filters[f].value;

        if(qa->filters[f].op == kQueryOpExists)
            printf("  Filter %u: %s %s", f + 1, key_str, op_str);
        else
            printf("  Filter %u: %s %s \"%s\"", f + 1, key_str, op_str, val_str);

        if(qa->filters[f].negate)
        {
            /* Also get the negated count */
            solo.filters[0].negate = 1;
            uint32_t neg = execute_query_count(fd, &solo);
            printf(" → %u matches (NOT → %u)", raw, neg);
        }
        else
        {
            printf(" → %u matches", raw);
        }

        if(qa->filter_count > 1)
            printf("  [group %u]", qa->filters[f].group);
        printf("\n");

        per_filter[f] = raw;
    }

    /* Per-group counts (if multiple filters) */
    if(qa->filter_count > 1)
    {
        /* Check if we have filters in group 0 and/or group 1 */
        int g0n = 0, g1n = 0;
        for(uint8_t f = 0; f < qa->filter_count; f++)
        {
            if(qa->filters[f].group == 0) g0n++;
            else                          g1n++;
        }

        if(g0n > 0 && g0n < qa->filter_count)
        {
            /* Run group 0 only */
            struct obmafs3_ioctl_metadata_query_arg g0q;
            memcpy(&g0q, qa, sizeof(g0q));
            g0q.filter_count = 0;
            for(uint8_t f = 0; f < qa->filter_count; f++)
            {
                if(qa->filters[f].group == 0)
                {
                    g0q.filters[g0q.filter_count] = qa->filters[f];
                    g0q.filters[g0q.filter_count].group = 0;
                    g0q.filter_count++;
                }
            }
            g0q.combine = qa->combine;
            uint32_t g0c = execute_query_count(fd, &g0q);
            printf("  Group 0 (%s): %u results\n", combine_name(qa->combine), g0c);
        }

        if(g1n > 0 && g1n < qa->filter_count)
        {
            /* Run group 1 only */
            struct obmafs3_ioctl_metadata_query_arg g1q;
            memcpy(&g1q, qa, sizeof(g1q));
            g1q.filter_count = 0;
            for(uint8_t f = 0; f < qa->filter_count; f++)
            {
                if(qa->filters[f].group == 1)
                {
                    g1q.filters[g1q.filter_count] = qa->filters[f];
                    g1q.filters[g1q.filter_count].group = 0;
                    g1q.filter_count++;
                }
            }
            g1q.combine = qa->combine1;
            uint32_t g1c = execute_query_count(fd, &g1q);
            printf("  Group 1 (%s): %u results\n", combine_name(qa->combine1), g1c);
        }

        if(g0n > 0 && g1n > 0)
            printf("  Groups combined: %s\n", combine_name(qa->group_combine));
        else if(qa->filter_count > 1)
            printf("  Combined: %s\n", combine_name(qa->combine));
    }

    /* Final combined count */
    uint32_t total = execute_query_count(fd, qa);
    printf("  ─────────────────────────────\n");
    printf("  Final result: %u matches\n\n", total);
}

/* ------------------------------------------------------------------ */
/*  Streaming output                                                   */
/* ------------------------------------------------------------------ */

/**
 * Execute a query in streaming mode: print each result as it arrives
 * from the paginated ioctl calls, without buffering the full result set.
 *
 * Supports "txt" (one path per line) and "jsonl" (one JSON object per line).
 * Metadata can be fetched and printed per-result if requested.
 *
 * Incompatible with sorting (requires all results), table/csv (needs
 * column widths), and regular JSON (needs wrapping array).
 *
 * @param fd            File descriptor on the OBMAFS3 mount.
 * @param qa            Parsed query argument.
 * @param format        "txt" or "jsonl" (NULL defaults to "txt").
 * @param mountpoint    Mount path (for metadata fetch).
 * @param show_metadata Whether to fetch and show metadata per result.
 * @param max_results   Stop after N results (0 = unlimited).
 * @param key_filter    Comma-separated key filter, or NULL for all.
 * @return 0 on success, non-zero on error.
 */
static int execute_query_stream(int fd, struct obmafs3_ioctl_metadata_query_arg *qa,
                                const char *format, const char *mountpoint,
                                int show_metadata, uint32_t max_results,
                                const char *key_filter)
{
    const char *fmt = format ? format : "txt";
    int is_jsonl = (strcasecmp(fmt, "jsonl") == 0);

    if(!is_jsonl && strcasecmp(fmt, "txt") != 0)
    {
        fprintf(stderr, "Error: streaming mode only supports 'txt' and 'jsonl' formats\n");
        return 1;
    }

    uint32_t offset = 0;
    uint32_t emitted = 0;
    qa->cursor_id = 0;

    while(1)
    {
        qa->offset = offset;
        qa->count  = 0;
        qa->total  = 0;

        if(ioctl(fd, OBMAFS3_IOC_QUERY_METADATA, qa) != 0)
        {
            fprintf(stderr, "Error: ioctl QUERY_METADATA failed: %s\n", strerror(errno));
            return 1;
        }

        if(qa->count == 0) break;

        for(uint32_t i = 0; i < qa->count; i++)
        {
            const char *path = qa->paths[i];

            if(is_jsonl)
            {
                printf("{\"path\": \"");
                json_escape(stdout, path);
                putchar('"');

                if(show_metadata)
                {
                    struct meta_list ml;
                    ml_init(&ml);
                    fetch_file_metadata(mountpoint, path, &ml);
                    if(ml.count > 0)
                    {
                        printf(", \"metadata\": {");
                        int first = 1;
                        for(uint32_t j = 0; j < ml.count; j++)
                        {
                            if(!key_in_filter(ml.pairs[j].key, key_filter)) continue;
                            if(!first) putchar(',');
                            printf(" \"");
                            json_escape(stdout, ml.pairs[j].key);
                            printf("\": \"");
                            json_escape(stdout, ml.pairs[j].value);
                            putchar('"');
                            first = 0;
                        }
                        printf(" }");
                    }
                    ml_free(&ml);
                }

                printf("}\n");
            }
            else
            {
                printf("%s\n", path);
                if(show_metadata)
                {
                    struct meta_list ml;
                    ml_init(&ml);
                    fetch_file_metadata(mountpoint, path, &ml);
                    for(uint32_t j = 0; j < ml.count; j++)
                        if(key_in_filter(ml.pairs[j].key, key_filter))
                            printf("  %s = %s\n", ml.pairs[j].key, ml.pairs[j].value);
                    ml_free(&ml);
                }
            }

            fflush(stdout);
            emitted++;
            if(max_results > 0 && emitted >= max_results) return 0;
        }

        offset += qa->count;
        if(offset >= qa->total) break;
    }

    return 0;
}

/**
 * Execute a query in batch mode: collect results and write to the
 * given output file (or stdout) in the specified format.
 *
 * @param fd      File descriptor on the OBMAFS3 mount.
 * @param qa      Parsed query argument.
 * @param format  "txt" or "json" (NULL defaults to "txt").
 * @param output  Output file path (NULL = stdout).
 * @return 0 on success, non-zero on error.
 */
static int execute_query_batch(int fd, struct obmafs3_ioctl_metadata_query_arg *qa,
                               const char *format, const char *output,
                               const char *mountpoint, int show_metadata,
                               const char *sort_key, int reverse, uint32_t max_results,
                               const char *key_filter)
{
    struct result_set rs;
    rs_init(&rs);

    if(execute_query_collect(fd, qa, &rs, max_results) != 0)
    {
        rs_free(&rs);
        return 1;
    }

    const char *fmt = format ? format : "txt";

    /* Sorting by a metadata key requires metadata to be fetched.
     * Table format also requires metadata. */
    int is_table  = (strcasecmp(fmt, "table") == 0 || strcasecmp(fmt, "csv") == 0);
    int need_meta = show_metadata || is_table || (sort_key && strcmp(sort_key, "path") != 0);
    if(need_meta && rs.count > 0)
        rs_fetch_metadata(mountpoint, &rs);

    if(sort_key && rs.count > 1)
        rs_sort(&rs, sort_key, reverse);

    if(output)
    {
        int rc;
        if(strcasecmp(fmt, "json") == 0)
            rc = export_json(&rs, output);
        else if(strcasecmp(fmt, "table") == 0)
            rc = export_table(&rs, output);
        else if(strcasecmp(fmt, "csv") == 0)
            rc = export_csv(&rs, output);
        else if(strcasecmp(fmt, "txt") == 0)
            rc = export_txt(&rs, output);
        else
        {
            fprintf(stderr, "Error: unknown format '%s' (use 'txt', 'json', 'table', or 'csv')\n", fmt);
            rs_free(&rs);
            return 1;
        }
        rs_free(&rs);
        return rc == 0 ? 0 : 1;
    }

    /* No output file — write to stdout */
    if(strcasecmp(fmt, "json") == 0)
    {
        printf("{\n  \"count\": %u,\n  \"results\": [", rs.count);
        for(uint32_t i = 0; i < rs.count; i++)
        {
            printf("%s\n    ", i ? "," : "");
            json_write_result(stdout, rs.paths[i], rs.metadata ? &rs.metadata[i] : NULL);
        }
        printf("\n  ]\n}\n");
    }
    else if(strcasecmp(fmt, "txt") == 0)
    {
        for(uint32_t i = 0; i < rs.count; i++)
        {
            printf("%s\n", rs.paths[i]);
            if(rs.metadata && rs.metadata[i].count > 0)
            {
                for(uint32_t j = 0; j < rs.metadata[i].count; j++)
                    if(key_in_filter(rs.metadata[i].pairs[j].key, key_filter))
                        printf("  %s = %s\n", rs.metadata[i].pairs[j].key, rs.metadata[i].pairs[j].value);
            }
        }
    }
    else if(strcasecmp(fmt, "table") == 0)
    {
        write_table(stdout, &rs);
    }
    else if(strcasecmp(fmt, "csv") == 0)
    {
        write_csv(stdout, &rs);
    }
    else
    {
        fprintf(stderr, "Error: unknown format '%s' (use 'txt', 'json', 'table', or 'csv')\n", fmt);
        rs_free(&rs);
        return 1;
    }

    rs_free(&rs);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Distinct values                                                    */
/* ------------------------------------------------------------------ */

/**
 * Execute a distinct values query for @p key via ioctl.
 * Paginates automatically and prints all unique values.
 *
 * @param fd      File descriptor on the OBMAFS3 mount.
 * @param key     Metadata key to enumerate.
 * @param format  "txt" or "json" (NULL defaults to "txt").
 * @param to_file Output file path (NULL = stdout).
 * @return 0 on success, non-zero on failure.
 */
static int execute_distinct(int fd, const char *key, const char *format, const char *to_file)
{
    /* Collect all distinct values via paginated ioctls */
    uint32_t  cap  = 64;
    uint32_t  n    = 0;
    char    **vals = malloc(cap * sizeof(char *));
    if(!vals)
    {
        fprintf(stderr, "Error: out of memory\n");
        return 1;
    }

    uint32_t offset = 0;
    uint32_t total  = 0;
    while(1)
    {
        struct obmafs3_ioctl_metadata_distinct_arg da;
        memset(&da, 0, sizeof(da));
        strncpy(da.key, key, METADATA_KEY_MAX - 1);
        da.offset = offset;

        if(ioctl(fd, OBMAFS3_IOC_DISTINCT_METADATA, &da) != 0)
        {
            fprintf(stderr, "Error: ioctl DISTINCT_METADATA failed: %s\n", strerror(errno));
            for(uint32_t i = 0; i < n; i++) free(vals[i]);
            free(vals);
            return 1;
        }

        total = da.total;
        if(da.count == 0) break;

        for(uint32_t i = 0; i < da.count; i++)
        {
            if(n >= cap)
            {
                cap *= 2;
                char **tmp = realloc(vals, cap * sizeof(char *));
                if(!tmp)
                {
                    for(uint32_t j = 0; j < n; j++) free(vals[j]);
                    free(vals);
                    fprintf(stderr, "Error: out of memory\n");
                    return 1;
                }
                vals = tmp;
            }
            vals[n] = strndup(da.values[i], METADATA_VALUE_MAX);
            n++;
        }

        offset += da.count;
        if(offset >= total) break;
    }

    /* Output */
    const char *fmt = format ? format : "txt";
    FILE       *fp  = stdout;
    if(to_file)
    {
        fp = fopen(to_file, "w");
        if(!fp)
        {
            fprintf(stderr, "Error: cannot open '%s': %s\n", to_file, strerror(errno));
            for(uint32_t i = 0; i < n; i++) free(vals[i]);
            free(vals);
            return 1;
        }
    }

    if(strcasecmp(fmt, "json") == 0)
    {
        fprintf(fp, "{\n  \"key\": \"");
        json_escape(fp, key);
        fprintf(fp, "\",\n  \"count\": %u,\n  \"values\": [", n);
        for(uint32_t i = 0; i < n; i++)
        {
            fprintf(fp, "%s\n    \"", i ? "," : "");
            json_escape(fp, vals[i]);
            fputc('"', fp);
        }
        fprintf(fp, "\n  ]\n}\n");
    }
    else
    {
        for(uint32_t i = 0; i < n; i++)
            fprintf(fp, "%s\n", vals[i]);
    }

    if(to_file)
    {
        fclose(fp);
        printf("Exported %u distinct value(s) to %s\n", n, to_file);
    }
    else if(strcasecmp(fmt, "txt") == 0)
    {
        printf("\n%u distinct value(s) for '%s'\n", n, key);
    }

    for(uint32_t i = 0; i < n; i++) free(vals[i]);
    free(vals);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  GROUP BY with count                                                */
/* ------------------------------------------------------------------ */

/**
 * Execute a GROUP BY query for @p key via ioctl.
 * Returns each distinct value with its file count.
 */
static int execute_groupby(int fd, const char *key, const char *format, const char *to_file)
{
    uint32_t  cap    = 64;
    uint32_t  n      = 0;
    char    **vals   = malloc(cap * sizeof(char *));
    uint32_t *cnts   = malloc(cap * sizeof(uint32_t));
    if(!vals || !cnts)
    {
        free(vals); free(cnts);
        fprintf(stderr, "Error: out of memory\n");
        return 1;
    }

    uint32_t offset = 0;
    uint32_t total  = 0;
    while(1)
    {
        struct obmafs3_ioctl_metadata_groupby_arg ga;
        memset(&ga, 0, sizeof(ga));
        strncpy(ga.key, key, METADATA_KEY_MAX - 1);
        ga.offset = offset;

        if(ioctl(fd, OBMAFS3_IOC_GROUPBY_METADATA, &ga) != 0)
        {
            fprintf(stderr, "Error: ioctl GROUPBY_METADATA failed: %s\n", strerror(errno));
            for(uint32_t i = 0; i < n; i++) free(vals[i]);
            free(vals); free(cnts);
            return 1;
        }

        total = ga.total;
        if(ga.count == 0) break;

        for(uint32_t i = 0; i < ga.count; i++)
        {
            if(n >= cap)
            {
                cap *= 2;
                char     **tv = realloc(vals, cap * sizeof(char *));
                uint32_t  *tc = realloc(cnts, cap * sizeof(uint32_t));
                if(!tv || !tc)
                {
                    for(uint32_t j = 0; j < n; j++) free(vals[j]);
                    free(vals); free(cnts);
                    fprintf(stderr, "Error: out of memory\n");
                    return 1;
                }
                vals = tv;
                cnts = tc;
            }
            vals[n] = strndup(ga.entries[i].value, METADATA_VALUE_MAX);
            cnts[n] = ga.entries[i].count;
            n++;
        }

        offset += ga.count;
        if(offset >= total) break;
    }

    /* Output */
    const char *fmt = format ? format : "txt";
    FILE       *fp  = stdout;
    if(to_file)
    {
        fp = fopen(to_file, "w");
        if(!fp)
        {
            fprintf(stderr, "Error: cannot open '%s': %s\n", to_file, strerror(errno));
            for(uint32_t i = 0; i < n; i++) free(vals[i]);
            free(vals); free(cnts);
            return 1;
        }
    }

    if(strcasecmp(fmt, "json") == 0)
    {
        fprintf(fp, "{\n  \"key\": \"");
        json_escape(fp, key);
        fprintf(fp, "\",\n  \"groups\": %u,\n  \"results\": [", n);
        for(uint32_t i = 0; i < n; i++)
        {
            fprintf(fp, "%s\n    {\"value\": \"", i ? "," : "");
            json_escape(fp, vals[i]);
            fprintf(fp, "\", \"count\": %u}", cnts[i]);
        }
        fprintf(fp, "\n  ]\n}\n");
    }
    else
    {
        /* Compute max value width for alignment */
        size_t max_vlen = 0;
        for(uint32_t i = 0; i < n; i++)
        {
            size_t vlen = strlen(vals[i]);
            if(vlen > max_vlen) max_vlen = vlen;
        }
        for(uint32_t i = 0; i < n; i++)
            fprintf(fp, "  %-*s  %u\n", (int)max_vlen, vals[i], cnts[i]);
    }

    if(to_file)
    {
        fclose(fp);
        printf("Exported %u group(s) to %s\n", n, to_file);
    }
    else if(strcasecmp(fmt, "txt") == 0)
    {
        printf("\n%u group(s) for '%s'\n", n, key);
    }

    for(uint32_t i = 0; i < n; i++) free(vals[i]);
    free(vals);
    free(cnts);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Numeric statistics                                                 */
/* ------------------------------------------------------------------ */

/**
 * Execute a stats query for @p key via ioctl.
 * Shows min, max, avg, sum, count.
 */
static int execute_stats(int fd, const char *key, const char *format)
{
    struct obmafs3_ioctl_metadata_stats_arg sa;
    memset(&sa, 0, sizeof(sa));
    strncpy(sa.key, key, METADATA_KEY_MAX - 1);

    if(ioctl(fd, OBMAFS3_IOC_STATS_METADATA, &sa) != 0)
    {
        fprintf(stderr, "Error: ioctl STATS_METADATA failed: %s\n", strerror(errno));
        return 1;
    }

    const char *fmt = format ? format : "txt";

    if(strcasecmp(fmt, "json") == 0)
    {
        printf("{\n  \"key\": \"");
        json_escape(stdout, key);
        printf("\",\n  \"count\": %u,\n", sa.count);
        printf("  \"min\": %" PRId64 ",\n", sa.min);
        printf("  \"max\": %" PRId64 ",\n", sa.max);
        printf("  \"sum\": %" PRId64 ",\n", sa.sum);
        if(sa.count > 0)
            printf("  \"avg\": %.2f\n", (double)sa.sum / (double)sa.count);
        else
            printf("  \"avg\": null\n");
        printf("}\n");
    }
    else
    {
        printf("\nStatistics for '%s':\n", key);
        printf("  Count: %u\n", sa.count);
        printf("  Min:   %" PRId64 "\n", sa.min);
        printf("  Max:   %" PRId64 "\n", sa.max);
        printf("  Sum:   %" PRId64 "\n", sa.sum);
        if(sa.count > 0)
            printf("  Avg:   %.2f\n", (double)sa.sum / (double)sa.count);
        printf("\n");
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
           "  distinct <key>     List all distinct values for a key\n"
           "  groupby <key>      Show distinct values with file counts\n"
           "  stats <key>        Show min/max/avg/sum for a numeric key\n"
           "  explain <query>    Show query plan with per-filter match counts\n"
           "  resort <key>       Re-sort cached results (e.g. resort N:year)\n"
           "  reverse            Toggle sort order and re-display\n"
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
           "  <key> ENDSWITH \"<value>\"           Suffix match\n"
           "  <key> IN \"<val1>,<val2>,...\"       Match any in list\n"
           "  <key> BETWEEN \"<low>\" \"<high>\"    Inclusive range (lexicographic)\n"
           "  <key> GLOB \"<pattern>\"             Wildcard match (*, ?, [abc])\n"
           "  <key> EXISTS                       Key exists (any value)\n"
           "\n"
           "Case-insensitive operators:\n"
           "  <key> I= \"<value>\"                 Equal (case-insensitive)\n"
           "  <key> I!= \"<value>\"                Not equal (case-insensitive)\n"
           "  <key> ICONTAINS \"<value>\"          Substring (case-insensitive)\n"
           "  <key> ISTARTSWITH \"<value>\"        Prefix (case-insensitive)\n"
           "  <key> IENDSWITH \"<value>\"          Suffix (case-insensitive)\n"
           "  <key> IIN \"<val1>,<val2>,...\"      IN match (case-insensitive)\n"
           "  <key> IGLOB \"<pattern>\"            Wildcard (case-insensitive)\n"
           "\n"
           "Numeric operators (values parsed as integers):\n"
           "  <key> N= \"<value>\"                 Numeric equal\n"
           "  <key> N!= \"<value>\"                Numeric not equal\n"
           "  <key> N> \"<value>\"                 Numeric greater than\n"
           "  <key> N< \"<value>\"                 Numeric less than\n"
           "  <key> N>= \"<value>\"                Numeric greater or equal\n"
           "  <key> N<= \"<value>\"                Numeric less or equal\n"
           "  <key> NBETWEEN \"<low>\" \"<high>\"   Inclusive numeric range\n"
           "\n"
           "Regex operators (POSIX extended regular expressions):\n"
           "  <key> REGEX \"<pattern>\"            Regex match (case-sensitive)\n"
           "  <key> IREGEX \"<pattern>\"           Regex match (case-insensitive)\n"
           "\n"
           "Negation:\n"
           "  NOT <key> = \"<value>\"               Invert a single filter\n"
           "  NOT <key> EXISTS                    Files without this key\n"
           "\n"
           "  Use * as the key to match across all keys:\n"
           "    * CONTAINS \"maiden\"               Any key's value contains\n"
           "    * ICONTAINS \"maiden\"              Any key (case-insensitive)\n"
           "    * = \"Rock\"                        Any key's value equals\n"
           "    * STARTSWITH \"Iron\"               Any key's value starts with\n"
           "\n"
           "  Multiple conditions joined with AND or OR:\n"
           "    artist I= \"iron maiden\" AND year N> \"1985\"\n"
           "    NOT genre = \"Pop\" AND year N> \"2000\"\n"
           "\n"
           "  Parentheses for mixed AND/OR (one group allowed):\n"
           "    (genre = \"Rock\" OR genre = \"Metal\") AND year N> \"1985\"\n"
           "    artist = \"Iron Maiden\" OR (year N>= \"1980\" AND year N<= \"1990\")\n"
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
    fprintf(stderr,
            "Usage: %s [options] <mountpoint>\n"
            "\n"
            "Options:\n"
            "  -q, --query <query>    Run a single query and exit\n"
            "  -c, --count            Count matching results only (no paths)\n"
            "  -l, --limit <N>        Return at most N results\n"
            "  -S, --stream           Stream results as they arrive (txt/jsonl only)\n"
            "  -d, --distinct <key>   List all distinct values for a metadata key\n"
            "  -g, --groupby <key>    Show distinct values with file counts\n"
            "  -t, --stats <key>      Show min/max/avg/sum for a numeric key\n"
            "  -f, --format <fmt>     Output format: txt, json, table, or csv\n"
            "  -o, --output <path>    Write results to file instead of stdout\n"
            "  -m, --metadata         Include all metadata for each result\n"
            "  -k, --keys <k1,k2,...>  Show only these metadata keys (implies -m)\n"
            "  -s, --sort <key>       Sort results by metadata key or 'path'\n"
            "                         Prefix with N: for numeric sort (e.g. N:year)\n"
            "  -r, --reverse          Reverse sort order (descending)\n"
            "  -h, --help             Show this help message\n"
            "\n"
            "Examples:\n"
            "  %s <mountpoint>                              Interactive mode\n"
            "  %s -q 'artist = \"Iron Maiden\"' /mnt/archive  Batch query\n"
            "  %s -q 'genre = \"Rock\"' -c /mnt/archive       Count only\n"
            "  %s -d genre /mnt/archive                      List all genres\n"
            "  %s -d genre -f json /mnt/archive              Distinct as JSON\n",
            prog, prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/*  Read-eval-print loop                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  REPL query cache                                                   */
/* ------------------------------------------------------------------ */

struct repl_cache
{
    char             query[4096];  /**< Last query string (empty = no cache) */
    struct result_set rs;          /**< Cached result set */
    int              has_metadata; /**< Whether metadata was fetched */
};

static void repl_cache_init(struct repl_cache *c)
{
    c->query[0]      = '\0';
    c->has_metadata  = 0;
    rs_init(&c->rs);
}

static void repl_cache_free(struct repl_cache *c)
{
    rs_free(&c->rs);
    c->query[0]     = '\0';
    c->has_metadata = 0;
}

/**
 * Check if a metadata key should be displayed.
 * @param key         The key to check.
 * @param key_filter  Comma-separated list of keys to show, or NULL to show all.
 * @return Non-zero if the key should be displayed.
 */
static int key_in_filter(const char *key, const char *key_filter)
{
    if(!key_filter) return 1; /* no filter = show all */
    const char *p = key_filter;
    size_t klen = strlen(key);
    while(*p)
    {
        const char *comma = p;
        while(*comma && *comma != ',') comma++;
        size_t ilen = (size_t)(comma - p);
        if(ilen == klen && strncmp(p, key, ilen) == 0) return 1;
        p = *comma ? comma + 1 : comma;
    }
    return 0;
}

/**
 * Display a result set to stdout, optionally with metadata.
 * @param key_filter  Comma-separated keys to show, or NULL for all.
 */
static void display_results(const struct result_set *rs, int show_metadata, const char *key_filter)
{
    for(uint32_t i = 0; i < rs->count; i++)
    {
        printf("  %s\n", rs->paths[i]);
        if(show_metadata && rs->metadata && rs->metadata[i].count > 0)
        {
            for(uint32_t j = 0; j < rs->metadata[i].count; j++)
                if(key_in_filter(rs->metadata[i].pairs[j].key, key_filter))
                    printf("    %s = %s\n", rs->metadata[i].pairs[j].key, rs->metadata[i].pairs[j].value);
        }
    }
    printf("\n%u result(s)\n", rs->count);
}

/* ------------------------------------------------------------------ */
/*  Read-eval-print loop                                               */
/* ------------------------------------------------------------------ */

/* History file in user's home directory */
#define HISTORY_FILE ".obmafs_query_history"
#define HISTORY_MAX  500

static void history_path(char *buf, size_t bufsz)
{
    const char *home = getenv("HOME");
    if(home)
        snprintf(buf, bufsz, "%s/%s", home, HISTORY_FILE);
    else
        buf[0] = '\0';
}

static void repl(const char *mountpoint, int query_fd, int show_metadata,
                 const char *sort_key, int reverse, const char *key_filter)
{
    struct repl_cache cache;
    repl_cache_init(&cache);

    /* Mutable copies of sort params so REPL commands can change them */
    char  current_sort[256] = {0};
    int   current_reverse   = reverse;
    if(sort_key) strncpy(current_sort, sort_key, sizeof(current_sort) - 1);

    /* Load persistent history */
    char histfile[4096];
    history_path(histfile, sizeof(histfile));
    using_history();
    stifle_history(HISTORY_MAX);
    if(histfile[0]) read_history(histfile);

    printf("obmafs-query: connected to %s\n", mountpoint);
    printf("Type 'help' for available commands, 'quit' to exit.\n\n");

    while(1)
    {
        char *line = readline(PROMPT);
        if(!line)
        {
            /* EOF (Ctrl-D) */
            printf("\n");
            break;
        }

        /* Skip leading whitespace */
        const char *cmd = line;
        while(*cmd == ' ' || *cmd == '\t') cmd++;

        /* Skip empty lines */
        if(*cmd == '\0') { free(line); continue; }

        /* Add non-empty lines to history */
        add_history(cmd);

        if(strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0) { free(line); break; }

        if(strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
        {
            print_help();
            free(line);
            continue;
        }

        /* distinct <key> — list distinct values for a metadata key */
        if(strncasecmp(cmd, "distinct ", 9) == 0 || strncasecmp(cmd, "distinct\t", 9) == 0)
        {
            const char *dkey = cmd + 9;
            while(*dkey == ' ' || *dkey == '\t') dkey++;
            if(*dkey == '\0')
                fprintf(stderr, "Error: expected a key name after 'distinct'\n");
            else
                execute_distinct(query_fd, dkey, NULL, NULL);
            free(line);
            continue;
        }

        /* groupby <key> — show distinct values with counts */
        if(strncasecmp(cmd, "groupby ", 8) == 0 || strncasecmp(cmd, "groupby\t", 8) == 0)
        {
            const char *gkey = cmd + 8;
            while(*gkey == ' ' || *gkey == '\t') gkey++;
            if(*gkey == '\0')
                fprintf(stderr, "Error: expected a key name after 'groupby'\n");
            else
                execute_groupby(query_fd, gkey, NULL, NULL);
            free(line);
            continue;
        }

        /* stats <key> — show numeric statistics */
        if(strncasecmp(cmd, "stats ", 6) == 0 || strncasecmp(cmd, "stats\t", 6) == 0)
        {
            const char *skey = cmd + 6;
            while(*skey == ' ' || *skey == '\t') skey++;
            if(*skey == '\0')
                fprintf(stderr, "Error: expected a key name after 'stats'\n");
            else
                execute_stats(query_fd, skey, NULL);
            free(line);
            continue;
        }

        /* explain <query> — show query plan with per-filter match counts */
        if(strncasecmp(cmd, "explain ", 8) == 0 || strncasecmp(cmd, "explain\t", 8) == 0)
        {
            const char *qstr = cmd + 8;
            while(*qstr == ' ' || *qstr == '\t') qstr++;
            if(*qstr == '\0')
            {
                fprintf(stderr, "Error: expected a query after 'explain'\n");
            }
            else
            {
                struct obmafs3_ioctl_metadata_query_arg eqa;
                if(parse_query(qstr, &eqa) == 0)
                    execute_explain(query_fd, &eqa);
            }
            free(line);
            continue;
        }

        /* resort <key> — re-display cached results with new sort key */
        if(strncasecmp(cmd, "resort ", 7) == 0 || strncasecmp(cmd, "resort\t", 7) == 0)
        {
            const char *skey = cmd + 7;
            while(*skey == ' ' || *skey == '\t') skey++;
            if(*skey == '\0')
            {
                fprintf(stderr, "Error: expected a sort key after 'resort'\n");
                continue;
            }
            if(cache.query[0] == '\0')
            {
                fprintf(stderr, "No cached query results to re-sort\n");
                continue;
            }

            /* Fetch metadata if needed for this sort key and not already fetched */
            if(!cache.has_metadata && strcmp(skey, "path") != 0 && cache.rs.count > 0)
            {
                rs_fetch_metadata(mountpoint, &cache.rs);
                cache.has_metadata = 1;
            }

            strncpy(current_sort, skey, sizeof(current_sort) - 1);
            current_sort[sizeof(current_sort) - 1] = '\0';
            if(cache.rs.count > 1) rs_sort(&cache.rs, current_sort, current_reverse);
            display_results(&cache.rs, show_metadata, key_filter);
            if(cache.rs.count > 0) offer_export(&cache.rs);
            free(line);
            continue;
        }

        /* reverse — toggle sort order and re-display */
        if(strcasecmp(cmd, "reverse") == 0)
        {
            if(cache.query[0] == '\0')
            {
                fprintf(stderr, "No cached query results to reverse\n");
                continue;
            }
            current_reverse = !current_reverse;
            if(current_sort[0] != '\0' && cache.rs.count > 1)
                rs_sort(&cache.rs, current_sort, current_reverse);
            printf("Sort order: %s\n", current_reverse ? "descending" : "ascending");
            display_results(&cache.rs, show_metadata, key_filter);
            if(cache.rs.count > 0) offer_export(&cache.rs);
            free(line);
            continue;
        }

        /* Parse query */
        struct obmafs3_ioctl_metadata_query_arg qa;
        if(parse_query(cmd, &qa) != 0) { free(line); continue; }

        /* Check if this query matches the cache */
        if(cache.query[0] != '\0' && strcmp(cmd, cache.query) == 0)
        {
            /* Same query — re-display cached results */
            printf("  (cached)\n");
            if(current_sort[0] != '\0' && cache.rs.count > 1)
                rs_sort(&cache.rs, current_sort, current_reverse);
            display_results(&cache.rs, show_metadata, key_filter);
            if(cache.rs.count > 0) offer_export(&cache.rs);
            free(line);
            continue;
        }

        /* New query — clear cache and execute */
        repl_cache_free(&cache);

        if(execute_query_collect(query_fd, &qa, &cache.rs, 0) != 0)
        {
            repl_cache_free(&cache);
            free(line);
            continue;
        }

        /* Fetch metadata if needed */
        int need_meta = show_metadata || (current_sort[0] != '\0' && strcmp(current_sort, "path") != 0);
        if(need_meta && cache.rs.count > 0)
        {
            rs_fetch_metadata(mountpoint, &cache.rs);
            cache.has_metadata = 1;
        }

        /* Sort if configured */
        if(current_sort[0] != '\0' && cache.rs.count > 1)
            rs_sort(&cache.rs, current_sort, current_reverse);

        /* Cache the query string */
        strncpy(cache.query, cmd, sizeof(cache.query) - 1);
        cache.query[sizeof(cache.query) - 1] = '\0';

        /* Display */
        display_results(&cache.rs, show_metadata, key_filter);
        if(cache.rs.count > 0) offer_export(&cache.rs);
        free(line);
    }

    /* Save history before exit */
    if(histfile[0]) write_history(histfile);

    repl_cache_free(&cache);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *query_str    = NULL;
    const char *format_str   = NULL;
    const char *output_str   = NULL;
    const char *sort_str     = NULL;
    const char *distinct_str = NULL;
    const char *groupby_str  = NULL;
    const char *stats_str    = NULL;
    int         show_meta    = 0;
    int         reverse      = 0;
    int         count_only   = 0;
    uint32_t    limit_n      = 0;
    const char *keys_str     = NULL;
    int         streaming    = 0;

    static struct option long_opts[] = {
        {"query",    required_argument, NULL, 'q'},
        {"format",   required_argument, NULL, 'f'},
        {"output",   required_argument, NULL, 'o'},
        {"metadata", no_argument,       NULL, 'm'},
        {"sort",     required_argument, NULL, 's'},
        {"reverse",  no_argument,       NULL, 'r'},
        {"count",    no_argument,       NULL, 'c'},
        {"distinct", required_argument, NULL, 'd'},
        {"groupby",  required_argument, NULL, 'g'},
        {"stats",    required_argument, NULL, 't'},
        {"limit",    required_argument, NULL, 'l'},
        {"keys",     required_argument, NULL, 'k'},
        {"stream",   no_argument,       NULL, 'S'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while((opt = getopt_long(argc, argv, "q:f:o:ms:rcd:g:t:l:k:Sh", long_opts, NULL)) != -1)
    {
        switch(opt)
        {
            case 'q': query_str    = optarg; break;
            case 'f': format_str   = optarg; break;
            case 'o': output_str   = optarg; break;
            case 'm': show_meta    = 1;      break;
            case 's': sort_str     = optarg; break;
            case 'r': reverse      = 1;      break;
            case 'c': count_only   = 1;      break;
            case 'd': distinct_str = optarg; break;
            case 'g': groupby_str  = optarg; break;
            case 't': stats_str    = optarg; break;
            case 'l': limit_n      = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'k': keys_str     = optarg; show_meta = 1; break;
            case 'S': streaming    = 1;      break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if(optind >= argc)
    {
        fprintf(stderr, "Error: missing mountpoint argument\n");
        usage(argv[0]);
        return 1;
    }

    const char *mountpoint = argv[optind];

    if(validate_mountpoint(mountpoint) != 0) return 1;

    int query_fd = open_sentinel(mountpoint);
    if(query_fd < 0) return 1;

    int rc = 0;

    if(distinct_str)
    {
        /* Distinct values mode */
        rc = execute_distinct(query_fd, distinct_str, format_str, output_str);
    }
    else if(groupby_str)
    {
        /* GROUP BY mode */
        rc = execute_groupby(query_fd, groupby_str, format_str, output_str);
    }
    else if(stats_str)
    {
        /* Stats mode */
        rc = execute_stats(query_fd, stats_str, format_str);
    }
    else if(query_str)
    {
        /* Batch mode: execute a single query and exit */
        struct obmafs3_ioctl_metadata_query_arg qa;
        if(parse_query(query_str, &qa) != 0)
        {
            close(query_fd);
            return 1;
        }

        if(count_only)
        {
            uint32_t total = execute_query_count(query_fd, &qa);
            if(total == (uint32_t)-1)
                rc = 1;
            else
                printf("%u\n", total);
        }
        else if(streaming)
        {
            if(sort_str)
                fprintf(stderr, "Warning: --sort is ignored in streaming mode\n");
            if(output_str)
                fprintf(stderr, "Warning: --output is ignored in streaming mode (use shell redirection)\n");
            rc = execute_query_stream(query_fd, &qa, format_str, mountpoint, show_meta, limit_n, keys_str);
        }
        else
        {
            rc = execute_query_batch(query_fd, &qa, format_str, output_str, mountpoint, show_meta, sort_str, reverse,
                                     limit_n, keys_str);
        }
    }
    else
    {
        /* Interactive REPL mode */
        if(format_str || output_str)
            fprintf(stderr, "Warning: --format and --output are ignored in interactive mode\n");
        if(count_only)
            fprintf(stderr, "Warning: --count is ignored in interactive mode (use batch mode with -q)\n");
        repl(mountpoint, query_fd, show_meta, sort_str, reverse, keys_str);
    }

    close(query_fd);
    return rc;
}
