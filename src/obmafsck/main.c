/*
 * obmafsck - Check and validate an OBMAFS3 filesystem
 */
#include "obmafs.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

/* ------------------------------------------------------------------ */
/*  Terminal colour & symbol helpers                                    */
/* ------------------------------------------------------------------ */

static int g_use_color = 0; /* set to 1 when stderr+stdout are ttys */

/* ANSI SGR codes — only emitted when g_use_color is set */
#define CLR_RESET     (g_use_color ? "\033[0m"    : "")
#define CLR_BOLD      (g_use_color ? "\033[1m"    : "")
#define CLR_DIM       (g_use_color ? "\033[2m"    : "")
#define CLR_RED       (g_use_color ? "\033[31m"   : "")
#define CLR_GREEN     (g_use_color ? "\033[32m"   : "")
#define CLR_YELLOW    (g_use_color ? "\033[33m"   : "")
#define CLR_BLUE      (g_use_color ? "\033[34m"   : "")
#define CLR_CYAN      (g_use_color ? "\033[36m"   : "")
#define CLR_BOLD_RED  (g_use_color ? "\033[1;31m" : "")
#define CLR_BOLD_GRN  (g_use_color ? "\033[1;32m" : "")
#define CLR_BOLD_YLW  (g_use_color ? "\033[1;33m" : "")
#define CLR_BOLD_CYAN (g_use_color ? "\033[1;36m" : "")

/* Unicode status symbols (with colour) for structured output */
#define SYM_OK      (g_use_color ? "\033[32m\u2714\033[0m" : "OK")      /* ✔ green  */
#define SYM_BAD     (g_use_color ? "\033[31m\u2718\033[0m" : "BAD")     /* ✘ red    */
#define SYM_WARN    (g_use_color ? "\033[33m\u26A0\033[0m" : "WARNING") /* ⚠ yellow */
#define SYM_FIXED   (g_use_color ? "\033[34m\u2726\033[0m" : "FIXED")   /* ✦ blue   */
#define SYM_SKIP    (g_use_color ? "\033[2m\u2500\033[0m"  : "-")       /* ─ dim    */

/* Initialise colour support (call once early in main) */
static void init_color(void)
{
    /* Respect NO_COLOR convention (https://no-color.org/) */
    if(getenv("NO_COLOR")) return;

    /* Only enable when both stdout and stderr are terminals */
    if(isatty(STDOUT_FILENO) && isatty(STDERR_FILENO))
        g_use_color = 1;
}

/* ------------------------------------------------------------------ */
/*  Timing helpers                                                     */
/* ------------------------------------------------------------------ */

static struct timespec g_start_time; /* set at programme start   */
static int             g_phase_num;  /* current phase number     */

/** Store the current monotonic time in @p ts. */
static void timer_now(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }

/** Return elapsed seconds between @p start and @p end. */
static double timer_elapsed(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) + (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

/** Format a duration in seconds to a human-readable string. */
static void fmt_duration(double secs, char *buf, size_t len)
{
    if(secs < 0.001)
        snprintf(buf, len, "<1ms");
    else if(secs < 1.0)
        snprintf(buf, len, "%.0fms", secs * 1000.0);
    else if(secs < 60.0)
        snprintf(buf, len, "%.1fs", secs);
    else
    {
        int m = (int)(secs / 60.0);
        snprintf(buf, len, "%dm%04.1fs", m, secs - m * 60.0);
    }
}

/* ------------------------------------------------------------------ */
/*  Phase header helper                                                */
/* ------------------------------------------------------------------ */

static struct timespec g_phase_start;

/**
 * Print a numbered phase header.
 *
 * Output: "Phase N: Title"  (bold when colour is enabled)
 */
static void phase_begin(const char *title)
{
    g_phase_num++;
    timer_now(&g_phase_start);
    printf("\n%s── Phase %d: %s%s\n", CLR_BOLD, g_phase_num, title, CLR_RESET);
}

/**
 * Print the elapsed time for the current phase (right-aligned, dim).
 */
static void phase_end(void)
{
    struct timespec now;
    timer_now(&now);
    char dur[32];
    fmt_duration(timer_elapsed(&g_phase_start, &now), dur, sizeof(dur));
    printf("  %s(%s)%s\n", CLR_DIM, dur, CLR_RESET);
}

/* ------------------------------------------------------------------ */
/*  Formatted-result helpers                                           */
/* ------------------------------------------------------------------ */

/**
 * Print a successful check result:  "  Label:  ✔ value"
 */
static void result_ok(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result_ok(const char *label, const char *fmt, ...)
{
    printf("  %-20s %s ", label, SYM_OK);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * Print a failed check result:  "  Label:  ✘ value"
 */
static void result_bad(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result_bad(const char *label, const char *fmt, ...)
{
    printf("  %-20s %s ", label, SYM_BAD);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * Print a fixed/repaired result:  "  Label:  ✦ value"
 */
static void result_fixed(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result_fixed(const char *label, const char *fmt, ...)
{
    printf("  %-20s %s ", label, SYM_FIXED);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * Print a simple labelled value (no status symbol).
 */
static void result_info(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result_info(const char *label, const char *fmt, ...)
{
    printf("  %-20s ", label);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/*  Options                                                            */
/* ------------------------------------------------------------------ */

/**
 * Print usage information for obmafsck.
 *
 * @param prog  Program name to display in the usage line.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <device-or-file>\n"
            "\n"
            "Options:\n"
            "  -y              Assume 'yes' to all repair questions\n"
            "  -n              Assume 'no' to all repair questions\n"
            "  -s, --scrub     Verify checksums of all data blocks\n"
            "  -d, --dedup-stats  Show deduplication and compression statistics\n"
            "  -D, --dedup-stats-only  Show dedup stats without integrity checks\n"
            "  -v, --verify-hashes  Verify dedup and CD hashes against stored data\n"
            "  -h, --help      Show this help message\n",
            prog);
}

/* Return value: 1 = yes, 0 = no */
/**
 * Prompt the user to fix a problem, or decide automatically.
 *
 * @param auto_yes  If non-zero, always return 1 (yes).
 * @param auto_no   If non-zero, always return 0 (no).
 * @param prompt    Question text displayed to the user.
 * @return 1 if the fix should be applied, 0 otherwise.
 */
static int ask_fix(int auto_yes, int auto_no, const char *prompt)
{
    if(auto_yes) return 1;
    if(auto_no) return 0;

    printf("  %s%s%s [y/n] ", CLR_BOLD_YLW, prompt, CLR_RESET);
    fflush(stdout);

    int ch = fgetc(stdin);
    /* consume rest of line */
    int c2;
    while((c2 = fgetc(stdin)) != '\n' && c2 != EOF);
    return (ch == 'y' || ch == 'Y');
}

/* ------------------------------------------------------------------ */
/*  Superblock field range checks                                      */
/* ------------------------------------------------------------------ */

/** Return 1 if @p v is a power of two. */
static int is_power_of_two(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

/**
 * Validate superblock field ranges and internal consistency.
 *
 * Checks block_size, dedup_block_size, total_bytes, checksum_type,
 * next_inode_id, bitmap parameters, LBA fields (in-range, unique),
 * volume_label NUL-termination, and creation_time plausibility.
 *
 * When a fixable mismatch is detected the user is prompted (unless
 * auto_yes / auto_no is set).  Fixes are written back via the
 * superblock write path.
 *
 * @param sb          Pointer to the in-memory superblock (modified on fix).
 * @param fd          File descriptor to write fixes.
 * @param file_size   Actual size of the backing file/device (from fstat).
 * @param auto_yes    If non-zero, always repair.
 * @param auto_no     If non-zero, never repair.
 * @param errors      In/out: incremented for each unfixed error.
 */
static void validate_superblock_fields(struct obmafs3_sb *sb, int fd, uint64_t file_size,
                                       int auto_yes, int auto_no, int *errors)
{
    int bad = 0, fixed = 0;
    uint64_t total_blocks = sb->total_bytes / sb->block_size;

    /* ---- block_size ---- */
    if(!is_power_of_two(sb->block_size))
    {
        printf("    block_size %" PRIu64 " is not a power of 2\n", sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set block_size to 4096?"))
        {
            sb->block_size = 4096;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }
    else if(sb->block_size < 4096)
    {
        printf("    block_size %" PRIu64 " is below minimum (4096)\n", sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set block_size to 4096?"))
        {
            sb->block_size = 4096;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }

    /* ---- dedup_block_size ---- */
    if(!is_power_of_two(sb->dedup_block_size))
    {
        printf("    dedup_block_size %" PRIu64 " is not a power of 2\n", sb->dedup_block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set dedup_block_size to 4194304?"))
        {
            sb->dedup_block_size = 4194304;
            fixed++;
        }
    }
    else if(sb->dedup_block_size < sb->block_size)
    {
        printf("    dedup_block_size %" PRIu64 " is smaller than block_size %" PRIu64 "\n",
               sb->dedup_block_size, sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set dedup_block_size to 4194304?"))
        {
            sb->dedup_block_size = 4194304;
            fixed++;
        }
    }
    else if(sb->dedup_block_size % sb->block_size != 0)
    {
        printf("    dedup_block_size %" PRIu64 " is not a multiple of block_size %" PRIu64 "\n",
               sb->dedup_block_size, sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set dedup_block_size to 4194304?"))
        {
            sb->dedup_block_size = 4194304;
            fixed++;
        }
    }

    /* ---- total_bytes ---- */
    if(sb->total_bytes % sb->block_size != 0)
    {
        printf("    total_bytes %" PRIu64 " is not a multiple of block_size %" PRIu64 "\n",
               sb->total_bytes, sb->block_size);
        bad++;
        uint64_t aligned = (sb->total_bytes / sb->block_size) * sb->block_size;
        printf("    (nearest aligned value: %" PRIu64 ")\n", aligned);
        if(ask_fix(auto_yes, auto_no, "    Round total_bytes down to block boundary?"))
        {
            sb->total_bytes = aligned;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }

    if(file_size > 0 && sb->total_bytes != file_size)
    {
        printf("    total_bytes %" PRIu64 " does not match actual file size %" PRIu64 "\n",
               sb->total_bytes, file_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set total_bytes to match file size?"))
        {
            sb->total_bytes = file_size;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }

    /* ---- checksum_type ---- */
    if(sb->checksum_type != kChecksumTypeXXH64)
    {
        printf("    checksum_type %" PRIu16 " is not supported (expected %d)\n",
               sb->checksum_type, kChecksumTypeXXH64);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set checksum_type to XXH64 (0)?"))
        {
            sb->checksum_type = kChecksumTypeXXH64;
            fixed++;
        }
    }

    /* ---- next_inode_id ---- */
    if(sb->next_inode_id < 3)
    {
        printf("    next_inode_id %" PRIu64 " is below minimum (3)\n", sb->next_inode_id);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set next_inode_id to 3?"))
        {
            sb->next_inode_id = 3;
            fixed++;
        }
    }

    /* ---- bitmap_lba ---- */
    if(sb->bitmap_lba == 0)
    {
        printf("    bitmap_lba is 0 (no allocation bitmap)\n");
        bad++;
    }
    else if(sb->bitmap_lba >= total_blocks)
    {
        printf("    bitmap_lba %" PRIu64 " is beyond total blocks %" PRIu64 "\n",
               sb->bitmap_lba, total_blocks);
        bad++;
    }

    /* ---- bitmap_blocks ---- */
    if(sb->bitmap_lba > 0 && sb->bitmap_blocks > 0)
    {
        uint64_t bitmap_bytes         = (total_blocks + 7) / 8;
        size_t   hdr_size             = sizeof(struct bitmap_header);
        uint64_t first_block_capacity = sb->block_size - hdr_size;
        uint64_t expected_bitmap_blks;
        if(bitmap_bytes <= first_block_capacity)
            expected_bitmap_blks = 1;
        else
            expected_bitmap_blks = 1 + (bitmap_bytes - first_block_capacity + sb->block_size - 1) / sb->block_size;

        if(sb->bitmap_blocks != expected_bitmap_blks)
        {
            printf("    bitmap_blocks %" PRIu64 " does not match expected %" PRIu64 "\n",
                   sb->bitmap_blocks, expected_bitmap_blks);
            bad++;
            if(ask_fix(auto_yes, auto_no, "    Fix bitmap_blocks?"))
            {
                sb->bitmap_blocks = expected_bitmap_blks;
                fixed++;
            }
        }

        if(sb->bitmap_lba + sb->bitmap_blocks > total_blocks)
        {
            printf("    bitmap extends beyond filesystem (LBA %" PRIu64 " + %" PRIu64 " blocks > %" PRIu64 ")\n",
                   sb->bitmap_lba, sb->bitmap_blocks, total_blocks);
            bad++;
        }
    }

    /* ---- keyset_lba / keyset_blocks ---- */
    if(sb->keyset_lba != 0)
    {
        if(sb->keyset_lba >= total_blocks)
        {
            printf("    keyset_lba %" PRIu64 " is beyond total blocks %" PRIu64 "\n",
                   sb->keyset_lba, total_blocks);
            bad++;
        }
        if(sb->keyset_blocks == 0)
        {
            printf("    keyset_lba is set but keyset_blocks is 0\n");
            bad++;
        }
        else if(sb->keyset_lba + sb->keyset_blocks > total_blocks)
        {
            printf("    keyset extends beyond filesystem (LBA %" PRIu64 " + %" PRIu64 " blocks > %" PRIu64 ")\n",
                   sb->keyset_lba, sb->keyset_blocks, total_blocks);
            bad++;
        }
    }

    /* ---- LBA range checks ---- */
    struct { const char *name; uint64_t lba; } lba_fields[] = {
        { "catalog_lba",       sb->catalog_lba       },
        { "inode_lba",         sb->inode_lba         },
        { "overflow_lba",      sb->overflow_lba      },
        { "dedup_lba",         sb->dedup_lba         },
        { "metadata_lba",      sb->metadata_lba      },
        { "media_tag_lba",     sb->media_tag_lba     },
        { "cd_prefix_lba",     sb->cd_prefix_lba     },
        { "cd_suffix_lba",     sb->cd_suffix_lba     },
        { "cd_subchannel_lba", sb->cd_subchannel_lba },
        { "metadata_idx_lba",  sb->metadata_idx_lba  },
        { "refcount_lba",      sb->refcount_lba      },
    };
    int lba_count = (int)(sizeof(lba_fields) / sizeof(lba_fields[0]));

    for(int i = 0; i < lba_count; i++)
    {
        if(lba_fields[i].lba == 0) continue; /* optional field */
        if(lba_fields[i].lba >= total_blocks)
        {
            printf("    %s %" PRIu64 " is beyond total blocks %" PRIu64 "\n",
                   lba_fields[i].name, lba_fields[i].lba, total_blocks);
            bad++;
        }
    }

    /* ---- LBA uniqueness ---- */
    for(int i = 0; i < lba_count; i++)
    {
        if(lba_fields[i].lba == 0) continue;
        for(int j = i + 1; j < lba_count; j++)
        {
            if(lba_fields[j].lba == 0) continue;
            if(lba_fields[i].lba == lba_fields[j].lba)
            {
                printf("    %s and %s share the same LBA %" PRIu64 "\n",
                       lba_fields[i].name, lba_fields[j].name, lba_fields[i].lba);
                bad++;
            }
        }
    }

    /* ---- volume_label NUL-termination ---- */
    {
        int has_nul = 0;
        for(size_t i = 0; i < sizeof(sb->volume_label); i++)
        {
            if(sb->volume_label[i] == '\0')
            {
                has_nul = 1;
                break;
            }
        }
        if(!has_nul)
        {
            printf("    volume_label is not NUL-terminated\n");
            bad++;
            if(ask_fix(auto_yes, auto_no, "    NUL-terminate volume_label?"))
            {
                sb->volume_label[sizeof(sb->volume_label) - 1] = '\0';
                fixed++;
            }
        }
    }

    /* ---- creation_time ---- */
    if(sb->creation_time == 0)
    {
        printf("    creation_time is 0 (not set)\n");
        bad++;
    }
    else
    {
        uint64_t now = (uint64_t)time(NULL);
        if(sb->creation_time > now)
        {
            printf("    creation_time %" PRIu64 " is in the future (now %" PRIu64 ")\n",
                   sb->creation_time, now);
            bad++;
        }
    }

    /* ---- Write fixes if any ---- */
    if(fixed > 0)
    {
        /* Recompute superblock checksum before writing */
        memset(sb->checksum, 0, sizeof(sb->checksum));
        obmafs3_checksum_block(sb, sizeof(*sb), sb->checksum);

        ssize_t n = pwrite(fd, sb, sizeof(*sb), 0);
        if(n < 0 || (size_t)n != sizeof(*sb))
            fprintf(stderr, "    Error: could not write superblock fix\n");
        else
        {
            printf("    Superblock updated (%d field(s) fixed).\n", fixed);
            /* Also update the backup superblock */
            if(sb->total_bytes > 0 && sb->block_size > 0)
            {
                uint64_t blba = OBMAFS3_BACKUP_SB_LBA(sb->total_bytes, sb->block_size);
                if(blba > 0)
                    pwrite(fd, sb, sizeof(*sb), (off_t)(blba * sb->block_size));
            }
        }
    }

    /* ---- Summary ---- */
    if(bad == 0)
    {
        result_ok("Field checks:", "");
    }
    else
    {
        if(fixed > 0)
            result_fixed("Field checks:", "%d error(s), %d fixed", bad, fixed);
        else
            result_bad("Field checks:", "%d error(s)", bad);
        *errors += (bad - fixed);
    }
}

/* ------------------------------------------------------------------ */
/*  Progress bar helper                                                */
/* ------------------------------------------------------------------ */

/** Clear the current progress line on stderr. */
static void bar_clear(void)
{
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
}

/**
 * Print a unified progress bar on stderr.
 *
 * Renders:
 *   \r  Walking Dedup tree  ████████░░░░░░░  53% · 768/1450
 *
 * Uses Unicode block-drawing characters when colour is enabled,
 * falling back to [===>   ] when it is not.
 *
 * @param prefix  Label text (e.g. "Walking Dedup tree").
 * @param done    Number of items completed.
 * @param total   Total number of items.
 */
static void print_bar(const char *prefix, uint64_t done, uint64_t total)
{
    const int bar_width = 30;
    double    frac      = total > 0 ? (double)done / (double)total : 1.0;
    if(frac > 1.0) frac = 1.0;
    int pct = (int)(frac * 100.0);

    if(g_use_color)
    {
        /* Unicode block-drawing progress bar */
        /* Each cell can show 8 sub-positions via block chars ▏▎▍▌▋▊▉█ */
        static const char *blocks[] = { " ", "\u258F", "\u258E", "\u258D",
                                        "\u258C", "\u258B", "\u258A", "\u2589", "\u2588" };
        double filled_f = frac * bar_width;
        int    filled_i = (int)filled_f;
        int    sub      = (int)((filled_f - filled_i) * 8.0);

        fprintf(stderr, "\r  \033[36m%-24s\033[0m ", prefix);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled_i)
                fprintf(stderr, "\033[36m\u2588\033[0m");
            else if(i == filled_i)
                fprintf(stderr, "\033[36m%s\033[0m", blocks[sub]);
            else
                fprintf(stderr, "\033[2m\u2591\033[0m");
        }
        fprintf(stderr, " %3d%% \033[2m\u00B7\033[0m %" PRIu64 "/%" PRIu64 "  ", pct, done, total);
    }
    else
    {
        /* ASCII fallback */
        int filled = (int)(frac * bar_width);
        fprintf(stderr, "\r  %-24s [", prefix);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled)       fputc('=', stderr);
            else if(i == filled) fputc('>', stderr);
            else                 fputc(' ', stderr);
        }
        fprintf(stderr, "] %3d%% %" PRIu64 "/%" PRIu64 "   ", pct, done, total);
    }
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/*  Walk all nodes in a single-block B+Tree (DFS)                      */
/*  index_entry_size / child_lba_off parameterize the index entry.     */
/* ------------------------------------------------------------------ */

/**
 * Walk all nodes in a single-block B+Tree via iterative DFS.
 *
 * Collects the LBA of every node (both index and leaf) reachable
 * from @p root_lba.  The caller provides the index entry size and the
 * byte offset of the @c child_lba field to correctly parse index nodes
 * of different tree types.
 *
 * @param ctx              Filesystem context.
 * @param root_lba         Root node LBA of the tree.
 * @param index_entry_size Size in bytes of each index entry.
 * @param child_lba_off    Byte offset of the @c child_lba field in
 *                         the index entry structure.
 * @param out_lbas         Output: heap-allocated array of node LBAs.
 * @param out_count        Output: number of elements in @p out_lbas.
 * @param total_nodes      Expected total nodes (for progress; 0 to disable).
 * @param label            Tree label for progress display (NULL to disable).
 * @return @c OBMAFS3_OK on success.
 */
static int walk_inode_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                                  size_t child_lba_off, uint64_t **out_lbas, uint64_t *out_count,
                                  uint32_t total_nodes, const char *label)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(root_lba == 0) return OBMAFS3_OK;

    int show_progress = (total_nodes > 10 && label != NULL);

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Iterative DFS via explicit stack */
    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    if(show_progress) fflush(stdout); /* ensure prior output appears before progress */

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        /* Grow output array */
        if(count >= cap)
        {
            cap           = cap == 0 ? 64 : cap * 2;
            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
            if(!tmp)
            {
                free(buf);
                free(stack);
                free(lbas);
                return OBMAFS3_ERR_NOMEM;
            }
            lbas = tmp;
        }
        lbas[count++] = lba;

        if(show_progress && (count <= 1 || (count & 0xFF) == 0 || count == (uint64_t)total_nodes))
        {
            char pfx[64];
            snprintf(pfx, sizeof(pfx), "Walking %s tree", label);
            print_bar(pfx, count, (uint64_t)total_nodes);
        }

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(hdr.level > 0)
        {
            /* Index node: push children onto stack */
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                uint64_t child_lba;
                memcpy(&child_lba,
                       buf + sizeof(struct btree_node_header) + (size_t)i * index_entry_size + child_lba_off,
                       sizeof(child_lba));

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = child_lba;
            }
        }
    }

    free(buf);
    free(stack);

    if(show_progress)
    {
        bar_clear();
    }

    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Walk all nodes in the catalog B+Tree (DFS)                         */
/*  Uses catalog_index_entry instead of btree_index_entry.             */
/* ------------------------------------------------------------------ */

/**
 * Walk all nodes in the catalog B+Tree via iterative DFS.
 *
 * Uses @c catalog_index_entry (rather than @c btree_index_entry) to
 * decode index node children.
 *
 * @param ctx        Filesystem context.
 * @param root_lba   Root node LBA of the catalog tree.
 * @param out_lbas   Output: heap-allocated array of node LBAs.
 * @param out_count  Output: number of elements in @p out_lbas.
 * @return @c OBMAFS3_OK on success.
 */
static int walk_catalog_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, uint64_t **out_lbas,
                                    uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(root_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Iterative DFS via explicit stack */
    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        /* Grow output array */
        if(count >= cap)
        {
            cap           = cap == 0 ? 64 : cap * 2;
            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
            if(!tmp)
            {
                free(buf);
                free(stack);
                free(lbas);
                return OBMAFS3_ERR_NOMEM;
            }
            lbas = tmp;
        }
        lbas[count++] = lba;

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(hdr.level > 0)
        {
            /* Index node: push children (catalog_index_entry) */
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct catalog_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Verify checksums for a list of B+Tree node LBAs                    */
/* ------------------------------------------------------------------ */

/**
 * Verify stored checksums of a list of B+Tree node blocks.
 *
 * Reads each node, recomputes its checksum, and compares it with the
 * stored value.  Reports mismatches to stderr.  Optionally rewrites
 * nodes with corrected checksums.
 *
 * @param ctx          Filesystem context.
 * @param node_lbas    Array of node LBAs to verify.
 * @param node_count   Number of elements in @p node_lbas.
 * @param tree_name    Human-readable tree name for diagnostic output.
 * @param auto_yes     If non-zero, always repair without asking.
 * @param auto_no      If non-zero, never repair.
 * @param bad_count    Output: number of nodes with bad checksums.
 * @param fixed_count  Output: number of nodes whose checksums were repaired.
 * @return @c OBMAFS3_OK on success.
 */
static int verify_btree_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                       const char *tree_name, int auto_yes, int auto_no, uint64_t *bad_count,
                                       uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /* Collect LBAs of nodes with bad checksums */
    uint64_t *bad_lbas = NULL;
    uint64_t  bad_cap  = 0;
    uint64_t  bad      = 0;

    for(uint64_t n = 0; n < node_count; n++)
    {
        if(node_count > 10)
        {
            char pfx[64];
            snprintf(pfx, sizeof(pfx), "Verifying %s nodes", tree_name);
            print_bar(pfx, n + 1, node_count);
        }

        uint64_t lba = node_lbas[n];

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(bad_lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": bad magic\n", tree_name, lba);
            bad++;
            continue;
        }

        size_t  data_size = sizeof(struct btree_node_header) + hdr.keys_length;
        uint8_t stored[32];
        memcpy(stored, hdr.checksum, 32);
        memset(buf + __builtin_offsetof(struct btree_node_header, checksum), 0, 32);
        uint8_t computed[32];
        obmafs3_checksum_block(buf, data_size, computed);
        memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), stored, 32);

        if(memcmp(stored, computed, 32) != 0)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": checksum mismatch\n", tree_name, lba);
            if(bad >= bad_cap)
            {
                bad_cap = bad_cap ? bad_cap * 2 : 16;
                uint64_t *tmp = realloc(bad_lbas, bad_cap * sizeof(uint64_t));
                if(!tmp) { free(buf); free(bad_lbas); return OBMAFS3_ERR_NOMEM; }
                bad_lbas = tmp;
            }
            bad_lbas[bad] = lba;
            bad++;
        }
    }

    if(node_count > 10)
    {
        bar_clear();
    }

    /* Offer to fix */
    uint64_t fixes = 0;
    if(bad > 0)
    {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "  Fix %" PRIu64 " %s node checksum%s?", bad, tree_name,
                 bad == 1 ? "" : "s");
        if(ask_fix(auto_yes, auto_no, prompt))
        {
            for(uint64_t i = 0; i < bad; i++)
            {
                int rc = obmafs3_block_read(ctx, bad_lbas[i], buf, (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK) continue;

                struct btree_node_header hdr;
                memcpy(&hdr, buf, sizeof(hdr));
                size_t data_size = sizeof(struct btree_node_header) + hdr.keys_length;
                memset(hdr.checksum, 0, 32);
                memcpy(buf, &hdr, sizeof(hdr));
                obmafs3_checksum_block(buf, data_size, hdr.checksum);
                memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr.checksum, 32);

                rc = obmafs3_block_write(ctx, bad_lbas[i], buf, (size_t)ctx->sb.block_size);
                if(rc == OBMAFS3_OK)
                    fixes++;
                else
                    fprintf(stderr, "    Error writing LBA %" PRIu64 ": %d\n", bad_lbas[i], rc);
            }
        }
    }

    free(buf);
    free(bad_lbas);
    *bad_count   = bad;
    *fixed_count = fixes;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  B+Tree key ordering validation                                     */
/* ------------------------------------------------------------------ */

/** Ordering tree-type identifiers for verify_btree_ordering(). */
#define ORD_UINT64_KEY  0  /**< key = first uint64_t (inode, dedup, cd_*, refcount) */
#define ORD_CATALOG     1  /**< key = (parent_id, name) */
#define ORD_OVERFLOW    2  /**< leaf: (inode_id, logical_offset), index: uint64_t */
#define ORD_MEDIA_TAG   3  /**< key = (inode_id, tag_type) */
#define ORD_METADATA    4  /**< key = (inode_id, key[256]) */
#define ORD_METADATA_IDX 5 /**< key = (key[256], value[1025], inode_id) */

/**
 * Compare two B+Tree keys extracted from raw record/entry bytes.
 *
 * @param a       Pointer to first record or index entry.
 * @param b       Pointer to second record or index entry.
 * @param type    One of the ORD_* tree-type constants.
 * @param is_leaf Non-zero for leaf records, zero for index entries.
 * @return Negative if a < b, 0 if equal, positive if a > b.
 */
static int ordering_key_cmp(const uint8_t *a, const uint8_t *b, int type, int is_leaf)
{
    uint64_t ua, ub;

    switch(type)
    {
    case ORD_UINT64_KEY:
        memcpy(&ua, a, 8);
        memcpy(&ub, b, 8);
        return (ua < ub) ? -1 : (ua > ub) ? 1 : 0;

    case ORD_CATALOG:
        if(is_leaf)
        {
            /* catalog_record: inode_id(8), parent_id(8), directory_flag(1), name[256] */
            uint64_t pid_a, pid_b;
            memcpy(&pid_a, a + 8, 8);
            memcpy(&pid_b, b + 8, 8);
            if(pid_a != pid_b) return (pid_a < pid_b) ? -1 : 1;
            return strcmp((const char *)(a + 17), (const char *)(b + 17));
        }
        else
        {
            /* catalog_index_entry: parent_id(8), name[256], child_lba(8) */
            uint64_t pid_a, pid_b;
            memcpy(&pid_a, a, 8);
            memcpy(&pid_b, b, 8);
            if(pid_a != pid_b) return (pid_a < pid_b) ? -1 : 1;
            return strcmp((const char *)(a + 8), (const char *)(b + 8));
        }

    case ORD_OVERFLOW:
        if(is_leaf)
        {
            /* overflow_extent: inode_id(8), logical_offset(8), ... */
            uint64_t id_a, id_b;
            memcpy(&id_a, a, 8);
            memcpy(&id_b, b, 8);
            if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
            uint64_t off_a, off_b;
            memcpy(&off_a, a + 8, 8);
            memcpy(&off_b, b + 8, 8);
            return (off_a < off_b) ? -1 : (off_a > off_b) ? 1 : 0;
        }
        else
        {
            /* btree_index_entry: key(8) = inode_id, child_lba(8) */
            memcpy(&ua, a, 8);
            memcpy(&ub, b, 8);
            return (ua < ub) ? -1 : (ua > ub) ? 1 : 0;
        }

    case ORD_MEDIA_TAG:
    {
        /* Both leaf and index begin with inode_id(8), tag_type(2) */
        uint64_t id_a, id_b;
        memcpy(&id_a, a, 8);
        memcpy(&id_b, b, 8);
        if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
        uint16_t ta, tb;
        memcpy(&ta, a + 8, 2);
        memcpy(&tb, b + 8, 2);
        return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
    }

    case ORD_METADATA:
        if(is_leaf)
        {
            /* metadata_record: inode_id(8), key[256], value[1025] */
            uint64_t id_a, id_b;
            memcpy(&id_a, a, 8);
            memcpy(&id_b, b, 8);
            if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
            return strncmp((const char *)(a + 8), (const char *)(b + 8), METADATA_KEY_MAX);
        }
        else
        {
            /* metadata_index_entry: inode_id(8), key[256], child_lba(8) */
            uint64_t id_a, id_b;
            memcpy(&id_a, a, 8);
            memcpy(&id_b, b, 8);
            if(id_a != id_b) return (id_a < id_b) ? -1 : 1;
            return strncmp((const char *)(a + 8), (const char *)(b + 8), METADATA_KEY_MAX);
        }

    case ORD_METADATA_IDX:
        if(is_leaf)
        {
            /* metadata_idx_record: key[256], value[1025], inode_id(8) */
            int r = strncmp((const char *)a, (const char *)b, METADATA_KEY_MAX);
            if(r != 0) return r;
            r = strncmp((const char *)(a + METADATA_KEY_MAX), (const char *)(b + METADATA_KEY_MAX),
                        METADATA_VALUE_MAX);
            if(r != 0) return r;
            uint64_t id_a, id_b;
            memcpy(&id_a, a + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            memcpy(&id_b, b + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            return (id_a < id_b) ? -1 : (id_a > id_b) ? 1 : 0;
        }
        else
        {
            /* metadata_idx_index_entry: key[256], value[1025], inode_id(8), child_lba(8) */
            int r = strncmp((const char *)a, (const char *)b, METADATA_KEY_MAX);
            if(r != 0) return r;
            r = strncmp((const char *)(a + METADATA_KEY_MAX), (const char *)(b + METADATA_KEY_MAX),
                        METADATA_VALUE_MAX);
            if(r != 0) return r;
            uint64_t id_a, id_b;
            memcpy(&id_a, a + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            memcpy(&id_b, b + METADATA_KEY_MAX + METADATA_VALUE_MAX, 8);
            return (id_a < id_b) ? -1 : (id_a > id_b) ? 1 : 0;
        }

    default:
        return 0;
    }
}

/* Context for qsort comparator (file-scope; obmafsck is single-threaded). */
static int  s_ord_key_type;
static int  s_ord_is_leaf;

/** qsort comparator that delegates to ordering_key_cmp(). */
static int ordering_qsort_cmp(const void *a, const void *b)
{
    return ordering_key_cmp((const uint8_t *)a, (const uint8_t *)b, s_ord_key_type, s_ord_is_leaf);
}

/**
 * Sort records within a node buffer, recompute the checksum, and write
 * the corrected node back to disk.
 *
 * @param ctx       Filesystem context.
 * @param buf       Node buffer (single block or multi-block).
 * @param buf_size  Total size of @p buf in bytes.
 * @param lba       LBA of the node (first block for multi-block).
 * @param nblocks   Number of blocks the node spans (1 for normal trees).
 * @param hdr       Parsed node header.
 * @param key_type  One of the ORD_* tree-type constants.
 * @param is_leaf   Non-zero for leaf nodes.
 * @param stride    Record/entry size in bytes.
 * @return @c OBMAFS3_OK on successful write.
 */
static int fix_node_ordering(struct obmafs3_ctx *ctx, uint8_t *buf, size_t buf_size, uint64_t lba, int nblocks,
                             struct btree_node_header *hdr, int key_type, int is_leaf, size_t stride)
{
    /* Set file-scope qsort context */
    s_ord_key_type = key_type;
    s_ord_is_leaf  = is_leaf;

    /* Sort the records in-place */
    uint8_t *entries = buf + sizeof(struct btree_node_header);
    qsort(entries, hdr->node_keys, stride, ordering_qsort_cmp);

    /* Recompute checksum over header+keys only */
    size_t cs_data_size = sizeof(struct btree_node_header) + hdr->keys_length;
    memset(hdr->checksum, 0, 32);
    memcpy(buf, hdr, sizeof(*hdr));
    obmafs3_checksum_block(buf, cs_data_size, hdr->checksum);
    memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr->checksum, 32);

    /* Write back */
    for(int b = 0; b < nblocks; b++)
    {
        int rc = obmafs3_block_write(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                     (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) return rc;
    }
    return OBMAFS3_OK;
}

/**
 * Compare the header's total_nodes with the actual walked count and
 * optionally repair the header if they disagree.
 *
 * @param ctx        Filesystem context.
 * @param hdr        Pointer to the btree_header (will be modified on fix).
 * @param hdr_lba    LBA of the header block on disk.
 * @param actual     Actual number of nodes discovered by the walk.
 * @param tree_name  Human-readable tree name for messages.
 * @param auto_yes   If non-zero, always repair without asking.
 * @param auto_no    If non-zero, never repair.
 * @param errors     Pointer to the cumulative error counter.
 */
static void verify_fix_total_nodes(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba,
                                   uint64_t actual, const char *tree_name, const char *indent, int auto_yes,
                                   int auto_no, int *errors)
{
    if((uint64_t)hdr->total_nodes == actual)
    {
        result_ok("Total nodes:", "%u", hdr->total_nodes);
        return;
    }

    result_bad("Total nodes:", "MISMATCH (header %u, walked %" PRIu64 ")", hdr->total_nodes, actual);
    (*errors)++;

    if(ask_fix(auto_yes, auto_no, "Fix total_nodes in header?"))
    {
        hdr->total_nodes = (uint32_t)actual;
        int rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        if(rc == OBMAFS3_OK)
        {
            result_fixed("Total nodes:", "%" PRIu64, actual);
            (*errors)--;
        }
        else
        {
            fprintf(stderr, "%sError writing %s header: %d\n", indent, tree_name, rc);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Free node chain consistency                                        */
/* ------------------------------------------------------------------ */

/**
 * Verify that the free node chain fields are zero (since the runtime
 * never uses them) and optionally repair the header if they aren't.
 *
 * The @c free_node_lba and @c free_nodes fields in @c btree_header are
 * reserved for a future recycling optimisation.  The current runtime always
 * returns freed nodes directly to the allocation bitmap, so both fields
 * must be zero on a healthy filesystem.
 *
 * @param ctx        Filesystem context.
 * @param hdr        Pointer to the btree_header (will be modified on fix).
 * @param hdr_lba    LBA of the header block on disk.
 * @param tree_name  Human-readable tree name for messages.
 * @param indent     Indentation prefix for output lines.
 * @param auto_yes   If non-zero, always repair without asking.
 * @param auto_no    If non-zero, never repair.
 * @param errors     Pointer to the cumulative error counter.
 */
static void verify_fix_free_nodes(struct obmafs3_ctx *ctx, struct btree_header *hdr, uint64_t hdr_lba,
                                  const char *tree_name, const char *indent, int auto_yes, int auto_no, int *errors)
{
    if(hdr->free_node_lba == 0 && hdr->free_nodes == 0)
    {
        result_ok("Free node chain:", "");
        return;
    }

    result_bad("Free node chain:", "free_node_lba=%" PRIu64 ", free_nodes=%u", hdr->free_node_lba,
              hdr->free_nodes);
    (*errors)++;

    if(ask_fix(auto_yes, auto_no, "Reset free node chain in header?"))
    {
        hdr->free_node_lba = 0;
        hdr->free_nodes    = 0;
        int rc = obmafs3_btree_header_write(ctx, hdr_lba, hdr);
        if(rc == OBMAFS3_OK)
        {
            result_fixed("Free node chain:", "reset to 0");
            (*errors)--;
        }
        else
        {
            fprintf(stderr, "%sError writing %s header: %d\n", indent, tree_name, rc);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Sibling-link consistency                                           */
/* ------------------------------------------------------------------ */

/**
 * Patch a single node's left_link and/or right_link on disk.
 *
 * Reads the node, updates the header, recomputes the checksum, and
 * writes back.
 *
 * @param ctx         Filesystem context.
 * @param lba         LBA of the node to patch.
 * @param new_left    New left_link value.
 * @param new_right   New right_link value.
 * @param nblocks     Number of contiguous blocks per node (1 or METADATA_NODE_BLOCKS).
 * @return @c OBMAFS3_OK on success.
 */
static int patch_sibling_links(struct obmafs3_ctx *ctx, uint64_t lba, uint64_t new_left, uint64_t new_right,
                               int nblocks)
{
    size_t   node_sz = (size_t)nblocks * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    for(int b = 0; b < nblocks; b++)
    {
        int rc = obmafs3_block_read(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                    (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
    }

    struct btree_node_header hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    hdr.left_link  = new_left;
    hdr.right_link = new_right;

    /* Zero checksum, copy header into buffer, recompute over header+keys only */
    size_t data_size = sizeof(struct btree_node_header) + hdr.keys_length;
    memset(hdr.checksum, 0, 32);
    memcpy(buf, &hdr, sizeof(hdr));
    obmafs3_checksum_block(buf, data_size, hdr.checksum);
    memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr.checksum, 32);

    for(int b = 0; b < nblocks; b++)
    {
        int rc = obmafs3_block_write(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                     (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            return rc;
        }
    }
    free(buf);
    return OBMAFS3_OK;
}

/**
 * Verify (and optionally repair) sibling-link consistency for a B+Tree.
 *
 * Performs a BFS from the root so that nodes at each level are
 * encountered in left-to-right order.  For every adjacent pair
 * (A, B) at the same level the check verifies:
 *   - A.right_link == LBA(B)
 *   - B.left_link  == LBA(A)
 *
 * The leftmost node must have left_link == 0 and the rightmost must
 * have right_link == 0.
 *
 * @param ctx              Filesystem context.
 * @param root_lba         Root node LBA.
 * @param index_entry_size sizeof() of the index entry structure.
 * @param child_lba_off    Offset within the index entry to the child_lba field.
 * @param nblocks          Blocks per node (1 for normal trees, METADATA_NODE_BLOCKS for metadata).
 * @param tree_name        Human-readable tree name.
 * @param auto_yes         If non-zero, always repair without asking.
 * @param auto_no          If non-zero, never repair.
 * @param bad_count        Output: number of link violations found.
 * @param fixed_count      Output: number of nodes whose links were repaired.
 * @return @c OBMAFS3_OK on success (even if violations were found).
 */
static int verify_fix_sibling_links(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                                    size_t child_lba_off, int nblocks, const char *tree_name, int auto_yes,
                                    int auto_no, uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)nblocks * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    /*
     * BFS queue.  Each entry is an LBA.  Because BFS visits parents
     * before children and enumerates children left-to-right, nodes at
     * each level appear in the correct left-to-right order.
     */
    uint64_t *queue    = malloc(64 * sizeof(uint64_t));
    uint64_t  q_head   = 0;
    uint64_t  q_tail   = 0;
    uint64_t  q_cap    = 64;
    if(!queue) { free(buf); return OBMAFS3_ERR_NOMEM; }

    /* Per-node metadata gathered during BFS */
    typedef struct
    {
        uint64_t lba;
        uint64_t left_link;
        uint64_t right_link;
        uint8_t  level;
    } node_info_t;

    node_info_t *infos    = malloc(64 * sizeof(node_info_t));
    uint64_t     info_cnt = 0;
    uint64_t     info_cap = 64;
    if(!infos) { free(buf); free(queue); return OBMAFS3_ERR_NOMEM; }

    /* Enqueue root */
    queue[q_tail++] = root_lba;

    while(q_head < q_tail)
    {
        uint64_t lba = queue[q_head++];

        /* Read node */
        int read_ok = 1;
        for(int b = 0; b < nblocks; b++)
        {
            int rc = obmafs3_block_read(ctx, lba + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                        (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) { read_ok = 0; break; }
        }
        if(!read_ok) continue;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;

        /* Record node info */
        if(info_cnt >= info_cap)
        {
            info_cap *= 2;
            node_info_t *tmp = realloc(infos, info_cap * sizeof(node_info_t));
            if(!tmp) { free(buf); free(queue); free(infos); return OBMAFS3_ERR_NOMEM; }
            infos = tmp;
        }
        infos[info_cnt++] = (node_info_t){lba, hdr.left_link, hdr.right_link, hdr.level};

        /* If index node, enqueue children left-to-right */
        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                uint64_t child_lba;
                memcpy(&child_lba,
                       buf + sizeof(struct btree_node_header) + (size_t)i * index_entry_size + child_lba_off,
                       sizeof(child_lba));

                if(q_tail >= q_cap)
                {
                    q_cap *= 2;
                    uint64_t *tmp = realloc(queue, q_cap * sizeof(uint64_t));
                    if(!tmp) { free(buf); free(queue); free(infos); return OBMAFS3_ERR_NOMEM; }
                    queue = tmp;
                }
                queue[q_tail++] = child_lba;
            }
        }
    }

    free(buf);
    free(queue);

    /*
     * infos[] now contains nodes in BFS order.  Within the same level
     * they are already in left-to-right order.  Walk the array and
     * verify the doubly-linked list for each level.
     */
    uint64_t violations = 0;
    uint64_t fixes      = 0;

    uint64_t run_start = 0;
    while(run_start < info_cnt)
    {
        uint8_t  cur_level = infos[run_start].level;
        uint64_t run_end   = run_start + 1;
        while(run_end < info_cnt && infos[run_end].level == cur_level)
            run_end++;

        /* run_start..run_end-1 are all nodes at cur_level in L→R order */
        uint64_t run_len = run_end - run_start;

        int level_bad = 0; /* any violation at this level? */

        for(uint64_t i = run_start; i < run_end; i++)
        {
            uint64_t expect_left  = (i == run_start) ? 0 : infos[i - 1].lba;
            uint64_t expect_right = (i == run_end - 1) ? 0 : infos[i + 1].lba;

            if(infos[i].left_link != expect_left || infos[i].right_link != expect_right)
            {
                if(!level_bad)
                    fprintf(stderr,
                            "\n  %s: sibling-link violations at level %u (%" PRIu64 " node%s):\n",
                            tree_name, cur_level, run_len, run_len == 1 ? "" : "s");
                level_bad = 1;
                violations++;

                fprintf(stderr,
                        "    LBA %" PRIu64 ": left=%" PRIu64 " (expect %" PRIu64 "), "
                        "right=%" PRIu64 " (expect %" PRIu64 ")\n",
                        infos[i].lba, infos[i].left_link, expect_left, infos[i].right_link, expect_right);
            }
        }

        if(level_bad)
        {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "  Fix sibling links at %s level %u?", tree_name, cur_level);
            if(ask_fix(auto_yes, auto_no, prompt))
            {
                for(uint64_t i = run_start; i < run_end; i++)
                {
                    uint64_t expect_left  = (i == run_start) ? 0 : infos[i - 1].lba;
                    uint64_t expect_right = (i == run_end - 1) ? 0 : infos[i + 1].lba;

                    if(infos[i].left_link != expect_left || infos[i].right_link != expect_right)
                    {
                        int rc = patch_sibling_links(ctx, infos[i].lba, expect_left, expect_right, nblocks);
                        if(rc == OBMAFS3_OK)
                            fixes++;
                        else
                            fprintf(stderr, "    Error patching LBA %" PRIu64 ": %d\n", infos[i].lba, rc);
                    }
                }
            }
        }

        run_start = run_end;
    }

    free(infos);

    *bad_count   = violations;
    *fixed_count = fixes;
    return OBMAFS3_OK;
}

/**
 * Verify that keys within every node of a single-block B+Tree are
 * strictly ascending (leaf) or non-decreasing (index).
 * Optionally repairs misordered nodes by sorting records in-place.
 *
 * @param ctx              Filesystem context.
 * @param node_lbas        Array of node LBAs collected by a walk function.
 * @param node_count       Number of node LBAs.
 * @param key_type         One of the ORD_* tree-type constants.
 * @param leaf_rec_size    sizeof() of the leaf record structure.
 * @param idx_entry_size   sizeof() of the index entry structure.
 * @param tree_name        Human-readable tree name for progress/error messages.
 * @param auto_yes         If non-zero, always repair without asking.
 * @param auto_no          If non-zero, never repair.
 * @param bad_count        Output: number of nodes with ordering violations.
 * @param fixed_count      Output: number of nodes successfully repaired.
 * @return @c OBMAFS3_OK on success (even if ordering errors were found).
 */
static int verify_btree_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                 int key_type, size_t leaf_rec_size, size_t idx_entry_size, const char *tree_name,
                                 int auto_yes, int auto_no, uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    for(uint64_t n = 0; n < node_count; n++)
    {
        if(node_count > 10 && (n == 0 || (n & 0xFF) == 0 || n == node_count - 1))
        {
            char pfx[80];
            snprintf(pfx, sizeof(pfx), "Ordering %s", tree_name);
            print_bar(pfx, n + 1, node_count);
        }

        int rc = obmafs3_block_read(ctx, node_lbas[n], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) continue;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;
        if(hdr.node_keys < 2) continue; /* 0 or 1 keys — nothing to compare */

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            is_leaf = (hdr.level == 0);
        size_t         stride  = is_leaf ? leaf_rec_size : idx_entry_size;

        /* Sanity: ensure records fit within the block */
        size_t avail = (size_t)ctx->sb.block_size - sizeof(struct btree_node_header);
        if((size_t)hdr.node_keys * stride > avail)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": record count (%u) exceeds block capacity\n", tree_name,
                    node_lbas[n], hdr.node_keys);
            (*bad_count)++;
            continue;
        }

        int misordered = 0;
        for(uint16_t i = 1; i < hdr.node_keys; i++)
        {
            const uint8_t *prev = entries + (size_t)(i - 1) * stride;
            const uint8_t *curr = entries + (size_t)i * stride;
            int            cmp  = ordering_key_cmp(prev, curr, key_type, is_leaf);

            if(is_leaf && cmp >= 0)
            {
                fprintf(stderr, "\n  %s leaf at LBA %" PRIu64 ": keys not strictly ascending at position %u\n",
                        tree_name, node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
            else if(!is_leaf && cmp > 0)
            {
                fprintf(stderr, "\n  %s index at LBA %" PRIu64 ": keys not in order at position %u\n", tree_name,
                        node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
        }

        if(misordered)
        {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "  Sort %s node at LBA %" PRIu64 "?", tree_name, node_lbas[n]);
            if(ask_fix(auto_yes, auto_no, prompt))
            {
                rc = fix_node_ordering(ctx, buf, (size_t)ctx->sb.block_size, node_lbas[n], 1, &hdr, key_type, is_leaf,
                                       stride);
                if(rc == OBMAFS3_OK)
                {
                    printf("  Repaired %s node at LBA %" PRIu64 "\n", tree_name, node_lbas[n]);
                    (*fixed_count)++;
                }
                else
                {
                    fprintf(stderr, "  Error: failed to write repaired node at LBA %" PRIu64 ": %d\n", node_lbas[n],
                            rc);
                }
            }
        }
    }

    if(node_count > 10)
    {
        bar_clear();
    }

    free(buf);
    return OBMAFS3_OK;
}

/**
 * Verify key ordering within every node of a multi-block metadata B+Tree.
 *
 * Each node spans @c METADATA_NODE_BLOCKS contiguous blocks.
 *
 * @param ctx              Filesystem context.
 * @param node_lbas        Array of node start-LBAs collected by walk_meta_btree_nodes().
 * @param node_count       Number of node LBAs.
 * @param key_type         One of ORD_METADATA or ORD_METADATA_IDX.
 * @param leaf_rec_size    sizeof() of the leaf record structure.
 * @param idx_entry_size   sizeof() of the index entry structure.
 * @param tree_name        Human-readable tree name for progress/error messages.
 * @param auto_yes         If non-zero, always repair without asking.
 * @param auto_no          If non-zero, never repair.
 * @param bad_count        Output: number of nodes with ordering violations.
 * @param fixed_count      Output: number of nodes successfully repaired.
 * @return @c OBMAFS3_OK on success (even if ordering errors were found).
 */
static int verify_meta_ordering(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count, int key_type,
                                size_t leaf_rec_size, size_t idx_entry_size, const char *tree_name, int auto_yes,
                                int auto_no, uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    size_t   node_bytes = (size_t)ctx->sb.block_size * METADATA_NODE_BLOCKS;
    uint8_t *buf        = calloc(1, node_bytes);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    for(uint64_t n = 0; n < node_count; n++)
    {
        if(node_count > 10 && (n == 0 || (n & 0xFF) == 0 || n == node_count - 1))
        {
            char pfx[80];
            snprintf(pfx, sizeof(pfx), "Ordering %s", tree_name);
            print_bar(pfx, n + 1, node_count);
        }

        /* Read all blocks of this multi-block node */
        int ok = 1;
        for(int b = 0; b < METADATA_NODE_BLOCKS; b++)
        {
            int rc = obmafs3_block_read(ctx, node_lbas[n] + (uint64_t)b, buf + (size_t)b * ctx->sb.block_size,
                                        (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) { ok = 0; break; }
        }
        if(!ok) continue;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) continue;
        if(hdr.node_keys < 2) continue;

        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        int            is_leaf = (hdr.level == 0);
        size_t         stride  = is_leaf ? leaf_rec_size : idx_entry_size;

        size_t avail = node_bytes - sizeof(struct btree_node_header);
        if((size_t)hdr.node_keys * stride > avail)
        {
            fprintf(stderr, "\n  %s node at LBA %" PRIu64 ": record count (%u) exceeds node capacity\n", tree_name,
                    node_lbas[n], hdr.node_keys);
            (*bad_count)++;
            continue;
        }

        int misordered = 0;
        for(uint16_t i = 1; i < hdr.node_keys; i++)
        {
            const uint8_t *prev = entries + (size_t)(i - 1) * stride;
            const uint8_t *curr = entries + (size_t)i * stride;
            int            cmp  = ordering_key_cmp(prev, curr, key_type, is_leaf);

            if(is_leaf && cmp >= 0)
            {
                fprintf(stderr, "\n  %s leaf at LBA %" PRIu64 ": keys not strictly ascending at position %u\n",
                        tree_name, node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
            else if(!is_leaf && cmp > 0)
            {
                fprintf(stderr, "\n  %s index at LBA %" PRIu64 ": keys not in order at position %u\n", tree_name,
                        node_lbas[n], i);
                misordered = 1;
                (*bad_count)++;
                break;
            }
        }

        if(misordered)
        {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "  Sort %s node at LBA %" PRIu64 "?", tree_name, node_lbas[n]);
            if(ask_fix(auto_yes, auto_no, prompt))
            {
                int wrc = fix_node_ordering(ctx, buf, node_bytes, node_lbas[n], METADATA_NODE_BLOCKS, &hdr, key_type,
                                            is_leaf, stride);
                if(wrc == OBMAFS3_OK)
                {
                    printf("  Repaired %s node at LBA %" PRIu64 "\n", tree_name, node_lbas[n]);
                    (*fixed_count)++;
                }
                else
                {
                    fprintf(stderr, "  Error: failed to write repaired node at LBA %" PRIu64 ": %d\n", node_lbas[n],
                            wrc);
                }
            }
        }
    }

    if(node_count > 10)
    {
        bar_clear();
    }

    free(buf);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Walk all nodes in a multi-block metadata B+Tree (DFS)              */
/*  Each node spans METADATA_NODE_BLOCKS contiguous blocks.            */
/*  index_entry_size / child_lba_off parameterize the index entry.     */
/* ------------------------------------------------------------------ */

/**
 * Walk all nodes in a multi-block metadata B+Tree via iterative DFS.
 *
 * Each node spans @c METADATA_NODE_BLOCKS contiguous blocks.  The
 * caller provides the index entry size and the byte offset of the
 * child_lba field within that entry to locate children.
 *
 * @param ctx               Filesystem context.
 * @param root_lba          Root node LBA.
 * @param index_entry_size  Size in bytes of each index entry.
 * @param child_lba_off     Byte offset of the @c child_lba field in
 *                          the index entry structure.
 * @param out_lbas          Output: heap-allocated array of node LBAs.
 * @param out_count         Output: number of elements in @p out_lbas.
 * @return @c OBMAFS3_OK on success.
 */
static int walk_meta_btree_nodes(struct obmafs3_ctx *ctx, uint64_t root_lba, size_t index_entry_size,
                                 size_t child_lba_off, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        if(count >= cap)
        {
            cap           = cap == 0 ? 64 : cap * 2;
            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
            if(!tmp)
            {
                free(buf);
                free(stack);
                free(lbas);
                return OBMAFS3_ERR_NOMEM;
            }
            lbas = tmp;
        }
        lbas[count++] = lba;

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                uint64_t child;
                memcpy(&child, buf + sizeof(struct btree_node_header) + (size_t)i * index_entry_size + child_lba_off,
                       sizeof(uint64_t));

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = child;
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Verify checksums for a list of multi-block metadata node LBAs      */
/* ------------------------------------------------------------------ */

/**
 * Verify stored checksums of a list of multi-block metadata node
 * blocks.
 *
 * Each node spans @c METADATA_NODE_BLOCKS contiguous blocks.
 * Optionally rewrites nodes with corrected checksums.
 *
 * @param ctx          Filesystem context.
 * @param node_lbas    Array of node LBAs to verify.
 * @param node_count   Number of elements in @p node_lbas.
 * @param tree_name    Human-readable tree name for diagnostic output.
 * @param auto_yes     If non-zero, always repair without asking.
 * @param auto_no      If non-zero, never repair.
 * @param bad_count    Output: number of nodes with bad checksums.
 * @param fixed_count  Output: number of nodes whose checksums were repaired.
 * @return @c OBMAFS3_OK on success.
 */
static int verify_meta_node_checksums(struct obmafs3_ctx *ctx, const uint64_t *node_lbas, uint64_t node_count,
                                      const char *tree_name, int auto_yes, int auto_no, uint64_t *bad_count,
                                      uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *bad_lbas = NULL;
    uint64_t  bad_cap  = 0;
    uint64_t  bad      = 0;

    for(uint64_t n = 0; n < node_count; n++)
    {
        uint64_t lba = node_lbas[n];

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(bad_lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            fprintf(stderr, "  %s node at LBA %" PRIu64 ": bad magic\n", tree_name, lba);
            bad++;
            continue;
        }

        size_t  data_size = sizeof(struct btree_node_header) + hdr.keys_length;
        uint8_t stored[32];
        memcpy(stored, hdr.checksum, 32);
        memset(buf + __builtin_offsetof(struct btree_node_header, checksum), 0, 32);
        uint8_t computed[32];
        obmafs3_checksum_block(buf, data_size, computed);
        memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), stored, 32);

        if(memcmp(stored, computed, 32) != 0)
        {
            fprintf(stderr, "  %s node at LBA %" PRIu64 ": checksum mismatch\n", tree_name, lba);
            if(bad >= bad_cap)
            {
                bad_cap = bad_cap ? bad_cap * 2 : 16;
                uint64_t *tmp = realloc(bad_lbas, bad_cap * sizeof(uint64_t));
                if(!tmp) { free(buf); free(bad_lbas); return OBMAFS3_ERR_NOMEM; }
                bad_lbas = tmp;
            }
            bad_lbas[bad] = lba;
            bad++;
        }
    }

    /* Offer to fix */
    uint64_t fixes = 0;
    if(bad > 0)
    {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "  Fix %" PRIu64 " %s node checksum%s?", bad, tree_name,
                 bad == 1 ? "" : "s");
        if(ask_fix(auto_yes, auto_no, prompt))
        {
            for(uint64_t i = 0; i < bad; i++)
            {
                int rc = obmafs3_block_read(ctx, bad_lbas[i], buf, node_sz);
                if(rc != OBMAFS3_OK) continue;

                struct btree_node_header hdr;
                memcpy(&hdr, buf, sizeof(hdr));
                size_t data_size = sizeof(struct btree_node_header) + hdr.keys_length;
                memset(hdr.checksum, 0, 32);
                memcpy(buf, &hdr, sizeof(hdr));
                obmafs3_checksum_block(buf, data_size, hdr.checksum);
                memcpy(buf + __builtin_offsetof(struct btree_node_header, checksum), hdr.checksum, 32);

                rc = obmafs3_block_write(ctx, bad_lbas[i], buf, node_sz);
                if(rc == OBMAFS3_OK)
                    fixes++;
                else
                    fprintf(stderr, "    Error writing LBA %" PRIu64 ": %d\n", bad_lbas[i], rc);
            }
        }
    }

    free(buf);
    free(bad_lbas);
    *bad_count   = bad;
    *fixed_count = fixes;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Collect data-block LBAs from all inodes                            */
/* ------------------------------------------------------------------ */

/**
 * Collect data block LBAs referenced by all inode extent records.
 *
 * Walks the inode B+Tree via iterative DFS and extracts every data
 * block LBA from the extent arrays of each leaf-level inode record.
 *
 * @param ctx             Filesystem context.
 * @param inode_root_lba  Root node LBA of the inode tree.
 * @param out_lbas        Output: heap-allocated array of data block LBAs.
 * @param out_count       Output: number of elements in @p out_lbas.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_inode_data_blocks(struct obmafs3_ctx *ctx, uint64_t inode_root_lba, uint64_t **out_lbas,
                                     uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(inode_root_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Iterative DFS via explicit stack (avoids stale right_link
       references to freed leaf nodes) */
    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = inode_root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(hdr.level > 0)
        {
            /* Index node: push children onto stack */
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf node: collect extent blocks from inode records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct inode_record rec;
            memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

            for(int e = 0; e < 8; e++)
            {
                if(rec.extents[e].block_count == 0) continue;
                for(uint64_t b = 0; b < rec.extents[e].block_count; b++)
                {
                    if(count >= cap)
                    {
                        cap           = (cap == 0) ? 128 : cap * 2;
                        uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(buf);
                            free(stack);
                            free(lbas);
                            return OBMAFS3_ERR_NOMEM;
                        }
                        lbas = tmp;
                    }
                    lbas[count++] = rec.extents[e].start_block + b;
                }
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Walk the overflow B+Tree (DFS) and collect all data blocks referenced
 * by overflow_extent entries in leaf nodes.
 */
static int collect_overflow_data_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(ctx->overflow_hdr.root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Iterative DFS via explicit stack */
    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level > 0)
        {
            /* Index node: push children onto stack */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));

                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
        }
        else
        {
            /* Leaf node: collect data block LBAs from extents */
            const uint8_t *entries = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct overflow_extent oe;
                memcpy(&oe, entries + i * sizeof(struct overflow_extent), sizeof(oe));
                for(uint64_t b = 0; b < oe.block_count; b++)
                {
                    if(count >= cap)
                    {
                        cap           = (cap == 0) ? 128 : cap * 2;
                        uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(buf);
                            free(stack);
                            free(lbas);
                            return OBMAFS3_ERR_NOMEM;
                        }
                        lbas = tmp;
                    }
                    lbas[count++] = oe.start_block + b;
                }
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Collect external data blocks used by media tags                    */
/* ------------------------------------------------------------------ */

/**
 * Walk the media tag B+Tree and collect all external (non-inline)
 * data block LBAs.
 */
static int collect_media_tag_data_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(ctx->sb.media_tag_lba == 0 || ctx->media_tag_hdr.root_node_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0;
    uint64_t  stk_cap  = 64;
    if(!stack)
    {
        free(buf);
        return OBMAFS3_ERR_NOMEM;
    }

    stack[stk_size++] = ctx->media_tag_hdr.root_node_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            free(buf);
            free(stack);
            free(lbas);
            return rc;
        }

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));

        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC)
        {
            free(buf);
            free(stack);
            free(lbas);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if(nhdr.level > 0)
        {
            const uint8_t *entries = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct media_tag_index_entry ie;
                memcpy(&ie, entries + i * sizeof(struct media_tag_index_entry), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp)
                    {
                        free(buf);
                        free(stack);
                        free(lbas);
                        return OBMAFS3_ERR_NOMEM;
                    }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
        }
        else
        {
            const uint8_t *entries = buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct media_tag_record rec;
                memcpy(&rec, entries + i * sizeof(struct media_tag_record), sizeof(rec));

                if(!(rec.flags & MEDIA_TAG_FLAG_INLINE) && rec.data_lba != 0 && rec.data_blocks != 0)
                {
                    for(uint64_t b = 0; b < rec.data_blocks; b++)
                    {
                        if(count >= cap)
                        {
                            cap           = (cap == 0) ? 32 : cap * 2;
                            uint64_t *tmp = realloc(lbas, cap * sizeof(*tmp));
                            if(!tmp)
                            {
                                free(buf);
                                free(stack);
                                free(lbas);
                                return OBMAFS3_ERR_NOMEM;
                            }
                            lbas = tmp;
                        }
                        lbas[count++] = rec.data_lba + b;
                    }
                }
            }
        }
    }

    free(buf);
    free(stack);
    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Collect all blocks used by dedup trees (headers, nodes, data)      */
/* ------------------------------------------------------------------ */

/**
 * Walk the dedup tree list and collect every block LBA that belongs to
 * dedup structures: the tree list block itself, each per-sector-size
 * tree header, every tree node, and every dedup data block.
 *
 * Dedup data blocks span dedup_block_size / block_size standard blocks.
 * Multiple dedup_entry records may share the same data block (different
 * offsets), so we deduplicate the data block LBAs.
 */
static int collect_dedup_blocks(struct obmafs3_ctx *ctx, uint64_t **out_lbas, uint64_t *out_count)
{
    *out_lbas  = NULL;
    *out_count = 0;

    if(ctx->sb.dedup_lba == 0) return OBMAFS3_OK;

    uint64_t *lbas  = NULL;
    uint64_t  count = 0;
    uint64_t  cap   = 0;

    /* Separate small array to track unique dedup data block base LBAs
     * for fast deduplication (one entry per dedup data block). */
    uint64_t *unique_bases = NULL;
    uint64_t  unique_count = 0;
    uint64_t  unique_cap   = 0;

#define PUSH_LBA(blk)                                        \
    do                                                       \
    {                                                        \
        if(count >= cap)                                     \
        {                                                    \
            cap          = (cap == 0) ? 256 : cap * 2;       \
            uint64_t *_t = realloc(lbas, cap * sizeof(*_t)); \
            if(!_t)                                          \
            {                                                \
                free(lbas);                                  \
                return OBMAFS3_ERR_NOMEM;                    \
            }                                                \
            lbas = _t;                                       \
        }                                                    \
        lbas[count++] = (blk);                               \
    } while(0)

    /* The tree list block itself is already marked in build_expected_bitmap */

    /* Read the tree list */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf) return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        free(list_buf);
        return rc;
    }

    struct tree_list_header list_hdr;
    memcpy(&list_hdr, list_buf, sizeof(list_hdr));
    if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC)
    {
        free(list_buf);
        return OBMAFS3_ERR_BADMAGIC;
    }

    uint64_t tree_count = list_hdr.tree_count;
    if(tree_count == 0)
    {
        free(list_buf);
        *out_lbas  = lbas;
        *out_count = count;
        return OBMAFS3_OK;
    }

    struct tree_list_entry *entries = malloc((size_t)(tree_count * sizeof(struct tree_list_entry)));
    if(!entries)
    {
        free(list_buf);
        return OBMAFS3_ERR_NOMEM;
    }
    memcpy(entries, list_buf + sizeof(struct tree_list_header), (size_t)(tree_count * sizeof(struct tree_list_entry)));
    free(list_buf);

    uint64_t std_per_dedup = ctx->sb.dedup_block_size / ctx->sb.block_size;

    /* For each dedup tree */
    for(uint64_t t = 0; t < tree_count; t++)
    {
        /* Mark the tree header block */
        PUSH_LBA(entries[t].tree_lba);

        /* Read tree header to get root node */
        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        /* Walk tree nodes (B+Tree: DFS walk) */
        uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(!node_buf)
        {
            free(entries);
            free(lbas);
            return OBMAFS3_ERR_NOMEM;
        }

        /* Iterative DFS via explicit stack */
        uint64_t *stk      = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0;
        uint64_t  stk_cap  = 64;
        if(!stk)
        {
            free(node_buf);
            free(entries);
            free(lbas);
            return OBMAFS3_ERR_NOMEM;
        }

        stk[stk_size++] = thdr.root_node_lba;

        uint64_t nodes_visited = 0;

        while(stk_size > 0)
        {
            uint64_t lba = stk[--stk_size];

            /* Mark the node block */
            PUSH_LBA(lba);

            nodes_visited++;
            {
                char pfx[80];
                snprintf(pfx, sizeof(pfx), "Collecting dedup [tree %" PRIu64 "/%" PRIu64 "]",
                         t + 1, tree_count);
                print_bar(pfx, nodes_visited, (uint64_t)thdr.total_nodes);
            }

            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                /* Index node: push children onto stack */
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, node_buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));

                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stk, stk_cap * sizeof(*tmp));
                        if(!tmp)
                        {
                            free(stk);
                            free(node_buf);
                            free(entries);
                            free(lbas);
                            free(unique_bases);
                            return OBMAFS3_ERR_NOMEM;
                        }
                        stk = tmp;
                    }
                    stk[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf node: collect data block LBAs from dedup entries */
            const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct dedup_entry de;
                memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));
                if(de.block_lba == 0) continue;

                /* Check if we already recorded this data block base LBA.
                 * Multiple entries can share the same dedup data block
                 * at different offsets. Use the small unique_bases array
                 * for fast lookup instead of scanning the full output. */
                int found = 0;
                for(uint64_t j = 0; j < unique_count; j++)
                {
                    if(unique_bases[j] == de.block_lba)
                    {
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    /* Record this base LBA */
                    if(unique_count >= unique_cap)
                    {
                        unique_cap   = (unique_cap == 0) ? 256 : unique_cap * 2;
                        uint64_t *ut = realloc(unique_bases, unique_cap * sizeof(*ut));
                        if(!ut)
                        {
                            free(stk);
                            free(node_buf);
                            free(entries);
                            free(lbas);
                            free(unique_bases);
                            return OBMAFS3_ERR_NOMEM;
                        }
                        unique_bases = ut;
                    }
                    unique_bases[unique_count++] = de.block_lba;

                    /* Read block header to determine actual allocation */
                    uint8_t  hdr_tmp[sizeof(struct block_header)];
                    int      hrc      = obmafs3_block_read(ctx, de.block_lba, hdr_tmp, sizeof(hdr_tmp));
                    uint64_t used_std = std_per_dedup; /* fallback */
                    if(hrc == OBMAFS3_OK)
                    {
                        struct block_header bh;
                        memcpy(&bh, hdr_tmp, sizeof(bh));
                        if(bh.magic == OBMAFS3_BLOCK_MAGIC)
                        {
                            uint64_t payload =
                                (bh.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? bh.compressed_size : bh.original_size;
                            uint64_t on_disk = sizeof(bh) + payload;
                            uint64_t bs      = ctx->sb.block_size;
                            used_std         = (on_disk + bs - 1) / bs;
                            if(used_std > std_per_dedup) used_std = std_per_dedup;
                        }
                    }

                    /* The last (partial) block of each dedup tree keeps all
                     * std_per_dedup blocks allocated — dedup_block_flush
                     * intentionally does not free trailing blocks so the
                     * block can be resumed on next mount.  Account for
                     * that here so the expected bitmap matches. */
                    uint64_t mark_std = (de.block_lba == thdr.last_block_lba)
                                            ? std_per_dedup
                                            : used_std;
                    for(uint64_t s = 0; s < mark_std; s++) PUSH_LBA(de.block_lba + s);
                }
            }
        }

        free(stk);
        free(node_buf);
    }

    free(entries);
    free(unique_bases);
#undef PUSH_LBA

    /* Clear progress line */
    bar_clear();

    *out_lbas  = lbas;
    *out_count = count;
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Build expected bitmap                                              */
/* ------------------------------------------------------------------ */

/**
 * Reconstruct the expected allocation bitmap from on-disk structures.
 *
 * Walks every tree (superblock, catalog, inode, overflow, dedup, media
 * tag, CD prefix/suffix/subchannel, metadata, metadata index) and
 * marks every referenced block in a freshly allocated bitmap.  The
 * result can be compared against the on-disk bitmap to detect
 * allocation inconsistencies.
 *
 * @param ctx           Filesystem context.
 * @param total_blocks  Number of blocks in the filesystem.
 * @param bitmap_bytes  Size of the bitmap in bytes.
 * @param out_error     Output: set to non-zero on allocation failure.
 * @return Heap-allocated expected bitmap, or @c NULL on error.
 */
static uint8_t *build_expected_bitmap(struct obmafs3_ctx *ctx, uint64_t total_blocks, uint64_t bitmap_bytes,
                                      int *out_error)
{
    uint8_t *expected = calloc(1, (size_t)bitmap_bytes);
    if(!expected)
    {
        *out_error = 1;
        return NULL;
    }

    /* Progress reporting — 17 discrete steps */
    int         step       = 0;
    const int   total_steps = 17;

#define PROGRESS(desc)                                                         \
    do                                                                         \
    {                                                                          \
        step++;                                                                \
        char _p_pfx[64];                                                       \
        snprintf(_p_pfx, sizeof(_p_pfx), "Bitmap [%2d/%d] %s",                \
                 step, total_steps, (desc));                                    \
        print_bar(_p_pfx, (uint64_t)step, (uint64_t)total_steps);             \
    } while(0)

/* Helper to set a bit */
#define MARK(blk)                                                            \
    do                                                                       \
    {                                                                        \
        if((blk) < total_blocks) expected[(blk) / 8] |= (1u << ((blk) % 8)); \
    } while(0)

    /* Block 0: superblock */
    PROGRESS("superblock");
    MARK(0);

    /* Catalog tree: header + nodes (B+Tree: DFS walk) */
    PROGRESS("catalog tree");
    MARK(ctx->sb.catalog_lba);
    {
        uint64_t *cat_nodes = NULL;
        uint64_t  cat_count = 0;
        int       rc        = walk_catalog_btree_nodes(ctx, ctx->catalog_hdr.root_node_lba, &cat_nodes, &cat_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < cat_count; i++) MARK(cat_nodes[i]);
            free(cat_nodes);
        }
        else
        {
            fprintf(stderr, "Warning: could not walk catalog tree nodes\n");
        }
    }

    /* Inode tree: header + nodes (B+Tree: DFS walk) */
    PROGRESS("inode tree");
    MARK(ctx->sb.inode_lba);
    {
        uint64_t *ino_nodes = NULL;
        uint64_t  ino_count = 0;
        int       rc        = walk_inode_btree_nodes(ctx, ctx->inode_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ino_nodes, &ino_count, 0, NULL);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < ino_count; i++) MARK(ino_nodes[i]);
            free(ino_nodes);
        }
        else
        {
            fprintf(stderr, "Warning: could not walk inode tree nodes\n");
        }
    }

    /* Overflow tree header (if present) */
    PROGRESS("overflow tree");
    if(ctx->sb.overflow_lba != 0)
    {
        MARK(ctx->sb.overflow_lba);
        if(ctx->overflow_hdr.root_node_lba != 0)
        {
            uint64_t *ovf_nodes = NULL;
            uint64_t  ovf_count = 0;
            int       rc        = walk_inode_btree_nodes(ctx, ctx->overflow_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ovf_nodes, &ovf_count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < ovf_count; i++) MARK(ovf_nodes[i]);
                free(ovf_nodes);
            }
        }
    }

    /* Dedup tree list header */
    PROGRESS("dedup tree list");
    if(ctx->sb.dedup_lba != 0) MARK(ctx->sb.dedup_lba);

    /* Media tag tree header and nodes (if present) */
    PROGRESS("media tag tree");
    if(ctx->sb.media_tag_lba != 0)
    {
        MARK(ctx->sb.media_tag_lba);
        if(ctx->media_tag_hdr.root_node_lba != 0)
        {
            uint64_t *mt_nodes = NULL;
            uint64_t  mt_count = 0;
            int       rc       = walk_inode_btree_nodes(ctx, ctx->media_tag_hdr.root_node_lba, sizeof(struct media_tag_index_entry), __builtin_offsetof(struct media_tag_index_entry, child_lba), &mt_nodes, &mt_count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < mt_count; i++) MARK(mt_nodes[i]);
                free(mt_nodes);
            }
        }
    }

    /* CD prefix tree header and nodes (if present) */
    PROGRESS("CD prefix tree");
    if(ctx->sb.cd_prefix_lba != 0)
    {
        MARK(ctx->sb.cd_prefix_lba);
        if(ctx->cd_prefix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->cd_prefix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* CD suffix tree header and nodes (if present) */
    PROGRESS("CD suffix tree");
    if(ctx->sb.cd_suffix_lba != 0)
    {
        MARK(ctx->sb.cd_suffix_lba);
        if(ctx->cd_suffix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->cd_suffix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* CD subchannel tree header and nodes (if present) */
    PROGRESS("CD subchannel tree");
    if(ctx->sb.cd_subchannel_lba != 0)
    {
        MARK(ctx->sb.cd_subchannel_lba);
        if(ctx->cd_subchannel_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->cd_subchannel_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* Metadata tree header and nodes (multi-block, if present) */
    PROGRESS("metadata tree");
    if(ctx->sb.metadata_lba != 0)
    {
        MARK(ctx->sb.metadata_lba);
        if(ctx->metadata_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int rc = walk_meta_btree_nodes(ctx, ctx->metadata_hdr.root_node_lba, sizeof(struct metadata_index_entry),
                                           __builtin_offsetof(struct metadata_index_entry, child_lba), &nodes, &count);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++)
                    for(int b = 0; b < METADATA_NODE_BLOCKS; b++) MARK(nodes[i] + (uint64_t)b);
                free(nodes);
            }
        }
    }

    /* Metadata index tree header and nodes (multi-block, if present) */
    PROGRESS("metadata index tree");
    if(ctx->sb.metadata_idx_lba != 0)
    {
        MARK(ctx->sb.metadata_idx_lba);
        if(ctx->metadata_idx_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc =
                walk_meta_btree_nodes(ctx, ctx->metadata_idx_hdr.root_node_lba, sizeof(struct metadata_idx_index_entry),
                                      __builtin_offsetof(struct metadata_idx_index_entry, child_lba), &nodes, &count);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++)
                    for(int b = 0; b < METADATA_NODE_BLOCKS; b++) MARK(nodes[i] + (uint64_t)b);
                free(nodes);
            }
        }
    }

    /* Refcount tree header and nodes (if present) */
    PROGRESS("refcount tree");
    if(ctx->sb.refcount_lba != 0)
    {
        MARK(ctx->sb.refcount_lba);
        if(ctx->refcount_hdr.root_node_lba != 0)
        {
            uint64_t *nodes = NULL;
            uint64_t  count = 0;
            int       rc    = walk_inode_btree_nodes(ctx, ctx->refcount_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &count, 0, NULL);
            if(rc == OBMAFS3_OK)
            {
                for(uint64_t i = 0; i < count; i++) MARK(nodes[i]);
                free(nodes);
            }
        }
    }

    /* Bitmap blocks */
    PROGRESS("bitmap blocks");
    for(uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++) MARK(ctx->sb.bitmap_lba + i);

    /* Keyset blocks */
    if(ctx->sb.keyset_lba != 0 && ctx->sb.keyset_blocks != 0)
    {
        PROGRESS("keyset blocks");
        for(uint64_t i = 0; i < ctx->sb.keyset_blocks; i++) MARK(ctx->sb.keyset_lba + i);
    }

    /* Backup superblock at the last block */
    MARK(total_blocks - 1);

    /* File data blocks from inode extents */
    PROGRESS("inode data blocks");
    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint64_t *data_lbas  = NULL;
        uint64_t  data_count = 0;
        int       rc         = collect_inode_data_blocks(ctx, ctx->inode_hdr.root_node_lba, &data_lbas, &data_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < data_count; i++) MARK(data_lbas[i]);
            free(data_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect inode data blocks\n");
        }
    }

    /* File data blocks from overflow extents */
    PROGRESS("overflow data blocks");
    {
        uint64_t *ovf_data_lbas  = NULL;
        uint64_t  ovf_data_count = 0;
        int       rc             = collect_overflow_data_blocks(ctx, &ovf_data_lbas, &ovf_data_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < ovf_data_count; i++) MARK(ovf_data_lbas[i]);
            free(ovf_data_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect overflow data blocks\n");
        }
    }

    /* Dedup tree blocks: headers, nodes, and data blocks */
    PROGRESS("dedup data blocks");
    {
        uint64_t *dedup_lbas  = NULL;
        uint64_t  dedup_count = 0;
        int       rc          = collect_dedup_blocks(ctx, &dedup_lbas, &dedup_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < dedup_count; i++) MARK(dedup_lbas[i]);
            free(dedup_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect dedup tree blocks\n");
        }
    }

    /* External media tag data blocks */
    PROGRESS("media tag data blocks");
    {
        uint64_t *mt_data_lbas  = NULL;
        uint64_t  mt_data_count = 0;
        int       rc            = collect_media_tag_data_blocks(ctx, &mt_data_lbas, &mt_data_count);
        if(rc == OBMAFS3_OK)
        {
            for(uint64_t i = 0; i < mt_data_count; i++) MARK(mt_data_lbas[i]);
            free(mt_data_lbas);
        }
        else
        {
            fprintf(stderr, "Warning: could not collect media tag data blocks\n");
        }
    }

#undef MARK

    /* Clear the progress line */
    bar_clear();

#undef PROGRESS

    *out_error = 0;
    return expected;
}

/* ------------------------------------------------------------------ */
/*  Scrub: verify checksums of all data blocks                         */
/* ------------------------------------------------------------------ */

/**
 * Print a progress bar (with error count) to stderr.
 *
 * Uses the same visual style as print_bar but appends an error counter.
 *
 * @param label  Activity label (e.g. "Scrubbing data blocks").
 * @param done   Number of items processed so far.
 * @param total  Total number of items.
 * @param bad    Number of errors detected so far.
 */
static void print_progress(const char *label, uint64_t done, uint64_t total, uint64_t bad)
{
    const int bar_width = 30;
    double    frac      = total > 0 ? (double)done / (double)total : 1.0;
    if(frac > 1.0) frac = 1.0;
    int pct = (int)(frac * 100.0);

    if(g_use_color)
    {
        static const char *blocks[] = { " ", "\u258F", "\u258E", "\u258D",
                                        "\u258C", "\u258B", "\u258A", "\u2589", "\u2588" };
        double filled_f = frac * bar_width;
        int    filled_i = (int)filled_f;
        int    sub      = (int)((filled_f - filled_i) * 8.0);

        fprintf(stderr, "\r  \033[36m%-24s\033[0m ", label);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled_i)
                fprintf(stderr, "\033[36m\u2588\033[0m");
            else if(i == filled_i)
                fprintf(stderr, "\033[36m%s\033[0m", blocks[sub]);
            else
                fprintf(stderr, "\033[2m\u2591\033[0m");
        }
        fprintf(stderr, " %3d%% \033[2m\u00B7\033[0m %" PRIu64 "/%" PRIu64, pct, done, total);
        if(bad > 0)
            fprintf(stderr, " \033[31m\u00B7 %" PRIu64 " error%s\033[0m", bad, bad == 1 ? "" : "s");
        fprintf(stderr, "  ");
    }
    else
    {
        int filled = (int)(frac * bar_width);
        fprintf(stderr, "\r  %-24s [", label);
        for(int i = 0; i < bar_width; i++)
        {
            if(i < filled)       fputc('=', stderr);
            else if(i == filled) fputc('>', stderr);
            else                 fputc(' ', stderr);
        }
        fprintf(stderr, "] %3d%% %" PRIu64 "/%" PRIu64, pct, done, total);
        if(bad > 0)
            fprintf(stderr, " | %" PRIu64 " error%s", bad, bad == 1 ? "" : "s");
        fprintf(stderr, "   ");
    }
    fflush(stderr);
}

/**
 * Verify checksums of all file data blocks.
 *
 * Collects data block LBAs from both inline inode extents and the
 * overflow tree, reads each block header, recomputes the checksum,
 * and reports any mismatches.
 *
 * @param ctx  Filesystem context.
 * @return Number of bad blocks detected.
 */
static uint64_t scrub_data_blocks(struct obmafs3_ctx *ctx)
{
    /* Collect all data block LBAs from inode extents */
    if(ctx->inode_hdr.root_node_lba == 0)
    {
        printf("\n  %sData block scrub%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no data blocks to scrub");
        return 0;
    }

    uint64_t *data_lbas  = NULL;
    uint64_t  data_count = 0;
    int       rc         = collect_inode_data_blocks(ctx, ctx->inode_hdr.root_node_lba, &data_lbas, &data_count);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "\nError: could not collect data block LBAs: %d\n", rc);
        return 0;
    }

    /* Also collect overflow data blocks */
    uint64_t *ovf_lbas  = NULL;
    uint64_t  ovf_count = 0;
    rc                  = collect_overflow_data_blocks(ctx, &ovf_lbas, &ovf_count);
    if(rc == OBMAFS3_OK && ovf_count > 0)
    {
        uint64_t *tmp = realloc(data_lbas, (data_count + ovf_count) * sizeof(*tmp));
        if(tmp)
        {
            data_lbas = tmp;
            memcpy(data_lbas + data_count, ovf_lbas, ovf_count * sizeof(*ovf_lbas));
            data_count += ovf_count;
        }
        free(ovf_lbas);
    }
    else
    {
        free(ovf_lbas);
    }

    if(data_count == 0)
    {
        printf("\nData block scrub:\n");
        printf("  No data blocks to scrub.\n");
        free(data_lbas);
        return 0;
    }

    printf("\n  %sData block scrub%s\n", CLR_BOLD, CLR_RESET);
    result_info("Blocks to verify:", "%" PRIu64, data_count);

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        free(data_lbas);
        return 0;
    }

    uint64_t bad         = 0;
    uint64_t read_errors = 0;

    /* With variable-length extents, individual data blocks no longer
     * carry a block_header.  Only compressed extent groups have a
     * header (spanning multiple physical blocks).  For this per-block
     * readability pass we simply verify that each physical block can
     * be read successfully. */
    for(uint64_t i = 0; i < data_count; i++)
    {
        if(i % 64 == 0 || i == data_count - 1) print_progress("Data blocks", i + 1, data_count, bad);

        rc = obmafs3_block_read(ctx, data_lbas[i], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            read_errors++;
            bad++;
        }
    }

    print_progress("Data blocks", data_count, data_count, bad);
    bar_clear();

    if(bad == 0) { result_ok("Result:", ""); }
    else
    {
        result_bad("Result:", "%" PRIu64 " error(s)", bad);
        if(read_errors > 0) printf("    Read errors:    %" PRIu64 "\n", read_errors);
    }

    free(buf);
    free(data_lbas);
    return bad;
}

/**
 * Scrub dedup data blocks.
 * Each dedup data block is a contiguous 4 MiB region (dedup_block_size)
 * with a single block_header at the start covering all the sector data.
 */
static uint64_t scrub_dedup_data_blocks(struct obmafs3_ctx *ctx)
{
    if(ctx->sb.dedup_lba == 0)
    {
        printf("\n  %sDedup data block scrub%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup blocks to scrub");
        return 0;
    }

    /* Read the tree list header */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf)
    {
        fprintf(stderr, "\nError: out of memory\n");
        return 0;
    }

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "\nError: could not read dedup tree list: %d\n", rc);
        free(list_buf);
        return 0;
    }

    struct tree_list_header list_hdr;
    memcpy(&list_hdr, list_buf, sizeof(list_hdr));
    if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC || list_hdr.tree_count == 0)
    {
        free(list_buf);
        printf("\n  %sDedup data block scrub%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup blocks to scrub");
        return 0;
    }

    uint64_t                tree_count = list_hdr.tree_count;
    struct tree_list_entry *entries    = malloc((size_t)(tree_count * sizeof(struct tree_list_entry)));
    if(!entries)
    {
        free(list_buf);
        return 0;
    }
    memcpy(entries, list_buf + sizeof(struct tree_list_header), (size_t)(tree_count * sizeof(struct tree_list_entry)));
    free(list_buf);

    /* Collect unique dedup data block base LBAs */
    uint64_t *bases      = NULL;
    uint64_t  base_count = 0;
    uint64_t  base_cap   = 0;

    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!node_buf)
    {
        free(entries);
        return 0;
    }

    for(uint64_t t = 0; t < tree_count; t++)
    {
        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        uint64_t lba = thdr.root_node_lba;
        while(lba != 0)
        {
            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct dedup_entry de;
                memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));
                if(de.block_lba == 0) continue;

                /* Check uniqueness */
                int found = 0;
                for(uint64_t j = 0; j < base_count; j++)
                {
                    if(bases[j] == de.block_lba)
                    {
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    if(base_count >= base_cap)
                    {
                        base_cap     = (base_cap == 0) ? 256 : base_cap * 2;
                        uint64_t *bt = realloc(bases, base_cap * sizeof(*bt));
                        if(!bt)
                        {
                            free(node_buf);
                            free(entries);
                            free(bases);
                            return 0;
                        }
                        bases = bt;
                    }
                    bases[base_count++] = de.block_lba;
                }
            }

            lba = nhdr.right_link;
        }
    }

    free(node_buf);
    free(entries);

    if(base_count == 0)
    {
        printf("\nDedup data block scrub:\n");
        printf("  No dedup data blocks to scrub.\n");
        free(bases);
        return 0;
    }

    printf("\n  %sDedup data block scrub%s\n", CLR_BOLD, CLR_RESET);
    result_info("Blocks to verify:", "%" PRIu64, base_count);

    size_t   dedup_size = (size_t)ctx->sb.dedup_block_size;
    uint8_t *buf        = calloc(1, dedup_size);
    if(!buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        free(bases);
        return 0;
    }

    uint64_t bad          = 0;
    uint64_t bad_magic    = 0;
    uint64_t bad_checksum = 0;
    uint64_t read_errors  = 0;

    for(uint64_t i = 0; i < base_count; i++)
    {
        if(i % 4 == 0 || i == base_count - 1) print_progress("Dedup blocks", i + 1, base_count, bad);

        /* Read first standard block to get the header */
        rc = obmafs3_block_read(ctx, bases[i], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK)
        {
            read_errors++;
            bad++;
            continue;
        }

        struct block_header bhdr;
        memcpy(&bhdr, buf, sizeof(bhdr));

        if(bhdr.magic != OBMAFS3_BLOCK_MAGIC)
        {
            bad_magic++;
            bad++;
            continue;
        }

        /* Determine actual on-disk payload size */
        size_t check_size =
            (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? (size_t)bhdr.compressed_size : (size_t)bhdr.original_size;
        if(check_size > dedup_size - sizeof(bhdr)) check_size = dedup_size - sizeof(bhdr);

        /* Read remaining standard blocks if payload extends beyond first */
        uint64_t total_on_disk = sizeof(bhdr) + check_size;
        uint64_t bs            = ctx->sb.block_size;
        uint64_t needed_std    = (total_on_disk + bs - 1) / bs;
        if(needed_std > 1)
        {
            rc = obmafs3_block_read(ctx, bases[i] + 1, buf + bs, (size_t)((needed_std - 1) * bs));
            if(rc != OBMAFS3_OK)
            {
                read_errors++;
                bad++;
                continue;
            }
        }

        uint8_t computed[32];
        obmafs3_checksum_block(buf + sizeof(bhdr), check_size, computed);

        if(memcmp(computed, bhdr.checksum, 32) != 0)
        {
            bad_checksum++;
            bad++;
        }
    }

    print_progress("Dedup blocks", base_count, base_count, bad);
    bar_clear();

    if(bad == 0) { result_ok("Result:", ""); }
    else
    {
        result_bad("Result:", "%" PRIu64 " error(s)", bad);
        if(read_errors > 0) printf("    Read errors:    %" PRIu64 "\n", read_errors);
        if(bad_magic > 0) printf("    Bad magic:      %" PRIu64 "\n", bad_magic);
        if(bad_checksum > 0) printf("    Bad checksum:   %" PRIu64 "\n", bad_checksum);
    }

    free(buf);
    free(bases);
    return bad;
}

/* ------------------------------------------------------------------ */
/*  Hash verification (dedup + CD prefix/suffix/subchannel)            */
/* ------------------------------------------------------------------ */

/**
 * Verify CD prefix/suffix/subchannel B+Tree hashes.
 *
 * Each CD record stores an XXH64 hash alongside inline data.  This
 * function walks the leaf nodes of the given tree and recomputes the
 * hash from the inline data, reporting mismatches.
 *
 * @param ctx        Filesystem context.
 * @param hdr        Cached B+Tree header for the tree.
 * @param label      Human-readable tree name for output (e.g. "CD prefix").
 * @param rec_size   Size of one leaf record (hash + inline data).
 * @param data_size  Size of the inline data portion after the hash.
 * @return Number of hash mismatches detected.
 */
static uint64_t verify_cd_tree_hashes(struct obmafs3_ctx *ctx, const struct btree_header *hdr, const char *label,
                                      size_t rec_size, size_t data_size)
{
    printf("\n  %s%s hash verification%s\n", CLR_BOLD, label, CLR_RESET);

    if(hdr->root_node_lba == 0)
    {
        result_info("Status:", "no entries to verify");
        return 0;
    }

    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!node_buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        return 0;
    }

    /* First pass: count total entries for progress reporting */
    uint64_t total_entries = 0;
    {
        uint64_t lba = hdr->root_node_lba;
        /* Descend to left-most leaf */
        while(lba != 0)
        {
            int rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                /* Index node: follow first child */
                struct btree_index_entry ie;
                memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                /* Leaf level: walk right links and count */
                while(lba != 0)
                {
                    rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;
                    memcpy(&nhdr, node_buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;
                    total_entries += nhdr.node_keys;
                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

    if(total_entries == 0)
    {
        result_info("Status:", "no entries to verify");
        free(node_buf);
        return 0;
    }

    result_info("Entries to verify:", "%" PRIu64, total_entries);

    /* Second pass: verify hashes */
    uint64_t checked   = 0;
    uint64_t bad       = 0;
    uint64_t lba       = hdr->root_node_lba;

    /* Descend to left-most leaf */
    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, node_buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        if(nhdr.level > 0)
        {
            struct btree_index_entry ie;
            memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
            lba = ie.child_lba;
        }
        else
        {
            /* Leaf level: walk right links and verify */
            while(lba != 0)
            {
                rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK) break;
                memcpy(&nhdr, node_buf, sizeof(nhdr));
                if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                const uint8_t *rp = node_buf + sizeof(struct btree_node_header);
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    uint64_t stored_hash;
                    memcpy(&stored_hash, rp + i * rec_size, sizeof(stored_hash));
                    const uint8_t *data_ptr = rp + i * rec_size + sizeof(uint64_t);

                    uint64_t computed = obmafs3_checksum_xxh64(data_ptr, data_size);
                    if(computed != stored_hash) bad++;

                    checked++;
                    if(checked % 256 == 0 || checked == total_entries) print_progress("CD hash verify", checked, total_entries, bad);
                }

                lba = nhdr.right_link;
            }
            break;
        }
    }

    print_progress("CD hash verify", total_entries, total_entries, bad);
    bar_clear();

    if(bad == 0)
        result_ok("Result:", "");
    else
        result_bad("Result:", "%" PRIu64 " hash mismatch(es)", bad);

    free(node_buf);
    return bad;
}

/**
 * Verify dedup entry hashes against the actual stored sector data.
 *
 * Walks all dedup B+Trees (one per sector size).  For each dedup_entry,
 * reads the sector data from the dedup data block at (block_lba,
 * block_offset), recomputes the XXH64 hash, and compares it to the
 * stored hash.
 *
 * @param ctx  Filesystem context.
 * @return Number of hash mismatches detected.
 */
static uint64_t verify_dedup_hashes(struct obmafs3_ctx *ctx)
{
    printf("\n  %sDedup hash verification%s\n", CLR_BOLD, CLR_RESET);

    if(ctx->sb.dedup_lba == 0)
    {
        result_info("Status:", "no dedup entries to verify");
        return 0;
    }

    /* Read the tree list header */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        return 0;
    }

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "  Error: could not read dedup tree list: %d\n", rc);
        free(list_buf);
        return 0;
    }

    struct tree_list_header list_hdr;
    memcpy(&list_hdr, list_buf, sizeof(list_hdr));
    if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC || list_hdr.tree_count == 0)
    {
        free(list_buf);
        result_info("Status:", "no dedup entries to verify");
        return 0;
    }

    uint64_t                tree_count = list_hdr.tree_count;
    struct tree_list_entry *entries    = malloc((size_t)(tree_count * sizeof(struct tree_list_entry)));
    if(!entries)
    {
        free(list_buf);
        return 0;
    }
    memcpy(entries, list_buf + sizeof(struct tree_list_header), (size_t)(tree_count * sizeof(struct tree_list_entry)));
    free(list_buf);

    /* First pass: count total dedup entries for progress */
    uint64_t total_entries = 0;
    uint8_t *node_buf      = calloc(1, (size_t)ctx->sb.block_size);
    if(!node_buf)
    {
        free(entries);
        return 0;
    }

    for(uint64_t t = 0; t < tree_count; t++)
    {
        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        uint64_t lba = thdr.root_node_lba;

        /* Descend to left-most leaf */
        while(lba != 0)
        {
            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                struct btree_index_entry ie;
                memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                while(lba != 0)
                {
                    rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;
                    memcpy(&nhdr, node_buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;
                    total_entries += nhdr.node_keys;
                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

    if(total_entries == 0)
    {
        result_info("Status:", "no dedup entries to verify");
        free(node_buf);
        free(entries);
        return 0;
    }

    result_info("Entries to verify:", "%" PRIu64, total_entries);

    /* Allocate buffers for reading dedup data blocks */
    size_t   dedup_size = (size_t)ctx->sb.dedup_block_size;
    uint8_t *dedup_buf  = calloc(1, dedup_size);
    uint8_t *decomp_buf = NULL;
    if(!dedup_buf)
    {
        fprintf(stderr, "  Error: out of memory\n");
        free(node_buf);
        free(entries);
        return 0;
    }

    uint64_t checked         = 0;
    uint64_t bad             = 0;
    uint64_t read_errors     = 0;
    uint64_t cached_dedup_lba = 0;
    int      cached_compressed = 0;

    /* Second pass: verify each entry */
    for(uint64_t t = 0; t < tree_count; t++)
    {
        uint16_t sector_size = entries[t].sector_size;

        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        uint64_t lba = thdr.root_node_lba;

        /* Descend to left-most leaf */
        while(lba != 0)
        {
            rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                struct btree_index_entry ie;
                memcpy(&ie, node_buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                while(lba != 0)
                {
                    rc = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;
                    memcpy(&nhdr, node_buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                    const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
                    for(uint16_t i = 0; i < nhdr.node_keys; i++)
                    {
                        struct dedup_entry de;
                        memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));

                        if(de.block_lba == 0)
                        {
                            checked++;
                            continue;
                        }

                        /* Read the dedup data block if not cached */
                        if(de.block_lba != cached_dedup_lba)
                        {
                            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf, (size_t)ctx->sb.block_size);
                            if(rc != OBMAFS3_OK)
                            {
                                read_errors++;
                                bad++;
                                checked++;
                                cached_dedup_lba = 0;
                                if(checked % 256 == 0 || checked == total_entries) print_progress("Dedup hash verify", checked, total_entries, bad);
                                continue;
                            }

                            struct block_header bhdr;
                            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

                            uint64_t payload_size;
                            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                                payload_size = bhdr.compressed_size;
                            else
                                payload_size = bhdr.original_size;

                            uint64_t total_on_disk = sizeof(bhdr) + payload_size;
                            uint64_t bs            = ctx->sb.block_size;
                            uint64_t needed_std    = (total_on_disk + bs - 1) / bs;

                            if(needed_std > 1)
                            {
                                rc = obmafs3_block_read(ctx, de.block_lba + 1, dedup_buf + bs, (size_t)((needed_std - 1) * bs));
                                if(rc != OBMAFS3_OK)
                                {
                                    read_errors++;
                                    bad++;
                                    checked++;
                                    cached_dedup_lba = 0;
                                    if(checked % 256 == 0 || checked == total_entries) print_progress("Dedup hash verify", checked, total_entries, bad);
                                    continue;
                                }
                            }

                            cached_dedup_lba = de.block_lba;

                            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                            {
                                if(!decomp_buf)
                                {
                                    decomp_buf = malloc(dedup_size);
                                    if(!decomp_buf)
                                    {
                                        fprintf(stderr, "\n  Error: out of memory for decompression buffer\n");
                                        goto done;
                                    }
                                }

                                ZSTD_DCtx *dctx = ZSTD_createDCtx();
                                if(!dctx)
                                {
                                    fprintf(stderr, "\n  Error: cannot create ZSTD decompression context\n");
                                    goto done;
                                }

                                size_t dret = ZSTD_decompressDCtx(dctx, decomp_buf, dedup_size,
                                                                  dedup_buf + sizeof(bhdr), (size_t)bhdr.compressed_size);
                                ZSTD_freeDCtx(dctx);

                                if(ZSTD_isError(dret))
                                {
                                    read_errors++;
                                    bad++;
                                    checked++;
                                    cached_dedup_lba = 0;
                                    if(checked % 256 == 0 || checked == total_entries) print_progress("Dedup hash verify", checked, total_entries, bad);
                                    continue;
                                }
                                cached_compressed = 1;
                            }
                            else
                            {
                                cached_compressed = 0;
                            }
                        }

                        /* Extract the sector data and compute hash */
                        const uint8_t *sector_data;
                        if(cached_compressed)
                        {
                            size_t decomp_off = (size_t)(de.block_offset - sizeof(struct block_header));
                            sector_data = decomp_buf + decomp_off;
                        }
                        else
                        {
                            sector_data = dedup_buf + de.block_offset;
                        }

                        uint64_t computed = obmafs3_checksum_xxh64(sector_data, (size_t)sector_size);
                        if(computed != de.hash) bad++;

                        checked++;
                        if(checked % 256 == 0 || checked == total_entries) print_progress("Dedup hash verify", checked, total_entries, bad);
                    }

                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

done:
    print_progress("Dedup hash verify", total_entries, total_entries, bad);
    bar_clear();

    if(bad == 0)
        result_ok("Result:", "");
    else
    {
        result_bad("Result:", "%" PRIu64 " error(s)", bad);
        if(read_errors > 0) printf("    Read/decomp errors: %" PRIu64 "\n", read_errors);
        uint64_t hash_bad = bad - read_errors;
        if(hash_bad > 0) printf("    Hash mismatches:    %" PRIu64 "\n", hash_bad);
    }

    free(decomp_buf);
    free(dedup_buf);
    free(node_buf);
    free(entries);
    return bad;
}

/* ------------------------------------------------------------------ */
/*  Dedup statistics                                                   */
/* ------------------------------------------------------------------ */

/**
 * Print a byte count as a human-readable string (B/KiB/MiB/GiB/TiB).
 *
 * @param bytes  Number of bytes to format.
 */
static void print_human_size(uint64_t bytes)
{
    if(bytes >= 1099511627776ULL)
        printf("%.2f TiB", (double)bytes / 1099511627776.0);
    else if(bytes >= 1073741824ULL)
        printf("%.2f GiB", (double)bytes / 1073741824.0);
    else if(bytes >= 1048576ULL)
        printf("%.2f MiB", (double)bytes / 1048576.0);
    else if(bytes >= 1024ULL)
        printf("%.2f KiB", (double)bytes / 1024.0);
    else
        printf("%" PRIu64 " B", bytes);
}

struct dedup_tree_stats
{
    uint16_t sector_size;
    uint64_t dedup_entries;      /* unique sector hashes in tree */
    uint64_t unique_blocks;      /* unique dedup data blocks */
    uint64_t original_bytes;     /* sum of original_size from block headers */
    uint64_t compressed_bytes;   /* sum of actual on-disk payload size */
    uint64_t physical_bytes;     /* unique_blocks x dedup_block_size */
    uint64_t compressed_count;   /* number of compressed blocks */
    uint64_t uncompressed_count; /* number of uncompressed blocks */
};

/**
 * Compute and print deduplication and compression statistics.
 *
 * Walks the dedup tree list, enumerates every dedup entry, counts
 * unique data blocks, reads block headers for compression information,
 * and prints per-tree and aggregate statistics including dedup ratio,
 * compression ratio, and total space savings.
 *
 * @param ctx  Filesystem context.
 * @return @c OBMAFS3_OK on success.
 */
static int compute_dedup_stats(struct obmafs3_ctx *ctx)
{
    if(ctx->sb.dedup_lba == 0)
    {
        printf("\n  %sDedup statistics%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup trees found");
        return 0;
    }

    /* Read the tree list header */
    uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!list_buf) return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
    if(rc != OBMAFS3_OK)
    {
        free(list_buf);
        return rc;
    }

    struct tree_list_header tlhdr;
    memcpy(&tlhdr, list_buf, sizeof(tlhdr));
    if(tlhdr.magic != OBMAFS3_TREELIST_MAGIC || tlhdr.tree_count == 0)
    {
        free(list_buf);
        printf("\n  %sDedup statistics%s\n", CLR_BOLD, CLR_RESET);
        result_info("Status:", "no dedup trees found");
        return 0;
    }

    uint64_t                tree_count = tlhdr.tree_count;
    struct tree_list_entry *tl_entries = malloc((size_t)(tree_count * sizeof(struct tree_list_entry)));
    if(!tl_entries)
    {
        free(list_buf);
        return OBMAFS3_ERR_NOMEM;
    }
    memcpy(tl_entries, list_buf + sizeof(struct tree_list_header),
           (size_t)(tree_count * sizeof(struct tree_list_entry)));
    free(list_buf);

    struct dedup_tree_stats *stats = calloc((size_t)tree_count, sizeof(*stats));
    if(!stats)
    {
        free(tl_entries);
        return OBMAFS3_ERR_NOMEM;
    }

    /* ---- Walk media image inodes to count sector maps ---- */
    uint64_t total_media_files        = 0;
    uint64_t total_media_file_size    = 0;
    uint64_t total_sector_map_entries = 0;
    uint64_t total_sector_count       = 0;

    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
        if(buf)
        {
            uint64_t *stack  = malloc(64 * sizeof(uint64_t));
            uint64_t  stk_sz = 0, stk_cap = 64;
            if(stack)
            {
                stack[stk_sz++] = ctx->inode_hdr.root_node_lba;
                while(stk_sz > 0)
                {
                    uint64_t lba = stack[--stk_sz];
                    rc           = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) break;

                    struct btree_node_header hdr;
                    memcpy(&hdr, buf, sizeof(hdr));
                    if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                    if(hdr.level > 0)
                    {
                        for(uint16_t i = 0; i < hdr.node_keys; i++)
                        {
                            struct btree_index_entry ie;
                            memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                            if(stk_sz >= stk_cap)
                            {
                                stk_cap *= 2;
                                uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                                if(!tmp) break;
                                stack = tmp;
                            }
                            stack[stk_sz++] = ie.child_lba;
                        }
                        continue;
                    }

                    /* Leaf node */
                    for(uint16_t i = 0; i < hdr.node_keys; i++)
                    {
                        struct inode_record rec;
                        memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));
                        if(rec.file_type == kFileTypeMediaImage)
                        {
                            total_media_files++;
                            total_media_file_size += rec.file_size;
                            total_sector_map_entries += rec.sector_map_size;
                            total_sector_count += rec.sector_count;
                        }
                    }
                }
                free(stack);
            }
            free(buf);
        }
    }

    /* ---- Walk each dedup tree ---- */
    for(uint64_t t = 0; t < tree_count; t++)
    {
        stats[t].sector_size = tl_entries[t].sector_size;

        struct btree_header thdr;
        rc = obmafs3_btree_header_read(ctx, tl_entries[t].tree_lba, &thdr);
        if(rc != OBMAFS3_OK) continue;

        /* Unique data block base LBAs */
        uint64_t *bases      = NULL;
        uint64_t  base_count = 0;
        uint64_t  base_cap   = 0;

        uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(!node_buf) continue;

        /* DFS walk */
        uint64_t *stk    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_sz = 0, stk_cap = 64;
        if(!stk)
        {
            free(node_buf);
            continue;
        }

        stk[stk_sz++] = thdr.root_node_lba;

        uint64_t nodes_visited = 0;

        while(stk_sz > 0)
        {
            uint64_t lba = stk[--stk_sz];

            nodes_visited++;
            {
                char pfx[80];
                snprintf(pfx, sizeof(pfx), "Dedup stats [tree %" PRIu64 "/%" PRIu64 "] nodes",
                         t + 1, tree_count);
                print_bar(pfx, nodes_visited, (uint64_t)thdr.total_nodes);
            }

            rc           = obmafs3_block_read(ctx, lba, node_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, node_buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, node_buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_sz >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stk, stk_cap * sizeof(*tmp));
                        if(!tmp) break;
                        stk = tmp;
                    }
                    stk[stk_sz++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf node: count entries and collect unique block LBAs */
            const uint8_t *ep = node_buf + sizeof(struct btree_node_header);
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct dedup_entry de;
                memcpy(&de, ep + i * sizeof(struct dedup_entry), sizeof(de));

                stats[t].dedup_entries++;

                if(de.block_lba == 0) continue;

                /* Check if this base LBA is already recorded */
                int found = 0;
                for(uint64_t j = 0; j < base_count; j++)
                {
                    if(bases[j] == de.block_lba)
                    {
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    if(base_count >= base_cap)
                    {
                        base_cap     = (base_cap == 0) ? 256 : base_cap * 2;
                        uint64_t *bt = realloc(bases, base_cap * sizeof(*bt));
                        if(!bt) break;
                        bases = bt;
                    }
                    bases[base_count++] = de.block_lba;
                }
            }
        }

        free(stk);
        free(node_buf);

        stats[t].unique_blocks  = base_count;
        stats[t].physical_bytes = 0;

        /* Read each unique data block header for compression stats
         * and compute actual physical allocation per block */
        if(base_count > 0)
        {
            uint8_t *hdr_buf = calloc(1, (size_t)ctx->sb.block_size);
            if(hdr_buf)
            {
                for(uint64_t b = 0; b < base_count; b++)
                {
                    {
                        char pfx[80];
                        snprintf(pfx, sizeof(pfx), "Dedup stats [tree %" PRIu64 "/%" PRIu64 "] blocks",
                                 t + 1, tree_count);
                        print_bar(pfx, b + 1, base_count);
                    }

                    rc = obmafs3_block_read(ctx, bases[b], hdr_buf, (size_t)ctx->sb.block_size);
                    if(rc != OBMAFS3_OK) continue;

                    struct block_header bhdr;
                    memcpy(&bhdr, hdr_buf, sizeof(bhdr));
                    if(bhdr.magic != OBMAFS3_BLOCK_MAGIC) continue;

                    stats[t].original_bytes += bhdr.original_size;

                    uint64_t payload_size;
                    if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                    {
                        stats[t].compressed_bytes += bhdr.compressed_size;
                        stats[t].compressed_count++;
                        payload_size = bhdr.compressed_size;
                    }
                    else
                    {
                        stats[t].compressed_bytes += bhdr.original_size;
                        stats[t].uncompressed_count++;
                        payload_size = bhdr.original_size;
                    }

                    /* Actual on-disk allocation: header + payload,
                     * rounded up to block_size */
                    uint64_t on_disk = sizeof(bhdr) + payload_size;
                    uint64_t bs      = ctx->sb.block_size;
                    stats[t].physical_bytes += ((on_disk + bs - 1) / bs) * bs;
                }
                free(hdr_buf);
            }
        }

        free(bases);
    }

    /* Clear progress line */
    bar_clear();

    /* ---- Print report ---- */
    printf("\n  %sDedup statistics%s\n", CLR_BOLD, CLR_RESET);
    printf("  Media image files:        %" PRIu64 "\n", total_media_files);
    printf("  Total logical size:       ");
    print_human_size(total_media_file_size);
    printf(" (%" PRIu64 " bytes)\n", total_media_file_size);
    printf("  Total sector refs:        %" PRIu64 "\n", total_sector_map_entries);
    printf("  Dedup block size:         ");
    print_human_size(ctx->sb.dedup_block_size);
    printf("\n");

    uint64_t grand_dedup_entries     = 0;
    uint64_t grand_unique_blocks     = 0;
    uint64_t grand_unique_sector_bytes = 0;
    uint64_t grand_original          = 0;
    uint64_t grand_compressed        = 0;
    uint64_t grand_physical          = 0;
    uint64_t grand_compressed_blks   = 0;
    uint64_t grand_uncompressed_blks = 0;

    for(uint64_t t = 0; t < tree_count; t++)
    {
        printf("\n  Tree %" PRIu64 " (sector size: %" PRIu16 " bytes):\n", t, stats[t].sector_size);
        printf("    Unique sectors:         %" PRIu64 "\n", stats[t].dedup_entries);

        uint64_t dedup_sector_bytes = stats[t].dedup_entries * (uint64_t)stats[t].sector_size;
        printf("    Unique sector data:     ");
        print_human_size(dedup_sector_bytes);
        printf("\n");

        printf("    Dedup data blocks:      %" PRIu64 "\n", stats[t].unique_blocks);
        printf("      Compressed:           %" PRIu64 "\n", stats[t].compressed_count);
        printf("      Uncompressed:         %" PRIu64 "\n", stats[t].uncompressed_count);

        printf("    Uncompressed data:      ");
        print_human_size(stats[t].original_bytes);
        printf("\n");
        printf("    Compressed data:        ");
        print_human_size(stats[t].compressed_bytes);
        printf("\n");
        printf("    Physical allocation:    ");
        print_human_size(stats[t].physical_bytes);
        printf("\n");

        if(stats[t].original_bytes > 0 && stats[t].compressed_bytes < stats[t].original_bytes)
        {
            double comp_ratio = (double)stats[t].original_bytes / (double)stats[t].compressed_bytes;
            double comp_saved = (1.0 - (double)stats[t].compressed_bytes / (double)stats[t].original_bytes) * 100.0;
            printf("    Compression ratio:      %.2f:1 (%.1f%% smaller)\n", comp_ratio, comp_saved);
        }

        grand_dedup_entries += stats[t].dedup_entries;
        grand_unique_blocks += stats[t].unique_blocks;
        grand_unique_sector_bytes += stats[t].dedup_entries * (uint64_t)stats[t].sector_size;
        grand_original += stats[t].original_bytes;
        grand_compressed += stats[t].compressed_bytes;
        grand_physical += stats[t].physical_bytes;
        grand_compressed_blks += stats[t].compressed_count;
        grand_uncompressed_blks += stats[t].uncompressed_count;
    }

    if(tree_count > 1)
    {
        printf("\n  Totals across all trees:\n");
        printf("    Unique sectors:         %" PRIu64 "\n", grand_dedup_entries);
        printf("    Dedup data blocks:      %" PRIu64 " (%" PRIu64 " compressed, %" PRIu64 " uncompressed)\n",
               grand_unique_blocks, grand_compressed_blks, grand_uncompressed_blks);
        printf("    Uncompressed data:      ");
        print_human_size(grand_original);
        printf("\n");
        printf("    Compressed data:        ");
        print_human_size(grand_compressed);
        printf("\n");
        printf("    Physical allocation:    ");
        print_human_size(grand_physical);
        printf("\n");
    }

    /* ---- Savings summary ---- */
    printf("\n  Savings summary:\n");

    if(total_sector_map_entries > 0 && grand_dedup_entries > 0)
    {
        double   dedup_ratio = (double)total_sector_map_entries / (double)grand_dedup_entries;
        uint64_t dup_sectors = total_sector_map_entries - grand_dedup_entries;
        printf("    Dedup ratio:            %.2f:1"
               " (%" PRIu64 " refs -> %" PRIu64 " unique)\n",
               dedup_ratio, total_sector_map_entries, grand_dedup_entries);
        printf("    Duplicate sectors:      %" PRIu64 "\n", dup_sectors);

        if(total_media_file_size > grand_unique_sector_bytes)
        {
            uint64_t bytes_saved_dedup = total_media_file_size - grand_unique_sector_bytes;
            printf("    Saved by dedup:         ");
            print_human_size(bytes_saved_dedup);
            printf(" (%.1f%%)\n", (double)bytes_saved_dedup / (double)total_media_file_size * 100.0);
        }
    }

    if(grand_original > 0 && grand_compressed > 0)
    {
        double comp_ratio = (double)grand_original / (double)grand_compressed;
        printf("    Compression ratio:      %.2f:1\n", comp_ratio);
        if(grand_compressed < grand_original)
        {
            uint64_t comp_saved = grand_original - grand_compressed;
            printf("    Saved by compression:   ");
            print_human_size(comp_saved);
            printf(" (%.1f%%)\n", (double)comp_saved / (double)grand_original * 100.0);
        }
    }

    if(total_media_file_size > 0 && grand_physical > 0)
    {
        double overall = (double)total_media_file_size / (double)grand_physical;
        printf("    Overall ratio:          %.2f:1\n", overall);
        if(total_media_file_size > grand_physical)
        {
            uint64_t total_saved = total_media_file_size - grand_physical;
            printf("    Total space saved:      ");
            print_human_size(total_saved);
            printf(" (%.1f%%)\n", (double)total_saved / (double)total_media_file_size * 100.0);
        }
    }

    free(stats);
    free(tl_entries);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Metadata tree bidirectional consistency                             */
/* ------------------------------------------------------------------ */

/**
 * A metadata triple collected from a leaf node.
 * Used for bidirectional comparison between metadata and metadata index trees.
 */
struct meta_triple
{
    uint64_t inode_id;
    char     key[METADATA_KEY_MAX];
    char     value[METADATA_VALUE_MAX];
};

/** qsort comparator for meta_triple: (inode_id, key, value). */
static int cmp_meta_triple(const void *a, const void *b)
{
    const struct meta_triple *ta = (const struct meta_triple *)a;
    const struct meta_triple *tb = (const struct meta_triple *)b;
    if(ta->inode_id < tb->inode_id) return -1;
    if(ta->inode_id > tb->inode_id) return 1;
    int kc = strncmp(ta->key, tb->key, METADATA_KEY_MAX);
    if(kc != 0) return kc;
    return strncmp(ta->value, tb->value, METADATA_VALUE_MAX);
}

/**
 * Walk all metadata tree leaf nodes and collect every (inode_id, key, value) tuple.
 *
 * @param ctx       Filesystem context.
 * @param out       Output: heap-allocated array of meta_triple.
 * @param out_count Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_metadata_records(struct obmafs3_ctx *ctx,
                                    struct meta_triple **out, uint64_t *out_count)
{
    *out       = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->metadata_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_triple *recs = NULL;
    uint64_t count = 0, cap = 0;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(recs); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct metadata_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect metadata records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                struct meta_triple *tmp = realloc(recs, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                recs = tmp;
            }
            struct metadata_record mrec;
            memcpy(&mrec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(mrec), sizeof(mrec));
            recs[count].inode_id = mrec.inode_id;
            memcpy(recs[count].key, mrec.key, METADATA_KEY_MAX);
            memcpy(recs[count].value, mrec.value, METADATA_VALUE_MAX);
            count++;
        }
    }

    free(buf);
    free(stack);
    *out       = recs;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Walk all metadata index tree leaf nodes and collect every (key, value, inode_id) tuple.
 *
 * @param ctx       Filesystem context.
 * @param out       Output: heap-allocated array of meta_triple.
 * @param out_count Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_metadata_idx_records(struct obmafs3_ctx *ctx,
                                        struct meta_triple **out, uint64_t *out_count)
{
    *out       = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->metadata_idx_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t   node_sz = (size_t)METADATA_NODE_BLOCKS * ctx->sb.block_size;
    uint8_t *buf     = calloc(1, node_sz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct meta_triple *recs = NULL;
    uint64_t count = 0, cap = 0;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, node_sz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(recs); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct metadata_idx_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect metadata index records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                struct meta_triple *tmp = realloc(recs, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(recs); return OBMAFS3_ERR_NOMEM; }
                recs = tmp;
            }
            struct metadata_idx_record irec;
            memcpy(&irec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(irec), sizeof(irec));
            recs[count].inode_id = irec.inode_id;
            memcpy(recs[count].key, irec.key, METADATA_KEY_MAX);
            memcpy(recs[count].value, irec.value, METADATA_VALUE_MAX);
            count++;
        }
    }

    free(buf);
    free(stack);
    *out       = recs;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Binary search for a meta_triple in a sorted array.
 *
 * @param arr    Sorted array of meta_triple.
 * @param count  Number of elements.
 * @param t      Triple to search for.
 * @return Non-zero if found.
 */
static int meta_triple_sorted_contains(const struct meta_triple *arr, uint64_t count,
                                       const struct meta_triple *t)
{
    uint64_t lo = 0, hi = count;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = cmp_meta_triple(&arr[mid], t);
        if(c < 0)      lo = mid + 1;
        else if(c > 0)  hi = mid;
        else             return 1;
    }
    return 0;
}

/**
 * Check bidirectional consistency between the metadata tree and the
 * metadata index tree.
 *
 * Detects:
 * - Entries in the metadata tree with no corresponding record in the
 *   metadata index tree.  Fixed by re-inserting via metadata_put.
 * - Entries in the metadata index tree with no corresponding record in
 *   the metadata tree.  Fixed by re-inserting via metadata_put.
 *
 * @param ctx       Filesystem context.
 * @param auto_yes  If nonzero, always repair.
 * @param auto_no   If nonzero, never repair.
 * @param errors    In/out: incremented for each unfixed error.
 */
static void check_metadata_bidirectional(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors)
{
    struct meta_triple *meta_recs = NULL, *idx_recs = NULL;
    uint64_t meta_count = 0, idx_count = 0;

    printf("\n  %sMetadata consistency%s\n", CLR_BOLD, CLR_RESET);

    int rc = collect_metadata_records(ctx, &meta_recs, &meta_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Metadata tree:", "could not walk (%d)", rc);
        (*errors)++;
        return;
    }

    rc = collect_metadata_idx_records(ctx, &idx_recs, &idx_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Metadata index:", "could not walk (%d)", rc);
        free(meta_recs);
        (*errors)++;
        return;
    }

    /* Sort both arrays by (inode_id, key, value) */
    if(meta_count > 0) qsort(meta_recs, (size_t)meta_count, sizeof(meta_recs[0]), cmp_meta_triple);
    if(idx_count > 0)  qsort(idx_recs,  (size_t)idx_count,  sizeof(idx_recs[0]),  cmp_meta_triple);

    /* ---- Phase 1: entries in metadata but not in index ---- */
    uint64_t missing_from_idx = 0, fixed_idx = 0;
    for(uint64_t i = 0; i < meta_count; i++)
    {
        if(!meta_triple_sorted_contains(idx_recs, idx_count, &meta_recs[i]))
            missing_from_idx++;
    }

    if(missing_from_idx > 0)
    {
        result_bad("Missing from index:", "%" PRIu64 " record(s)", missing_from_idx);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Re-insert missing entries into metadata index?"))
        {
            for(uint64_t i = 0; i < meta_count; i++)
            {
                if(!meta_triple_sorted_contains(idx_recs, idx_count, &meta_recs[i]))
                {
                    rc = obmafs3_metadata_put(ctx, meta_recs[i].inode_id,
                                              meta_recs[i].key, meta_recs[i].value);
                    if(rc == OBMAFS3_OK)
                        fixed_idx++;
                    else
                        printf("    Error: could not re-insert inode %" PRIu64 " key '%s' (%d)\n",
                               meta_recs[i].inode_id, meta_recs[i].key, rc);
                }
            }
            if(fixed_idx == missing_from_idx)
            {
                result_fixed("Index entries:", "%" PRIu64 " fixed", fixed_idx);
                (*errors)--;
            }
            else
            {
                result_fixed("Index entries:", "%" PRIu64 " of %" PRIu64 " fixed",
                             fixed_idx, missing_from_idx);
            }
        }
    }

    /* ---- Phase 2: entries in index but not in metadata ---- */
    uint64_t missing_from_meta = 0, fixed_meta = 0;
    for(uint64_t i = 0; i < idx_count; i++)
    {
        if(!meta_triple_sorted_contains(meta_recs, meta_count, &idx_recs[i]))
            missing_from_meta++;
    }

    if(missing_from_meta > 0)
    {
        result_bad("Missing from meta:", "%" PRIu64 " record(s)", missing_from_meta);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Re-insert missing entries into metadata tree?"))
        {
            for(uint64_t i = 0; i < idx_count; i++)
            {
                if(!meta_triple_sorted_contains(meta_recs, meta_count, &idx_recs[i]))
                {
                    rc = obmafs3_metadata_put(ctx, idx_recs[i].inode_id,
                                              idx_recs[i].key, idx_recs[i].value);
                    if(rc == OBMAFS3_OK)
                        fixed_meta++;
                    else
                        printf("    Error: could not re-insert inode %" PRIu64 " key '%s' (%d)\n",
                               idx_recs[i].inode_id, idx_recs[i].key, rc);
                }
            }
            if(fixed_meta == missing_from_meta)
            {
                result_fixed("Meta entries:", "%" PRIu64 " fixed", fixed_meta);
                (*errors)--;
            }
            else
            {
                result_fixed("Meta entries:", "%" PRIu64 " of %" PRIu64 " fixed",
                             fixed_meta, missing_from_meta);
            }
        }
    }

    /* ---- Summary ---- */
    if(missing_from_idx == 0 && missing_from_meta == 0)
        result_ok("Status:", "%" PRIu64 " record(s)", meta_count);

    free(meta_recs);
    free(idx_recs);
}

/* ------------------------------------------------------------------ */
/*  Cross-reference: orphan inodes & dangling catalog entries          */
/* ------------------------------------------------------------------ */

/**
 * Catalog entry reference collected from a catalog leaf node.
 * Stores enough information to identify and delete the entry.
 */
struct catalog_ref
{
    uint64_t inode_id;
    uint64_t parent_id;
    char     name[256];
};

/**
 * Walk all catalog B+Tree leaf nodes and collect every
 * (inode_id, parent_id, name) tuple.
 *
 * @param ctx       Filesystem context.
 * @param out_refs  Output: heap-allocated array of catalog_ref.
 * @param out_count Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_catalog_refs(struct obmafs3_ctx *ctx, struct catalog_ref **out_refs, uint64_t *out_count)
{
    *out_refs  = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->catalog_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct catalog_ref *refs = NULL;
    uint64_t count = 0, cap = 0;

    /* Iterative DFS */
    uint64_t *stack = malloc(64 * sizeof(uint64_t));
    uint64_t stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(refs); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct catalog_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect catalog records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                struct catalog_ref *tmp = realloc(refs, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_NOMEM; }
                refs = tmp;
            }
            struct catalog_record crec;
            memcpy(&crec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(crec), sizeof(crec));
            refs[count].inode_id  = crec.inode_id;
            refs[count].parent_id = crec.parent_id;
            memset(refs[count].name, 0, sizeof(refs[count].name));
            memcpy(refs[count].name, crec.name, sizeof(crec.name));
            count++;
        }
    }

    free(buf);
    free(stack);
    *out_refs  = refs;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Walk all inode B+Tree leaf nodes and collect every inode_id.
 *
 * @param ctx        Filesystem context.
 * @param out_ids    Output: heap-allocated array of inode_id values.
 * @param out_count  Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
static int collect_inode_ids(struct obmafs3_ctx *ctx, uint64_t **out_ids, uint64_t *out_count)
{
    *out_ids   = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *ids = NULL;
    uint64_t count = 0, cap = 0;

    uint64_t *stack = malloc(64 * sizeof(uint64_t));
    uint64_t stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(ids); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect inode_id from each inode_record */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                uint64_t *tmp = realloc(ids, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_NOMEM; }
                ids = tmp;
            }
            struct inode_record irec;
            memcpy(&irec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(irec), sizeof(irec));
            ids[count++] = irec.inode_id;
        }
    }

    free(buf);
    free(stack);
    *out_ids   = ids;
    *out_count = count;
    return OBMAFS3_OK;
}

/** qsort comparator for uint64_t values. */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

/** qsort comparator for catalog_ref by inode_id. */
static int cmp_catalog_ref(const void *a, const void *b)
{
    const struct catalog_ref *ra = (const struct catalog_ref *)a;
    const struct catalog_ref *rb = (const struct catalog_ref *)b;
    return (ra->inode_id < rb->inode_id) ? -1 : (ra->inode_id > rb->inode_id) ? 1 : 0;
}

/** Binary search: return non-zero if @p id is present in the sorted array. */
static int u64_sorted_contains(const uint64_t *arr, uint64_t count, uint64_t id)
{
    uint64_t lo = 0, hi = count;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if(arr[mid] < id)      lo = mid + 1;
        else if(arr[mid] > id) hi = mid;
        else                   return 1;
    }
    return 0;
}

/**
 * Cross-reference the inode and catalog trees.
 *
 * Detects:
 * - **Orphan inodes**: inode_id present in the inode tree but not
 *   referenced by any catalog entry.  Fixed by deleting the inode.
 * - **Dangling catalog entries**: a catalog entry whose inode_id does
 *   not exist in the inode tree.  Fixed by deleting the catalog entry.
 *
 * @param ctx        Filesystem context.
 * @param auto_yes   If nonzero, always repair without prompting.
 * @param auto_no    If nonzero, never repair.
 * @param errors     In/out: incremented for each unfixed error.
 */
static void cross_check_inodes_catalog(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors)
{
    struct catalog_ref *cat_refs  = NULL;
    uint64_t           *inode_ids = NULL;
    uint64_t            cat_count = 0, ino_count = 0;
    int                 rc;

    printf("\n  %sInode / Catalog cross-reference%s\n", CLR_BOLD, CLR_RESET);

    rc = collect_catalog_refs(ctx, &cat_refs, &cat_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Catalog tree:", "could not walk (%d)", rc);
        (*errors)++;
        return;
    }

    rc = collect_inode_ids(ctx, &inode_ids, &ino_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Inode tree:", "could not walk (%d)", rc);
        free(cat_refs);
        (*errors)++;
        return;
    }

    /* Build sorted inode_id set from catalog refs */
    uint64_t *cat_ids = NULL;
    uint64_t  cat_unique = 0;
    if(cat_count > 0)
    {
        qsort(cat_refs, (size_t)cat_count, sizeof(cat_refs[0]), cmp_catalog_ref);

        /* Deduplicate to get unique inode_ids referenced by catalog */
        cat_ids = malloc(cat_count * sizeof(*cat_ids));
        if(cat_ids)
        {
            cat_ids[0] = cat_refs[0].inode_id;
            cat_unique = 1;
            for(uint64_t i = 1; i < cat_count; i++)
            {
                if(cat_refs[i].inode_id != cat_ids[cat_unique - 1])
                    cat_ids[cat_unique++] = cat_refs[i].inode_id;
            }
        }
    }

    /* Sort inode_ids */
    if(ino_count > 0)
        qsort(inode_ids, (size_t)ino_count, sizeof(inode_ids[0]), cmp_u64);

    /* ---- Detect orphan inodes ---- */
    uint64_t orphan_count = 0;
    if(inode_ids && cat_ids)
    {
        for(uint64_t i = 0; i < ino_count; i++)
        {
            uint64_t id = inode_ids[i];
            if(id == OBMAFS3_ROOT_INODE_ID) continue; /* root dir always exists */
            if(!u64_sorted_contains(cat_ids, cat_unique, id))
                orphan_count++;
        }
    }

    if(orphan_count == 0)
    {
        result_ok("Orphan inodes:", "");
    }
    else
    {
        result_bad("Orphan inodes:", "%" PRIu64 " found", orphan_count);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Re-link orphan inodes into lost+found?"))
        {
            /* Ensure lost+found directory exists under root */
            struct catalog_record lf_cat;
            uint64_t lf_inode_id = 0;
            rc = obmafs3_catalog_lookup(ctx, OBMAFS3_ROOT_INODE_ID, "lost+found", &lf_cat);
            if(rc == OBMAFS3_OK)
            {
                lf_inode_id = lf_cat.inode_id;
            }
            else
            {
                /* Create lost+found directory */
                lf_inode_id = obmafs3_alloc_inode_id(ctx);

                struct catalog_record new_cat;
                memset(&new_cat, 0, sizeof(new_cat));
                new_cat.inode_id       = lf_inode_id;
                new_cat.parent_id      = OBMAFS3_ROOT_INODE_ID;
                new_cat.directory_flag  = 1;
                strncpy(new_cat.name, "lost+found", sizeof(new_cat.name) - 1);
                rc = obmafs3_catalog_insert(ctx, &new_cat);
                if(rc != OBMAFS3_OK)
                {
                    printf("    Error: could not create lost+found directory (%d)\n", rc);
                    goto skip_orphan_fix;
                }

                uint64_t now = (uint64_t)time(NULL);
                struct inode_record lf_inode;
                memset(&lf_inode, 0, sizeof(lf_inode));
                lf_inode.inode_id          = lf_inode_id;
                lf_inode.uid               = 0;
                lf_inode.gid               = 0;
                lf_inode.mode              = 0755;
                lf_inode.creation_time     = now;
                lf_inode.modification_time = now;
                lf_inode.access_time       = now;
                lf_inode.file_type         = kFileTypeDirectory;
                lf_inode.ref_count         = 1;

                rc = obmafs3_inode_put(ctx, &lf_inode);
                if(rc != OBMAFS3_OK)
                {
                    printf("    Error: could not create lost+found inode (%d)\n", rc);
                    goto skip_orphan_fix;
                }
                printf("    Created lost+found directory (inode %" PRIu64 ")\n", lf_inode_id);
            }

            /* Re-link each orphan inode into lost+found */
            uint64_t fixed = 0;
            for(uint64_t i = 0; i < ino_count; i++)
            {
                uint64_t id = inode_ids[i];
                if(id == OBMAFS3_ROOT_INODE_ID) continue;
                if(id == lf_inode_id) continue;  /* skip lost+found itself */
                if(!u64_sorted_contains(cat_ids, cat_unique, id))
                {
                    /* Read the inode to determine file type */
                    struct inode_record irec;
                    rc = obmafs3_inode_get(ctx, id, &irec);
                    if(rc != OBMAFS3_OK) continue;

                    /* Build a name: "inode_<id>" */
                    char name_buf[64];
                    snprintf(name_buf, sizeof(name_buf), "inode_%" PRIu64, id);

                    struct catalog_record new_entry;
                    memset(&new_entry, 0, sizeof(new_entry));
                    new_entry.inode_id      = id;
                    new_entry.parent_id     = lf_inode_id;
                    new_entry.directory_flag = (irec.file_type == kFileTypeDirectory) ? 1 : 0;
                    strncpy(new_entry.name, name_buf, sizeof(new_entry.name) - 1);

                    rc = obmafs3_catalog_insert(ctx, &new_entry);
                    if(rc == OBMAFS3_OK)
                    {
                        /* Ensure ref_count is at least 1 */
                        if(irec.ref_count == 0)
                        {
                            irec.ref_count = 1;
                            obmafs3_inode_put(ctx, &irec);
                        }
                        fixed++;
                    }
                }
            }
            printf("    Re-linked %" PRIu64 " orphan inode(s) into lost+found.\n", fixed);
            if(fixed == orphan_count) (*errors)--;
        }
    }
skip_orphan_fix:

    /* ---- Detect dangling catalog entries ---- */
    uint64_t dangling_count = 0;
    if(cat_refs && inode_ids)
    {
        for(uint64_t i = 0; i < cat_count; i++)
        {
            if(!u64_sorted_contains(inode_ids, ino_count, cat_refs[i].inode_id))
                dangling_count++;
        }
    }

    if(dangling_count == 0)
    {
        result_ok("Dangling catalog:", "");
    }
    else
    {
        result_bad("Dangling catalog:", "%" PRIu64 " found", dangling_count);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Delete dangling catalog entries?"))
        {
            uint64_t fixed = 0;
            for(uint64_t i = 0; i < cat_count; i++)
            {
                if(!u64_sorted_contains(inode_ids, ino_count, cat_refs[i].inode_id))
                {
                    rc = obmafs3_catalog_delete(ctx, cat_refs[i].parent_id, cat_refs[i].name);
                    if(rc == OBMAFS3_OK) fixed++;
                }
            }
            printf("    Deleted %" PRIu64 " dangling catalog entry(ies).\n", fixed);
            if(fixed == dangling_count) (*errors)--;
        }
    }

    free(cat_ids);
    free(cat_refs);
    free(inode_ids);
}

/* ------------------------------------------------------------------ */
/*  Extent validation                                                  */
/* ------------------------------------------------------------------ */

/**
 * Validate a single extent run.
 *
 * Checks performed:
 * - @c start_block must not be 0 (that is the superblock).
 * - @c start_block + @c block_count must not exceed @p total_blocks.
 * - @c logical_blocks must be >= @c block_count.
 * - If @c block_count is 0, both @c start_block and @c logical_blocks
 *   must also be 0 (unused slot).
 *
 * @param ext          Pointer to the extent run to validate.
 * @param total_blocks Total block count of the filesystem.
 * @param inode_id     Owning inode (for diagnostics).
 * @param slot         0-based slot/overflow index (for diagnostics).
 * @param label        "inline" or "overflow" (for diagnostics).
 * @param bad_count    Incremented for each violation found.
 */
static void validate_extent(const struct extent_run *ext, uint64_t total_blocks, uint64_t inode_id, int slot,
                            const char *label, uint64_t *bad_count)
{
    if(ext->block_count == 0 && ext->start_block == 0 && ext->logical_blocks == 0)
        return; /* unused slot — OK */

    if(ext->block_count == 0)
    {
        printf("    inode %" PRIu64 " %s extent %d: block_count=0 but start_block=%" PRIu64
               " logical_blocks=%" PRIu64 "\n",
               inode_id, label, slot, ext->start_block, ext->logical_blocks);
        (*bad_count)++;
        return;
    }

    if(ext->start_block == 0)
    {
        printf("    inode %" PRIu64 " %s extent %d: start_block=0 (superblock) "
               "block_count=%" PRIu64 "\n",
               inode_id, label, slot, ext->block_count);
        (*bad_count)++;
        return;
    }

    if(ext->start_block + ext->block_count > total_blocks)
    {
        printf("    inode %" PRIu64 " %s extent %d: out of bounds "
               "(start=%" PRIu64 " count=%" PRIu64 " total=%" PRIu64 ")\n",
               inode_id, label, slot, ext->start_block, ext->block_count, total_blocks);
        (*bad_count)++;
    }

    if(ext->logical_blocks < ext->block_count)
    {
        printf("    inode %" PRIu64 " %s extent %d: logical_blocks (%" PRIu64
               ") < block_count (%" PRIu64 ")\n",
               inode_id, label, slot, ext->logical_blocks, ext->block_count);
        (*bad_count)++;
    }
}

/**
 * Walk all inode records and validate inline extents.
 *
 * For each inode leaf record, calls validate_extent() on each of the
 * 8 inline extent slots.  When a bad extent is found the user is
 * offered the option to zero it (clearing both the data reference and
 * the corresponding file_size contribution).
 *
 * @param ctx          Filesystem context.
 * @param total_blocks Total block count of the filesystem.
 * @param auto_yes     If nonzero, always repair.
 * @param auto_no      If nonzero, never repair.
 * @param bad_count    Output: total bad extents found.
 * @param fixed_count  Output: total bad extents cleared.
 */
static void validate_inline_extents(struct obmafs3_ctx *ctx, uint64_t total_blocks, int auto_yes, int auto_no,
                                    uint64_t *bad_count, uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); return; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: validate each inode's inline extents */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            struct inode_record rec;
            memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

            uint64_t this_bad = 0;
            for(int e = 0; e < 8; e++)
                validate_extent(&rec.extents[e], total_blocks, rec.inode_id, e, "inline", &this_bad);

            if(this_bad > 0)
            {
                *bad_count += this_bad;

                if(ask_fix(auto_yes, auto_no, "    Clear bad inline extent(s)?"))
                {
                    int changed = 0;
                    for(int e = 0; e < 8; e++)
                    {
                        struct extent_run *ext = &rec.extents[e];
                        int bad = 0;

                        if(ext->block_count == 0 && ext->start_block == 0 && ext->logical_blocks == 0)
                            continue;
                        if(ext->block_count == 0) bad = 1;
                        if(ext->start_block == 0 && ext->block_count != 0) bad = 1;
                        if(ext->block_count != 0 && ext->start_block + ext->block_count > total_blocks) bad = 1;
                        if(ext->block_count != 0 && ext->logical_blocks < ext->block_count) bad = 1;

                        if(bad)
                        {
                            memset(ext, 0, sizeof(*ext));
                            changed++;
                            (*fixed_count)++;
                        }
                    }
                    if(changed)
                    {
                        /* Re-compute file_size from remaining valid extents */
                        uint64_t logical_total = 0;
                        for(int e = 0; e < 8; e++)
                            logical_total += rec.extents[e].logical_blocks;
                        rec.file_size = logical_total * ctx->sb.block_size;

                        obmafs3_inode_put(ctx, &rec);
                    }
                }
            }
        }
    }

    free(buf);
    free(stack);
}

/**
 * Walk all overflow extent records and validate each one.
 *
 * Reports every overflow extent that fails the same structural checks
 * applied to inline extents.
 *
 * @param ctx          Filesystem context.
 * @param total_blocks Total block count of the filesystem.
 * @param bad_count    Output: total bad overflow extents found.
 */
static void validate_overflow_extents(struct obmafs3_ctx *ctx, uint64_t total_blocks, uint64_t *bad_count)
{
    *bad_count = 0;

    if(ctx->overflow_hdr.root_node_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    uint64_t *stack    = malloc(64 * sizeof(uint64_t));
    uint64_t  stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return; }

    stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

        struct btree_node_header nhdr;
        memcpy(&nhdr, buf, sizeof(nhdr));
        if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

        if(nhdr.level > 0)
        {
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); return; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: validate each overflow_extent */
        const uint8_t *entries = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < nhdr.node_keys; i++)
        {
            struct overflow_extent oe;
            memcpy(&oe, entries + (size_t)i * sizeof(oe), sizeof(oe));

            struct extent_run ext;
            ext.start_block    = oe.start_block;
            ext.block_count    = oe.block_count;
            ext.logical_blocks = oe.logical_count;

            validate_extent(&ext, total_blocks, oe.inode_id, (int)i, "overflow", bad_count);
        }
    }

    free(buf);
    free(stack);
}

/* ------------------------------------------------------------------ */
/*  Refcount tree validation                                           */
/* ------------------------------------------------------------------ */

/**
 * Internal: insert or add to an LBA→count mapping in a sorted array.
 * Returns the new count on success, 0 on allocation failure.
 */
struct lba_count
{
    uint64_t lba;
    uint32_t count;
};

static uint64_t lba_map_add(struct lba_count **map, uint64_t *cap, uint64_t *len, uint64_t lba)
{
    /* Binary search for existing entry */
    uint64_t lo = 0, hi = *len;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if((*map)[mid].lba < lba)
            lo = mid + 1;
        else
            hi = mid;
    }

    if(lo < *len && (*map)[lo].lba == lba)
    {
        (*map)[lo].count++;
        return *len;
    }

    /* Insert new entry */
    if(*len >= *cap)
    {
        uint64_t new_cap         = *cap ? *cap * 2 : 4096;
        struct lba_count *tmp    = realloc(*map, (size_t)(new_cap * sizeof(struct lba_count)));
        if(!tmp) return 0;
        *map = tmp;
        *cap = new_cap;
    }

    if(lo < *len)
        memmove(&(*map)[lo + 1], &(*map)[lo], (size_t)(*len - lo) * sizeof(struct lba_count));

    (*map)[lo].lba   = lba;
    (*map)[lo].count = 1;
    (*len)++;
    return *len;
}

/**
 * Binary search for an LBA in a sorted lba_count array.
 * Returns the index if found, or UINT64_MAX if not.
 */
static uint64_t lba_map_find(const struct lba_count *map, uint64_t len, uint64_t lba)
{
    uint64_t lo = 0, hi = len;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if(map[mid].lba < lba)
            lo = mid + 1;
        else
            hi = mid;
    }
    if(lo < len && map[lo].lba == lba) return lo;
    return UINT64_MAX;
}

/**
 * Verify the refcount tree against actual block sharing across inodes.
 *
 * Phase 1: Walk all inode extents (inline + overflow) and count how
 *          many inodes reference each physical data block.
 * Phase 2: Walk the refcount tree leaf nodes and collect stored records.
 * Phase 3: Compare expected vs stored refcounts, reporting and optionally
 *          fixing mismatches.
 *
 * Blocks referenced by exactly one inode have an implicit refcount of 1
 * and should NOT appear in the refcount tree.  Blocks with refcount > 1
 * must appear with the correct count.
 *
 * @param ctx        Filesystem context.
 * @param auto_yes   If nonzero, always repair.
 * @param auto_no    If nonzero, never repair.
 * @param bad_count  Output: number of mismatches found.
 * @param fix_count  Output: number of mismatches repaired.
 */
static void verify_refcount_tree(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, uint64_t *bad_count,
                                 uint64_t *fix_count)
{
    *bad_count = 0;
    *fix_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    /* ---- Phase 1: build expected refcount map from all inode extents ---- */
    struct lba_count *expected   = NULL;
    uint64_t          exp_cap    = 0;
    uint64_t          exp_len    = 0;

    /* 1a: Inline extents from inode tree */
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack) { free(buf); return; }

        stack[stk_size++] = root_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];
            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp) { free(buf); free(stack); free(expected); return; }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: collect physical blocks from each inode's inline extents */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct inode_record rec;
                memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

                for(int e = 0; e < 8; e++)
                {
                    if(rec.extents[e].block_count == 0 || rec.extents[e].start_block == 0) continue;

                    for(uint64_t b = 0; b < rec.extents[e].block_count; b++)
                    {
                        if(!lba_map_add(&expected, &exp_cap, &exp_len, rec.extents[e].start_block + b))
                        {
                            free(buf);
                            free(stack);
                            free(expected);
                            return;
                        }
                    }
                }
            }
        }

        free(stack);
    }

    /* 1b: Overflow extents */
    if(ctx->overflow_hdr.root_node_lba != 0)
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack) { free(buf); free(expected); return; }

        stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];
            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp) { free(buf); free(stack); free(expected); return; }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: collect physical blocks from overflow extents */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct overflow_extent oe;
                memcpy(&oe, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(oe), sizeof(oe));

                if(oe.block_count == 0 || oe.start_block == 0) continue;

                for(uint64_t b = 0; b < oe.block_count; b++)
                {
                    if(!lba_map_add(&expected, &exp_cap, &exp_len, oe.start_block + b))
                    {
                        free(buf);
                        free(stack);
                        free(expected);
                        return;
                    }
                }
            }
        }

        free(stack);
    }

    /* ---- Phase 2: collect stored refcount records ---- */
    struct lba_count *stored     = NULL;
    uint64_t          sto_cap    = 0;
    uint64_t          sto_len    = 0;

    if(ctx->refcount_hdr.root_node_lba != 0)
    {
        uint64_t lba = ctx->refcount_hdr.root_node_lba;

        /* Descend to left-most leaf */
        while(lba != 0)
        {
            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header), sizeof(ie));
                lba = ie.child_lba;
            }
            else
            {
                /* Walk leaf chain */
                while(lba != 0)
                {
                    if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;
                    memcpy(&nhdr, buf, sizeof(nhdr));
                    if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

                    const uint8_t *rp = buf + sizeof(struct btree_node_header);
                    for(uint16_t i = 0; i < nhdr.node_keys; i++)
                    {
                        struct refcount_record rr;
                        memcpy(&rr, rp + (size_t)i * sizeof(rr), sizeof(rr));

                        if(sto_len >= sto_cap)
                        {
                            sto_cap = sto_cap ? sto_cap * 2 : 256;
                            struct lba_count *tmp = realloc(stored, (size_t)(sto_cap * sizeof(*tmp)));
                            if(!tmp)
                            {
                                free(buf);
                                free(expected);
                                free(stored);
                                return;
                            }
                            stored = tmp;
                        }
                        stored[sto_len].lba   = rr.lba;
                        stored[sto_len].count = rr.ref_count;
                        sto_len++;
                    }

                    lba = nhdr.right_link;
                }
                break;
            }
        }
    }

    /* ---- Phase 3: compare expected vs stored ---- */

    /* 3a: Check blocks that should have refcount > 1 */
    for(uint64_t i = 0; i < exp_len; i++)
    {
        uint32_t exp_rc = expected[i].count;
        if(exp_rc <= 1) continue; /* Implicit refcount 1 — should NOT be in the tree */

        /* Look up in stored records */
        uint64_t si = lba_map_find(stored, sto_len, expected[i].lba);
        if(si == UINT64_MAX)
        {
            /* Should be in tree but is not */
            printf("    LBA %" PRIu64 ": expected refcount %" PRIu32 ", not in tree\n",
                   expected[i].lba, exp_rc);
            (*bad_count)++;
            if(ask_fix(auto_yes, auto_no, "    Insert correct refcount?"))
            {
                int rc = obmafs3_refcount_set(ctx, expected[i].lba, exp_rc);
                if(rc == OBMAFS3_OK)
                    (*fix_count)++;
                else
                    fprintf(stderr, "    Error: could not set refcount: %d\n", rc);
            }
        }
        else if(stored[si].count != exp_rc)
        {
            printf("    LBA %" PRIu64 ": stored refcount %" PRIu32 ", expected %" PRIu32 "\n",
                   expected[i].lba, stored[si].count, exp_rc);
            (*bad_count)++;
            if(ask_fix(auto_yes, auto_no, "    Fix refcount?"))
            {
                int rc = obmafs3_refcount_set(ctx, expected[i].lba, exp_rc);
                if(rc == OBMAFS3_OK)
                    (*fix_count)++;
                else
                    fprintf(stderr, "    Error: could not set refcount: %d\n", rc);
            }
            /* Mark as verified by zeroing (to detect stale entries below) */
            stored[si].count = 0;
        }
        else
        {
            /* Correct — mark as verified */
            stored[si].count = 0;
        }
    }

    /* 3b: Check for stale entries in the refcount tree
     * (entries for blocks that are not shared, or not referenced at all) */
    for(uint64_t i = 0; i < sto_len; i++)
    {
        if(stored[i].count == 0) continue; /* Already verified in 3a */

        /* This entry exists in the tree but shouldn't (block isn't shared) */
        uint64_t ei = lba_map_find(expected, exp_len, stored[i].lba);
        uint32_t actual = (ei != UINT64_MAX) ? expected[ei].count : 0;

        if(actual <= 1)
        {
            printf("    LBA %" PRIu64 ": stale refcount %" PRIu32 " in tree (actual %s)\n",
                   stored[i].lba, stored[i].count,
                   actual == 0 ? "unallocated/unreferenced" : "1");
            (*bad_count)++;
            if(ask_fix(auto_yes, auto_no, "    Remove stale refcount entry?"))
            {
                /* Setting to 1 removes the entry from the tree */
                int rc = obmafs3_refcount_set(ctx, stored[i].lba, 1);
                if(rc == OBMAFS3_OK)
                    (*fix_count)++;
                else
                    fprintf(stderr, "    Error: could not remove refcount: %d\n", rc);
            }
        }
    }

    free(buf);
    free(expected);
    free(stored);
}

/**
 * Verify that each inode's file_size matches the sum of its extent
 * logical blocks (inline + overflow) multiplied by block_size.
 *
 * Directories (file_type == kFileTypeDirectory) are skipped because
 * they have no data extents.
 *
 * When a mismatch is detected, the user is offered to update
 * file_size to match the extent sum.
 *
 * @param ctx          Filesystem context.
 * @param auto_yes     If nonzero, always repair.
 * @param auto_no      If nonzero, never repair.
 * @param bad_count    Output: number of mismatches found.
 * @param fixed_count  Output: number of mismatches repaired.
 */
static void check_file_size_vs_extents(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, uint64_t *bad_count,
                                       uint64_t *fixed_count)
{
    *bad_count   = 0;
    *fixed_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return;

    size_t   bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return;

    /* --- Phase 1: collect per-inode overflow logical_count sums --- */

    /* Hash map: simple open-addressing table mapping inode_id → sum */
    typedef struct
    {
        uint64_t inode_id;
        uint64_t logical_sum;
    } ovf_entry_t;

    uint64_t     ovf_cap   = 0;
    uint64_t     ovf_count = 0;
    ovf_entry_t *ovf_map   = NULL;

    if(ctx->overflow_hdr.root_node_lba != 0)
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack) { free(buf); return; }

        stack[stk_size++] = ctx->overflow_hdr.root_node_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];

            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header nhdr;
            memcpy(&nhdr, buf, sizeof(nhdr));
            if(nhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(nhdr.level > 0)
            {
                for(uint16_t i = 0; i < nhdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp) { free(buf); free(stack); free(ovf_map); return; }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: accumulate overflow logical_count per inode_id */
            for(uint16_t i = 0; i < nhdr.node_keys; i++)
            {
                struct overflow_extent oe;
                memcpy(&oe, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(oe), sizeof(oe));

                /* Linear scan (sufficient for fsck; inodes with overflow are rare) */
                int found = 0;
                for(uint64_t j = 0; j < ovf_count; j++)
                {
                    if(ovf_map[j].inode_id == oe.inode_id)
                    {
                        ovf_map[j].logical_sum += oe.logical_count;
                        found = 1;
                        break;
                    }
                }
                if(!found)
                {
                    if(ovf_count >= ovf_cap)
                    {
                        ovf_cap = ovf_cap ? ovf_cap * 2 : 64;
                        ovf_entry_t *tmp = realloc(ovf_map, ovf_cap * sizeof(*tmp));
                        if(!tmp) { free(buf); free(stack); free(ovf_map); return; }
                        ovf_map = tmp;
                    }
                    ovf_map[ovf_count].inode_id    = oe.inode_id;
                    ovf_map[ovf_count].logical_sum = oe.logical_count;
                    ovf_count++;
                }
            }
        }

        free(stack);
    }

    /* --- Phase 2: walk inodes, compare file_size vs extent sum --- */
    {
        uint64_t *stack    = malloc(64 * sizeof(uint64_t));
        uint64_t  stk_size = 0, stk_cap = 64;
        if(!stack) { free(buf); free(ovf_map); return; }

        stack[stk_size++] = root_lba;

        while(stk_size > 0)
        {
            uint64_t lba = stack[--stk_size];

            if(obmafs3_block_read(ctx, lba, buf, bsz) != OBMAFS3_OK) break;

            struct btree_node_header hdr;
            memcpy(&hdr, buf, sizeof(hdr));
            if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) break;

            if(hdr.level > 0)
            {
                for(uint16_t i = 0; i < hdr.node_keys; i++)
                {
                    struct btree_index_entry ie;
                    memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                    if(stk_size >= stk_cap)
                    {
                        stk_cap *= 2;
                        uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                        if(!tmp) { free(buf); free(stack); free(ovf_map); return; }
                        stack = tmp;
                    }
                    stack[stk_size++] = ie.child_lba;
                }
                continue;
            }

            /* Leaf: check each inode */
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct inode_record rec;
                memcpy(&rec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(rec), sizeof(rec));

                /* Skip directories — they have no data extents */
                if(rec.file_type == kFileTypeDirectory) continue;

                /* Skip media/CD images — file_size reflects logical disk size,
                 * not extent capacity, because data is stored via dedup trees */
                if(rec.file_type == kFileTypeMediaImage || rec.file_type == kFileTypeCompactDiscImage) continue;

                /* Sum inline extents */
                uint64_t logical_sum = 0;
                for(int e = 0; e < 8; e++)
                    logical_sum += rec.extents[e].logical_blocks;

                /* Add overflow extents for this inode */
                for(uint64_t j = 0; j < ovf_count; j++)
                {
                    if(ovf_map[j].inode_id == rec.inode_id)
                    {
                        logical_sum += ovf_map[j].logical_sum;
                        break;
                    }
                }

                uint64_t expected_size = logical_sum * bsz;

                /*
                 * The last extent's logical coverage may exceed the actual
                 * file_size (partial last block).  So file_size must be:
                 *   (total_logical - last_extent_logical) * bsz < file_size <= total_logical * bsz
                 *
                 * Simplified: file_size must not exceed extent capacity,
                 * and extent capacity minus one extent worth must not exceed file_size.
                 * But for files with zero extents, file_size must be 0.
                 */
                if(logical_sum == 0)
                {
                    if(rec.file_size != 0)
                    {
                        printf("    inode %" PRIu64 ": file_size=%" PRIu64
                               " but no extents (expected 0)\n",
                               rec.inode_id, rec.file_size);
                        (*bad_count)++;
                        if(ask_fix(auto_yes, auto_no, "    Set file_size to 0?"))
                        {
                            rec.file_size = 0;
                            obmafs3_inode_put(ctx, &rec);
                            (*fixed_count)++;
                        }
                    }
                    continue;
                }

                if(rec.file_size > expected_size)
                {
                    printf("    inode %" PRIu64 ": file_size=%" PRIu64
                           " exceeds extent capacity %" PRIu64 " (%" PRIu64 " logical blocks)\n",
                           rec.inode_id, rec.file_size, expected_size, logical_sum);
                    (*bad_count)++;
                    if(ask_fix(auto_yes, auto_no, "    Clamp file_size to extent capacity?"))
                    {
                        rec.file_size = expected_size;
                        obmafs3_inode_put(ctx, &rec);
                        (*fixed_count)++;
                    }
                }
                else if(rec.file_size == 0 && logical_sum > 0)
                {
                    printf("    inode %" PRIu64 ": file_size=0 but has %" PRIu64
                           " logical blocks (capacity %" PRIu64 ")\n",
                           rec.inode_id, logical_sum, expected_size);
                    (*bad_count)++;
                    if(ask_fix(auto_yes, auto_no, "    Set file_size to extent capacity?"))
                    {
                        rec.file_size = expected_size;
                        obmafs3_inode_put(ctx, &rec);
                        (*fixed_count)++;
                    }
                }
            }
        }

        free(stack);
    }

    free(buf);
    free(ovf_map);
}

/**
 * Top-level extent validation: inline + overflow.
 *
 * @param ctx       Filesystem context.
 * @param auto_yes  If nonzero, always repair.
 * @param auto_no   If nonzero, never repair.
 * @param errors    In/out: incremented for each unfixed error.
 */
static void check_extent_validity(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors)
{
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;

    printf("\n  %sExtent validation%s\n", CLR_BOLD, CLR_RESET);

    /* Inline extents */
    uint64_t inline_bad = 0, inline_fixed = 0;
    validate_inline_extents(ctx, total_blocks, auto_yes, auto_no, &inline_bad, &inline_fixed);

    if(inline_bad == 0)
    {
        result_ok("Inline extents:", "");
    }
    else
    {
        if(inline_fixed > 0)
            result_fixed("Inline extents:", "%" PRIu64 " bad, %" PRIu64 " cleared", inline_bad, inline_fixed);
        else
            result_bad("Inline extents:", "%" PRIu64 " bad", inline_bad);
        *errors += (int)(inline_bad - inline_fixed);
    }

    /* Overflow extents */
    uint64_t overflow_bad = 0;
    validate_overflow_extents(ctx, total_blocks, &overflow_bad);

    if(overflow_bad == 0)
    {
        result_ok("Overflow extents:", "");
    }
    else
    {
        result_bad("Overflow extents:", "%" PRIu64 " bad", overflow_bad);
        *errors += (int)overflow_bad;
    }

    /* ---- File size vs extent sum ---- */
    uint64_t sz_bad = 0, sz_fixed = 0;
    check_file_size_vs_extents(ctx, auto_yes, auto_no, &sz_bad, &sz_fixed);

    if(sz_bad == 0)
    {
        result_ok("File size check:", "");
    }
    else
    {
        if(sz_fixed > 0)
            result_fixed("File size check:", "%" PRIu64 " mismatch, %" PRIu64 " fixed", sz_bad, sz_fixed);
        else
            result_bad("File size check:", "%" PRIu64 " mismatch", sz_bad);
        *errors += (int)(sz_bad - sz_fixed);
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

/**
 * Entry point for the OBMAFS3 filesystem checker.
 *
 * Opens the filesystem in lenient mode, validates the superblock,
 * verifies tree header and node checksums for all B+Trees (catalog,
 * inode, overflow, dedup, media tag, CD prefix/suffix/subchannel,
 * metadata, metadata index), checks allocation bitmap consistency,
 * and optionally scrubs data block checksums and reports dedup
 * statistics.
 */
int main(int argc, char *argv[])
{
    int auto_yes          = 0;
    int auto_no           = 0;
    int do_scrub          = 0;
    int do_dedup_stats    = 0;
    int dedup_stats_only  = 0;
    int do_verify_hashes  = 0;

    static struct option long_opts[] = {
        {          "help", no_argument, NULL, 'h'},
        {         "scrub", no_argument, NULL, 's'},
        {   "dedup-stats", no_argument, NULL, 'd'},
        {"dedup-stats-only", no_argument, NULL, 'D'},
        {"verify-hashes", no_argument, NULL, 'v'},
        {           NULL,           0, NULL,   0}
    };

    int opt;
    while((opt = getopt_long(argc, argv, "ynsdDvh", long_opts, NULL)) != -1)
    {
        switch(opt)
        {
            case 'y':
                auto_yes = 1;
                break;
            case 'n':
                auto_no = 1;
                break;
            case 's':
                do_scrub = 1;
                break;
            case 'd':
                do_dedup_stats = 1;
                break;
            case 'D':
                dedup_stats_only = 1;
                do_dedup_stats   = 1;
                break;
            case 'v':
                do_verify_hashes = 1;
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if(auto_yes && auto_no)
    {
        fprintf(stderr, "Error: -y and -n are mutually exclusive\n");
        return 1;
    }

    if(optind >= argc)
    {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[optind];

    /* Initialise colour support and timing */
    init_color();
    timer_now(&g_start_time);
    g_phase_num = 0;

    printf("%sobmafsck%s — OBMAFS v3 filesystem checker\n", CLR_BOLD, CLR_RESET);
    printf("%sChecking %s%s\n", CLR_DIM, path, CLR_RESET);

    /* ---- Open the filesystem with raw I/O and detailed error reporting ---- */
    int fd = open(path, O_RDWR);
    if(fd < 0)
    {
        /* Fall back to read-only if read-write fails (e.g. read-only media) */
        fd = open(path, O_RDONLY);
        if(fd < 0)
        {
            fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
            return 1;
        }
    }

    struct stat file_stat;
    if(fstat(fd, &file_stat) < 0)
    {
        fprintf(stderr, "Error: cannot stat '%s': %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    if(!S_ISREG(file_stat.st_mode) && !S_ISBLK(file_stat.st_mode))
    {
        fprintf(stderr, "Error: '%s' is not a regular file or block device\n", path);
        close(fd);
        return 1;
    }

    /* Read the superblock directly */
    struct obmafs3_sb sb;
    ssize_t           nread = pread(fd, &sb, sizeof(sb), 0);
    if(nread < 0)
    {
        fprintf(stderr, "Error: cannot read superblock from '%s': %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if((size_t)nread < sizeof(sb))
    {
        fprintf(stderr, "Error: '%s' is too small to contain a superblock (read %zd of %zu bytes)\n", path, nread,
                sizeof(sb));
        close(fd);
        return 1;
    }

    if(sb.magic != OBMAFS3_SB_MAGIC)
    {
        fprintf(stderr,
                "Warning: primary superblock has bad magic (expected 0x%016" PRIx64 ", found 0x%016" PRIx64 ")\n",
                (uint64_t)OBMAFS3_SB_MAGIC, sb.magic);

        /* Try to recover from the backup superblock at the last block. */
        off_t                 fsize    = (S_ISREG(file_stat.st_mode)) ? file_stat.st_size : lseek(fd, 0, SEEK_END);
        int                   recovered = 0;
        static const uint64_t try_bs[]  = {4096, 512, 1024, 2048, 8192, 16384, 32768, 65536};
        if(fsize > 0)
        {
            for(int i = 0; i < (int)(sizeof(try_bs) / sizeof(try_bs[0])); i++)
            {
                uint64_t          bs = try_bs[i];
                if((uint64_t)fsize < 2 * bs) continue;
                struct obmafs3_sb backup;
                if(obmafs3_sb_read_backup(fd, bs, (uint64_t)fsize, &backup) == OBMAFS3_OK &&
                   obmafs3_sb_validate(&backup) == OBMAFS3_OK && backup.block_size == bs &&
                   backup.total_bytes == (uint64_t)fsize)
                {
                    fprintf(stderr, "  Recovered superblock from backup (block_size=%" PRIu64 ")\n", bs);
                    sb        = backup;
                    recovered = 1;

                    /* Restore primary from the backup */
                    if(pwrite(fd, &sb, sizeof(sb), 0) == sizeof(sb))
                        fprintf(stderr, "  Primary superblock restored from backup.\n");
                    else
                        fprintf(stderr, "  Warning: could not restore primary superblock.\n");
                    break;
                }
            }
        }
        if(!recovered)
        {
            fprintf(stderr,
                    "Error: '%s' does not contain an OBMAFS3 filesystem\n"
                    "  (primary magic bad and no valid backup found)\n",
                    path);
            close(fd);
            return 1;
        }
    }

    if(sb.block_size == 0)
    {
        fprintf(stderr, "Error: superblock has invalid block_size = 0\n");
        close(fd);
        return 1;
    }

    if(sb.total_bytes == 0)
    {
        fprintf(stderr, "Error: superblock has invalid total_bytes = 0\n");
        close(fd);
        return 1;
    }

    if(sb.dedup_block_size == 0)
    {
        fprintf(stderr, "Error: superblock has invalid dedup_block_size = 0\n");
        close(fd);
        return 1;
    }

    if(sb.catalog_lba == 0)
    {
        fprintf(stderr, "Error: superblock has no catalog tree (catalog_lba = 0)\n");
        close(fd);
        return 1;
    }

    if(sb.inode_lba == 0)
    {
        fprintf(stderr, "Error: superblock has no inode tree (inode_lba = 0)\n");
        close(fd);
        return 1;
    }

    /* Build a minimal ctx for the library helpers (block_read, btree_header_read, etc.) */
    struct obmafs3_ctx *ctx = calloc(1, sizeof(*ctx));
    if(!ctx)
    {
        fprintf(stderr, "Error: out of memory allocating filesystem context\n");
        close(fd);
        return 1;
    }
    ctx->fd          = fd;
    ctx->sb          = sb;
    ctx->compression = 1;
    ctx->zstd_level  = 15;

    /* Initialise thread-local buffer key (obmafsck is single-threaded,
     * but the library now uses TLS for its scratch buffers). */
    if(pthread_key_create(&ctx->tls_key, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_key_create failed\n");
        close(fd);
        free(ctx);
        return 1;
    }
    pthread_mutex_init(&ctx->write_lock, NULL);

    /* Force lazy TLS allocation so the library has buffers to work with */
    struct obmafs3_thread_bufs *tb = obmafs3_get_thread_bufs(ctx);

    ctx->rc_leaf_buf   = malloc((size_t)sb.block_size);
    ctx->rc_leaf_valid = 0;

    if(!tb || !tb->hdr_buf || !tb->node_buf || !tb->io_buf || !tb->io_buf2 ||
       !tb->comp_buf || !tb->zstd_cctx || !tb->zstd_dctx || !ctx->rc_leaf_buf)
    {
        fprintf(stderr, "Error: out of memory allocating work buffers\n");
        obmafs3_close(ctx);
        return 1;
    }

    /* Read B+Tree headers leniently (tolerate checksum errors so we can report them) */
    int cs_tmp;
    int rc;

    rc = obmafs3_btree_header_read_lenient(ctx, sb.catalog_lba, &ctx->catalog_hdr, &cs_tmp);
    if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
        fprintf(stderr, "Warning: cannot read catalog tree header at LBA %" PRIu64 " (error %d)\n", sb.catalog_lba,
                rc);

    rc = obmafs3_btree_header_read_lenient(ctx, sb.inode_lba, &ctx->inode_hdr, &cs_tmp);
    if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
        fprintf(stderr, "Warning: cannot read inode tree header at LBA %" PRIu64 " (error %d)\n", sb.inode_lba, rc);

    if(sb.overflow_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.overflow_lba, &ctx->overflow_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read overflow tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.overflow_lba, rc);
    }

    if(sb.media_tag_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.media_tag_lba, &ctx->media_tag_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read media tag tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.media_tag_lba, rc);
    }

    if(sb.cd_prefix_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.cd_prefix_lba, &ctx->cd_prefix_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read CD prefix tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.cd_prefix_lba, rc);
    }

    if(sb.cd_suffix_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.cd_suffix_lba, &ctx->cd_suffix_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read CD suffix tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.cd_suffix_lba, rc);
    }

    if(sb.cd_subchannel_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.cd_subchannel_lba, &ctx->cd_subchannel_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read CD subchannel tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.cd_subchannel_lba, rc);
    }

    if(sb.metadata_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.metadata_lba, &ctx->metadata_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read metadata tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.metadata_lba, rc);
    }

    if(sb.metadata_idx_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.metadata_idx_lba, &ctx->metadata_idx_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read metadata index tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.metadata_idx_lba, rc);
    }

    if(sb.refcount_lba != 0)
    {
        rc = obmafs3_btree_header_read_lenient(ctx, sb.refcount_lba, &ctx->refcount_hdr, &cs_tmp);
        if(rc != OBMAFS3_OK && rc != OBMAFS3_ERR_CHECKSUM)
            fprintf(stderr, "Warning: cannot read refcount tree header at LBA %" PRIu64 " (error %d)\n",
                    sb.refcount_lba, rc);
    }

    int errors = 0;

    /* ---- Dedup-stats-only fast path: skip all integrity checks ---- */
    if(dedup_stats_only)
    {
        phase_begin("Dedup statistics");
        rc = compute_dedup_stats(ctx);
        if(rc != OBMAFS3_OK) fprintf(stderr, "Warning: could not compute dedup stats: %d\n", rc);
        phase_end();

        /* Summary */
        {
            char            total_dur[32];
            struct timespec now;
            timer_now(&now);
            fmt_duration(timer_elapsed(&g_start_time, &now), total_dur, sizeof(total_dur));
            printf("\n%s── Summary%s\n", CLR_BOLD, CLR_RESET);
            printf("  %sCompleted in %s%s\n", CLR_DIM, total_dur, CLR_RESET);
        }

        obmafs3_close(ctx);
        return 0;
    }

    /* ---- Superblock ---- */
    phase_begin("Superblock");
    if(ctx->sb.magic == OBMAFS3_SB_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->sb.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->sb.magic);

    /* Verify superblock checksum */
    {
        uint8_t stored[32], computed[32];
        memcpy(stored, ctx->sb.checksum, 32);
        memset(ctx->sb.checksum, 0, 32);
        obmafs3_checksum_block(&ctx->sb, sizeof(ctx->sb), computed);
        memcpy(ctx->sb.checksum, stored, 32);
        int sb_cs_ok = (memcmp(stored, computed, 32) == 0);
        if(sb_cs_ok)
            result_ok("Checksum:", "");
        else
        {
            result_bad("Checksum:", "mismatch");
            errors++;
            if(ask_fix(auto_yes, auto_no, "Recompute superblock checksum?"))
            {
                memset(ctx->sb.checksum, 0, 32);
                obmafs3_checksum_block(&ctx->sb, sizeof(ctx->sb), ctx->sb.checksum);
                ssize_t nn = pwrite(fd, &ctx->sb, sizeof(ctx->sb), 0);
                if(nn < 0 || (size_t)nn != sizeof(ctx->sb))
                    fprintf(stderr, "  %sError: could not write superblock checksum fix%s\n", CLR_RED, CLR_RESET);
                else
                {
                    result_fixed("Checksum:", "recomputed");
                    errors--;
                    /* Also update the backup superblock */
                    if(ctx->sb.total_bytes > 0 && ctx->sb.block_size > 0)
                    {
                        uint64_t blba = OBMAFS3_BACKUP_SB_LBA(ctx->sb.total_bytes, ctx->sb.block_size);
                        if(blba > 0)
                            pwrite(fd, &ctx->sb, sizeof(ctx->sb), (off_t)(blba * ctx->sb.block_size));
                    }
                }
            }
        }
    }

    result_info("Block size:", "%" PRIu64, ctx->sb.block_size);
    result_info("Dedup block size:", "%" PRIu64, ctx->sb.dedup_block_size);
    result_info("Total bytes:", "%" PRIu64, ctx->sb.total_bytes);
    result_info("Volume label:", "%s", ctx->sb.volume_label);

    if(ctx->sb.magic != OBMAFS3_SB_MAGIC) errors++;

    validate_superblock_fields(&ctx->sb, fd, (uint64_t)file_stat.st_size, auto_yes, auto_no, &errors);

    /* ---- Backup superblock ---- */
    if(ctx->sb.total_bytes > 0 && ctx->sb.block_size > 0)
    {
        uint64_t          backup_lba = OBMAFS3_BACKUP_SB_LBA(ctx->sb.total_bytes, ctx->sb.block_size);
        struct obmafs3_sb backup_sb;
        int               backup_cs_ok = 0;
        int backup_rc = obmafs3_sb_read_backup_lenient(fd, ctx->sb.block_size, ctx->sb.total_bytes, &backup_sb,
                                                       &backup_cs_ok);

        printf("  %sBackup (LBA %" PRIu64 "):%s\n", CLR_DIM, backup_lba, CLR_RESET);

        if(backup_rc != OBMAFS3_OK)
        {
            result_bad("Read:", "failed (rc=%d)", backup_rc);
            errors++;
            if(ask_fix(auto_yes, auto_no, "Write backup superblock from primary?"))
            {
                /* Recompute checksum on the primary and write as backup */
                struct obmafs3_sb tmp = ctx->sb;
                memset(tmp.checksum, 0, sizeof(tmp.checksum));
                obmafs3_checksum_block(&tmp, sizeof(tmp), tmp.checksum);
                off_t   boff = (off_t)(backup_lba * ctx->sb.block_size);
                ssize_t nn   = pwrite(fd, &tmp, sizeof(tmp), boff);
                if(nn >= 0 && (size_t)nn == sizeof(tmp))
                {
                    result_fixed("Backup:", "written from primary");
                    errors--;
                }
                else
                {
                    fprintf(stderr, "  %sError: could not write backup superblock%s\n", CLR_RED, CLR_RESET);
                }
            }
        }
        else
        {
            if(backup_sb.magic == OBMAFS3_SB_MAGIC)
                result_ok("Magic:", "0x%016" PRIx64, backup_sb.magic);
            else
                result_bad("Magic:", "0x%016" PRIx64, backup_sb.magic);
            if(backup_cs_ok)
                result_ok("Checksum:", "");
            else
                result_bad("Checksum:", "mismatch");

            if(backup_sb.magic != OBMAFS3_SB_MAGIC || !backup_cs_ok)
            {
                errors++;
                if(ask_fix(auto_yes, auto_no, "Overwrite backup superblock from primary?"))
                {
                    struct obmafs3_sb tmp = ctx->sb;
                    memset(tmp.checksum, 0, sizeof(tmp.checksum));
                    obmafs3_checksum_block(&tmp, sizeof(tmp), tmp.checksum);
                    off_t   boff = (off_t)(backup_lba * ctx->sb.block_size);
                    ssize_t nn   = pwrite(fd, &tmp, sizeof(tmp), boff);
                    if(nn >= 0 && (size_t)nn == sizeof(tmp))
                    {
                        result_fixed("Backup:", "overwritten from primary");
                        errors--;
                    }
                    else
                    {
                        fprintf(stderr, "  %sError: could not write backup superblock%s\n", CLR_RED, CLR_RESET);
                    }
                }
            }
            else
            {
                /* Both readable — compare contents (excluding checksum which may differ) */
                struct obmafs3_sb primary_cmp = ctx->sb;
                struct obmafs3_sb backup_cmp  = backup_sb;
                memset(primary_cmp.checksum, 0, sizeof(primary_cmp.checksum));
                memset(backup_cmp.checksum, 0, sizeof(backup_cmp.checksum));

                if(memcmp(&primary_cmp, &backup_cmp, sizeof(struct obmafs3_sb)) == 0)
                {
                    result_ok("Consistency:", "");
                }
                else
                {
                    result_bad("Consistency:", "backup differs from primary");
                    errors++;
                    if(ask_fix(auto_yes, auto_no, "Overwrite backup superblock from primary?"))
                    {
                        struct obmafs3_sb tmp = ctx->sb;
                        memset(tmp.checksum, 0, sizeof(tmp.checksum));
                        obmafs3_checksum_block(&tmp, sizeof(tmp), tmp.checksum);
                        off_t   boff = (off_t)(backup_lba * ctx->sb.block_size);
                        ssize_t nn   = pwrite(fd, &tmp, sizeof(tmp), boff);
                        if(nn >= 0 && (size_t)nn == sizeof(tmp))
                        {
                            result_fixed("Backup:", "synced from primary");
                            errors--;
                        }
                        else
                        {
                            fprintf(stderr, "  %sError: could not write backup superblock%s\n", CLR_RED, CLR_RESET);
                        }
                    }
                }
            }
        }
    }
    phase_end();

    /* ---- Catalog tree ---- */
    phase_begin("B+Tree structures");  /* catalog is first */
    printf("\n  %sCatalog tree%s\n", CLR_BOLD, CLR_RESET);
    if(ctx->catalog_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->catalog_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->catalog_hdr.magic);
    {
        int cat_hdr_cs_ok = 0;
        obmafs3_btree_header_read_lenient(ctx, ctx->sb.catalog_lba, &ctx->catalog_hdr, &cat_hdr_cs_ok);
        if(cat_hdr_cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
        if(!cat_hdr_cs_ok) errors++;
    }
    result_info("Root node LBA:", "%" PRIu64, ctx->catalog_hdr.root_node_lba);

    if(ctx->catalog_hdr.root_node_lba != 0)
    {
        uint64_t *cat_nodes      = NULL;
        uint64_t  cat_node_count = 0;
        int       wrc = walk_catalog_btree_nodes(ctx, ctx->catalog_hdr.root_node_lba, &cat_nodes, &cat_node_count);
        if(wrc == OBMAFS3_OK)
        {
            uint64_t cat_bad = 0, cat_cs_fix = 0;
            verify_btree_node_checksums(ctx, cat_nodes, cat_node_count, "Catalog", auto_yes, auto_no,
                                        &cat_bad, &cat_cs_fix);
            uint64_t cat_ord = 0, cat_fix = 0;
            verify_btree_ordering(ctx, cat_nodes, cat_node_count, ORD_CATALOG,
                                  sizeof(struct catalog_record), sizeof(struct catalog_index_entry),
                                  "Catalog", auto_yes, auto_no, &cat_ord, &cat_fix);
            free(cat_nodes);
            if(cat_bad > 0)
            {
                if(cat_cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", cat_bad, cat_cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", cat_bad);
                errors += (int)(cat_bad - cat_cs_fix);
            }
            else
            {
                result_ok("Node checksums:", "");
            }
            if(cat_ord > 0)
            {
                if(cat_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", cat_ord, cat_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", cat_ord);
                errors += (int)(cat_ord - cat_fix);
            }
            else
            {
                result_ok("Key ordering:", "");
            }
            verify_fix_total_nodes(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, cat_node_count, "Catalog", "  ",
                                   auto_yes, auto_no, &errors);
            verify_fix_free_nodes(ctx, &ctx->catalog_hdr, ctx->sb.catalog_lba, "Catalog", "  ",
                                  auto_yes, auto_no, &errors);
            {
                uint64_t sib_bad = 0, sib_fix = 0;
                verify_fix_sibling_links(ctx, ctx->catalog_hdr.root_node_lba,
                                         sizeof(struct catalog_index_entry),
                                         __builtin_offsetof(struct catalog_index_entry, child_lba), 1,
                                         "Catalog", auto_yes, auto_no, &sib_bad, &sib_fix);
                if(sib_bad > 0)
                {
                    if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                    errors += (int)(sib_bad - sib_fix);
                }
                else
                {
                    result_ok("Sibling links:", "");
                }
            }
        }
        else
        {
            result_bad("Node checksums:", "walk failed");
            errors++;
        }
    }

    /* ---- Inode tree ---- */
    printf("\n  %sInode tree%s\n", CLR_BOLD, CLR_RESET);
    if(ctx->inode_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->inode_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->inode_hdr.magic);
    {
        int ino_hdr_cs_ok = 0;
        obmafs3_btree_header_read_lenient(ctx, ctx->sb.inode_lba, &ctx->inode_hdr, &ino_hdr_cs_ok);
        if(ino_hdr_cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
        if(!ino_hdr_cs_ok) errors++;
    }
    result_info("Root node LBA:", "%" PRIu64, ctx->inode_hdr.root_node_lba);

    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint64_t *ino_nodes      = NULL;
        uint64_t  ino_node_count = 0;
        int       wrc = walk_inode_btree_nodes(ctx, ctx->inode_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ino_nodes, &ino_node_count, ctx->inode_hdr.total_nodes, "Inode");
        if(wrc == OBMAFS3_OK)
        {
            uint64_t ino_bad = 0, ino_cs_fix = 0;
            verify_btree_node_checksums(ctx, ino_nodes, ino_node_count, "Inode", auto_yes, auto_no,
                                        &ino_bad, &ino_cs_fix);
            uint64_t ino_ord = 0, ino_fix = 0;
            verify_btree_ordering(ctx, ino_nodes, ino_node_count, ORD_UINT64_KEY,
                                  sizeof(struct inode_record), sizeof(struct btree_index_entry),
                                  "Inode", auto_yes, auto_no, &ino_ord, &ino_fix);
            free(ino_nodes);
            if(ino_bad > 0)
            {
                if(ino_cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", ino_bad, ino_cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", ino_bad);
                errors += (int)(ino_bad - ino_cs_fix);
            }
            else
            {
                result_ok("Node checksums:", "");
            }
            if(ino_ord > 0)
            {
                if(ino_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ino_ord, ino_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", ino_ord);
                errors += (int)(ino_ord - ino_fix);
            }
            else
            {
                result_ok("Key ordering:", "");
            }
            verify_fix_total_nodes(ctx, &ctx->inode_hdr, ctx->sb.inode_lba, ino_node_count, "Inode", "  ",
                                   auto_yes, auto_no, &errors);
            verify_fix_free_nodes(ctx, &ctx->inode_hdr, ctx->sb.inode_lba, "Inode", "  ",
                                  auto_yes, auto_no, &errors);
            {
                uint64_t sib_bad = 0, sib_fix = 0;
                verify_fix_sibling_links(ctx, ctx->inode_hdr.root_node_lba,
                                         sizeof(struct btree_index_entry),
                                         __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                         "Inode", auto_yes, auto_no, &sib_bad, &sib_fix);
                if(sib_bad > 0)
                {
                    if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                    errors += (int)(sib_bad - sib_fix);
                }
                else
                {
                    result_ok("Sibling links:", "");
                }
            }
        }
        else
        {
            result_bad("Node checksums:", "could not walk tree");
            errors++;
        }
    }

    /* ---- Overflow tree ---- */
    if(ctx->sb.overflow_lba != 0)
    {
        printf("\n  %sOverflow tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->overflow_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->overflow_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->overflow_hdr.magic);
        {
            int ovf_hdr_cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.overflow_lba, &ctx->overflow_hdr, &ovf_hdr_cs_ok);
            if(ovf_hdr_cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!ovf_hdr_cs_ok) errors++;
        }

        if(ctx->overflow_hdr.root_node_lba != 0)
        {
            uint64_t *ovf_nodes      = NULL;
            uint64_t  ovf_node_count = 0;
            int       wrc = walk_inode_btree_nodes(ctx, ctx->overflow_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &ovf_nodes, &ovf_node_count, ctx->overflow_hdr.total_nodes, "Overflow");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t ovf_bad = 0, ovf_cs_fix = 0;
                verify_btree_node_checksums(ctx, ovf_nodes, ovf_node_count, "Overflow", auto_yes, auto_no,
                                            &ovf_bad, &ovf_cs_fix);
                uint64_t ovf_ord = 0, ovf_fix = 0;
                verify_btree_ordering(ctx, ovf_nodes, ovf_node_count, ORD_OVERFLOW,
                                      sizeof(struct overflow_extent), sizeof(struct btree_index_entry),
                                      "Overflow", auto_yes, auto_no, &ovf_ord, &ovf_fix);
                free(ovf_nodes);
                if(ovf_bad > 0)
                {
                    if(ovf_cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", ovf_bad, ovf_cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", ovf_bad);
                    errors += (int)(ovf_bad - ovf_cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ovf_ord > 0)
                {
                    if(ovf_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ovf_ord, ovf_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", ovf_ord);
                    errors += (int)(ovf_ord - ovf_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->overflow_hdr, ctx->sb.overflow_lba, ovf_node_count, "Overflow",
                                       "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->overflow_hdr, ctx->sb.overflow_lba, "Overflow", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->overflow_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "Overflow", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Dedup tree list ---- */
    if(ctx->sb.dedup_lba != 0)
    {
        printf("\n  %sDedup tree list%s\n", CLR_BOLD, CLR_RESET);

        uint8_t *list_buf = calloc(1, (size_t)ctx->sb.block_size);
        if(list_buf)
        {
            rc = obmafs3_block_read(ctx, ctx->sb.dedup_lba, list_buf, (size_t)ctx->sb.block_size);
            if(rc == OBMAFS3_OK)
            {
                struct tree_list_header list_hdr;
                memcpy(&list_hdr, list_buf, sizeof(list_hdr));
                if(list_hdr.magic == OBMAFS3_TREELIST_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, list_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, list_hdr.magic);
                if(list_hdr.magic != OBMAFS3_TREELIST_MAGIC) errors++;

                /* Verify list header checksum */
                if(list_hdr.magic == OBMAFS3_TREELIST_MAGIC)
                {
                    uint8_t stored_cs[32];
                    memcpy(stored_cs, list_hdr.checksum, 32);
                    memset(list_buf + __builtin_offsetof(struct tree_list_header, checksum), 0, 32);
                    uint8_t computed_cs[32];
                    size_t  cs_len = sizeof(struct tree_list_header) +
                                    (size_t)(list_hdr.tree_count * sizeof(struct tree_list_entry));
                    obmafs3_checksum_block(list_buf, cs_len, computed_cs);
                    int cs_ok = (memcmp(stored_cs, computed_cs, 32) == 0);
                    if(cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
                    if(!cs_ok) errors++;

                    printf("  Trees:            %" PRIu64 "\n", list_hdr.tree_count);

                    /* Verify each per-sector-size tree */
                    struct tree_list_entry *tl_entries = NULL;
                    if(list_hdr.tree_count > 0)
                    {
                        tl_entries = malloc((size_t)(list_hdr.tree_count * sizeof(struct tree_list_entry)));
                        if(tl_entries)
                            memcpy(tl_entries, list_buf + sizeof(struct tree_list_header),
                                   (size_t)(list_hdr.tree_count * sizeof(struct tree_list_entry)));
                    }

                    for(uint64_t t = 0; t < list_hdr.tree_count && tl_entries; t++)
                    {
                        printf("  Tree %" PRIu64 " (sector_size=%" PRIu16 "):\n", t, tl_entries[t].sector_size);

                        struct btree_header thdr;
                        rc = obmafs3_btree_header_read(ctx, tl_entries[t].tree_lba, &thdr);
                        if(rc == OBMAFS3_OK)
                        {
                            printf("    Magic:          0x%016" PRIx64 " (%s)\n", thdr.magic,
                                   thdr.magic == OBMAFS3_BTREE_HDR_MAGIC ? "OK" : "BAD");
                            if(thdr.magic != OBMAFS3_BTREE_HDR_MAGIC) errors++;

                            int thdr_cs_ok = 0;
                            obmafs3_btree_header_read_lenient(ctx, tl_entries[t].tree_lba, &thdr, &thdr_cs_ok);
                            printf("    Header checksum:%s\n", thdr_cs_ok ? " OK" : " BAD");
                            if(!thdr_cs_ok) errors++;

                            if(thdr.root_node_lba != 0)
                            {
                                uint64_t *dd_nodes = NULL;
                                uint64_t  dd_count = 0;
                                int       wrc = walk_inode_btree_nodes(ctx, thdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &dd_nodes, &dd_count, thdr.total_nodes, "Dedup");
                                if(wrc == OBMAFS3_OK)
                                {
                                    uint64_t dbad = 0, dcs_fix = 0;
                                    verify_btree_node_checksums(ctx, dd_nodes, dd_count, "Dedup", auto_yes,
                                                                auto_no, &dbad, &dcs_fix);
                                    uint64_t dord = 0, dfix = 0;
                                    verify_btree_ordering(ctx, dd_nodes, dd_count, ORD_UINT64_KEY,
                                                          sizeof(struct dedup_entry), sizeof(struct btree_index_entry),
                                                          "Dedup", auto_yes, auto_no, &dord, &dfix);
                                    free(dd_nodes);
                                    if(dbad > 0)
                                    {
                                        printf("    Node checksums: "
                                               "%" PRIu64 " BAD",
                                               dbad);
                                        if(dcs_fix > 0) printf(" (%" PRIu64 " fixed)", dcs_fix);
                                        printf("\n");
                                        errors += (int)(dbad - dcs_fix);
                                    }
                                    else
                                    {
                                        printf("    Node checksums:"
                                               " OK\n");
                                    }
                                    if(dord > 0)
                                    {
                                        printf("    Key ordering:   "
                                               "%" PRIu64 " BAD",
                                               dord);
                                        if(dfix > 0) printf(" (%" PRIu64 " fixed)", dfix);
                                        printf("\n");
                                        errors += (int)(dord - dfix);
                                    }
                                    else
                                    {
                                        printf("    Key ordering:  "
                                               " OK\n");
                                    }
                                    verify_fix_total_nodes(ctx, &thdr, tl_entries[t].tree_lba, dd_count, "Dedup",
                                                           "    ", auto_yes, auto_no, &errors);
                                    verify_fix_free_nodes(ctx, &thdr, tl_entries[t].tree_lba, "Dedup", "    ",
                                                          auto_yes, auto_no, &errors);
                                    {
                                        uint64_t sib_bad = 0, sib_fix = 0;
                                        verify_fix_sibling_links(ctx, thdr.root_node_lba,
                                                                 sizeof(struct btree_index_entry),
                                                                 __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                                                 "Dedup", auto_yes, auto_no, &sib_bad, &sib_fix);
                                        if(sib_bad > 0)
                                        {
                                            printf("    Sibling links:  %" PRIu64 " BAD", sib_bad);
                                            if(sib_fix > 0) printf(" (%" PRIu64 " fixed)", sib_fix);
                                            printf("\n");
                                            errors += (int)(sib_bad - sib_fix);
                                        }
                                        else
                                        {
                                            printf("    Sibling links:  OK\n");
                                        }
                                    }
                                }
                                else
                                {
                                    printf("    Node checksums:"
                                           " could not walk tree\n");
                                    errors++;
                                }
                            }
                        }
                        else
                        {
                            printf("    Error reading header: %d\n", rc);
                            errors++;
                        }
                    }

                    free(tl_entries);
                }
            }
            else
            {
                printf("  Error reading tree list block: %d\n", rc);
                errors++;
            }
            free(list_buf);
        }
    }

    /* ---- Media tag tree ---- */
    if(ctx->sb.media_tag_lba != 0)
    {
        printf("\n  %sMedia tag tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->media_tag_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->media_tag_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->media_tag_hdr.magic);
        {
            int mt_hdr_cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.media_tag_lba, &ctx->media_tag_hdr, &mt_hdr_cs_ok);
            if(mt_hdr_cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!mt_hdr_cs_ok) errors++;
        }

        if(ctx->media_tag_hdr.root_node_lba != 0)
        {
            uint64_t *mt_nodes      = NULL;
            uint64_t  mt_node_count = 0;
            int       wrc = walk_inode_btree_nodes(ctx, ctx->media_tag_hdr.root_node_lba, sizeof(struct media_tag_index_entry), __builtin_offsetof(struct media_tag_index_entry, child_lba), &mt_nodes, &mt_node_count, ctx->media_tag_hdr.total_nodes, "Media tag");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t mt_bad = 0, mt_cs_fix = 0;
                verify_btree_node_checksums(ctx, mt_nodes, mt_node_count, "Media tag", auto_yes, auto_no,
                                            &mt_bad, &mt_cs_fix);
                uint64_t mt_ord = 0, mt_fix = 0;
                verify_btree_ordering(ctx, mt_nodes, mt_node_count, ORD_MEDIA_TAG,
                                      sizeof(struct media_tag_record), sizeof(struct media_tag_index_entry),
                                      "Media tag", auto_yes, auto_no, &mt_ord, &mt_fix);
                free(mt_nodes);
                if(mt_bad > 0)
                {
                    if(mt_cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", mt_bad, mt_cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", mt_bad);
                    errors += (int)(mt_bad - mt_cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(mt_ord > 0)
                {
                    if(mt_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", mt_ord, mt_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", mt_ord);
                    errors += (int)(mt_ord - mt_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, mt_node_count, "Media tag",
                                       "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->media_tag_hdr, ctx->sb.media_tag_lba, "Media tag", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->media_tag_hdr.root_node_lba,
                                             sizeof(struct media_tag_index_entry),
                                             __builtin_offsetof(struct media_tag_index_entry, child_lba), 1,
                                             "Media tag", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- CD prefix tree ---- */
    if(ctx->sb.cd_prefix_lba != 0)
    {
        printf("\n  %sCD prefix tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->cd_prefix_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->cd_prefix_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->cd_prefix_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_prefix_lba, &ctx->cd_prefix_hdr, &cs_ok);
            if(cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_prefix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_inode_btree_nodes(ctx, ctx->cd_prefix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->cd_prefix_hdr.total_nodes, "CD prefix");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD prefix", auto_yes, auto_no,
                                            &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct cd_prefix_record), sizeof(struct btree_index_entry),
                                      "CD prefix", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_prefix_hdr, ctx->sb.cd_prefix_lba, node_count, "CD prefix",
                                       "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->cd_prefix_hdr, ctx->sb.cd_prefix_lba, "CD prefix", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_prefix_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "CD prefix", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- CD suffix tree ---- */
    if(ctx->sb.cd_suffix_lba != 0)
    {
        printf("\n  %sCD suffix tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->cd_suffix_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->cd_suffix_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->cd_suffix_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_suffix_lba, &ctx->cd_suffix_hdr, &cs_ok);
            if(cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_suffix_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_inode_btree_nodes(ctx, ctx->cd_suffix_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->cd_suffix_hdr.total_nodes, "CD suffix");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD suffix", auto_yes, auto_no,
                                            &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct cd_suffix_record), sizeof(struct btree_index_entry),
                                      "CD suffix", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_suffix_hdr, ctx->sb.cd_suffix_lba, node_count, "CD suffix",
                                       "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->cd_suffix_hdr, ctx->sb.cd_suffix_lba, "CD suffix", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_suffix_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "CD suffix", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- CD subchannel tree ---- */
    if(ctx->sb.cd_subchannel_lba != 0)
    {
        printf("\n  %sCD subchannel tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->cd_subchannel_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->cd_subchannel_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->cd_subchannel_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.cd_subchannel_lba, &ctx->cd_subchannel_hdr, &cs_ok);
            if(cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->cd_subchannel_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc = walk_inode_btree_nodes(ctx, ctx->cd_subchannel_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->cd_subchannel_hdr.total_nodes, "CD subchannel");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "CD subchannel", auto_yes, auto_no,
                                            &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct cd_subchannel_record), sizeof(struct btree_index_entry),
                                      "CD subchannel", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->cd_subchannel_hdr, ctx->sb.cd_subchannel_lba, node_count,
                                       "CD subchannel", "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->cd_subchannel_hdr, ctx->sb.cd_subchannel_lba, "CD subchannel",
                                      "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->cd_subchannel_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "CD subchannel", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Metadata tree (per-image key=value) ---- */
    if(ctx->sb.metadata_lba != 0)
    {
        printf("\n  %sMetadata tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->metadata_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->metadata_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->metadata_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.metadata_lba, &ctx->metadata_hdr, &cs_ok);
            if(cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->metadata_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc =
                walk_meta_btree_nodes(ctx, ctx->metadata_hdr.root_node_lba, sizeof(struct metadata_index_entry),
                                      __builtin_offsetof(struct metadata_index_entry, child_lba), &nodes, &node_count);
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_meta_node_checksums(ctx, nodes, node_count, "Metadata", auto_yes, auto_no,
                                           &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_meta_ordering(ctx, nodes, node_count, ORD_METADATA,
                                     sizeof(struct metadata_record), sizeof(struct metadata_index_entry),
                                     "Metadata", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors++;
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, node_count, "Metadata",
                                       "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->metadata_hdr, ctx->sb.metadata_lba, "Metadata", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->metadata_hdr.root_node_lba,
                                             sizeof(struct metadata_index_entry),
                                             __builtin_offsetof(struct metadata_index_entry, child_lba),
                                             METADATA_NODE_BLOCKS,
                                             "Metadata", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Metadata index tree (reverse key+value→inode) ---- */
    if(ctx->sb.metadata_idx_lba != 0)
    {
        printf("\n  %sMetadata index tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->metadata_idx_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->metadata_idx_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->metadata_idx_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.metadata_idx_lba, &ctx->metadata_idx_hdr, &cs_ok);
            if(cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->metadata_idx_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_meta_btree_nodes(
                ctx, ctx->metadata_idx_hdr.root_node_lba, sizeof(struct metadata_idx_index_entry),
                __builtin_offsetof(struct metadata_idx_index_entry, child_lba), &nodes, &node_count);
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_meta_node_checksums(ctx, nodes, node_count, "Metadata index", auto_yes, auto_no,
                                           &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_meta_ordering(ctx, nodes, node_count, ORD_METADATA_IDX,
                                     sizeof(struct metadata_idx_record), sizeof(struct metadata_idx_index_entry),
                                     "Metadata index", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, node_count,
                                       "Metadata index", "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->metadata_idx_hdr, ctx->sb.metadata_idx_lba, "Metadata index",
                                      "  ", auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->metadata_idx_hdr.root_node_lba,
                                             sizeof(struct metadata_idx_index_entry),
                                             __builtin_offsetof(struct metadata_idx_index_entry, child_lba),
                                             METADATA_NODE_BLOCKS,
                                             "Metadata index", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    /* ---- Metadata bidirectional consistency ---- */
    if(ctx->sb.metadata_lba != 0 && ctx->sb.metadata_idx_lba != 0 &&
       ctx->metadata_hdr.root_node_lba != 0 && ctx->metadata_idx_hdr.root_node_lba != 0)
    {
        check_metadata_bidirectional(ctx, auto_yes, auto_no, &errors);
    }

    /* ---- Refcount tree ---- */
    if(ctx->sb.refcount_lba != 0)
    {
        printf("\n  %sRefcount tree%s\n", CLR_BOLD, CLR_RESET);
        if(ctx->refcount_hdr.magic == OBMAFS3_BTREE_HDR_MAGIC)
        result_ok("Magic:", "0x%016" PRIx64, ctx->refcount_hdr.magic);
    else
        result_bad("Magic:", "0x%016" PRIx64, ctx->refcount_hdr.magic);
        {
            int cs_ok = 0;
            obmafs3_btree_header_read_lenient(ctx, ctx->sb.refcount_lba, &ctx->refcount_hdr, &cs_ok);
            if(cs_ok)
            result_ok("Header checksum:", "");
        else
            result_bad("Header checksum:", "mismatch");
            if(!cs_ok) errors++;
        }

        if(ctx->refcount_hdr.root_node_lba != 0)
        {
            uint64_t *nodes      = NULL;
            uint64_t  node_count = 0;
            int       wrc        = walk_inode_btree_nodes(ctx, ctx->refcount_hdr.root_node_lba, sizeof(struct btree_index_entry), __builtin_offsetof(struct btree_index_entry, child_lba), &nodes, &node_count, ctx->refcount_hdr.total_nodes, "Refcount");
            if(wrc == OBMAFS3_OK)
            {
                uint64_t bad = 0, cs_fix = 0;
                verify_btree_node_checksums(ctx, nodes, node_count, "Refcount", auto_yes, auto_no,
                                            &bad, &cs_fix);
                uint64_t ord = 0, ord_fix = 0;
                verify_btree_ordering(ctx, nodes, node_count, ORD_UINT64_KEY,
                                      sizeof(struct refcount_record), sizeof(struct btree_index_entry),
                                      "Refcount", auto_yes, auto_no, &ord, &ord_fix);
                free(nodes);
                if(bad > 0)
                {
                    if(cs_fix > 0)
                result_fixed("Node checksums:", "%" PRIu64 " bad, %" PRIu64 " fixed", bad, cs_fix);
            else
                result_bad("Node checksums:", "%" PRIu64 " bad", bad);
                    errors += (int)(bad - cs_fix);
                }
                else
                {
                    result_ok("Node checksums:", "");
                }
                if(ord > 0)
                {
                    if(ord_fix > 0)
                result_fixed("Key ordering:", "%" PRIu64 " bad, %" PRIu64 " fixed", ord, ord_fix);
            else
                result_bad("Key ordering:", "%" PRIu64 " bad", ord);
                    errors += (int)(ord - ord_fix);
                }
                else
                {
                    result_ok("Key ordering:", "");
                }
                verify_fix_total_nodes(ctx, &ctx->refcount_hdr, ctx->sb.refcount_lba, node_count, "Refcount",
                                       "  ", auto_yes, auto_no, &errors);
                verify_fix_free_nodes(ctx, &ctx->refcount_hdr, ctx->sb.refcount_lba, "Refcount", "  ",
                                      auto_yes, auto_no, &errors);
                {
                    uint64_t sib_bad = 0, sib_fix = 0;
                    verify_fix_sibling_links(ctx, ctx->refcount_hdr.root_node_lba,
                                             sizeof(struct btree_index_entry),
                                             __builtin_offsetof(struct btree_index_entry, child_lba), 1,
                                             "Refcount", auto_yes, auto_no, &sib_bad, &sib_fix);
                    if(sib_bad > 0)
                    {
                        if(sib_fix > 0)
                    result_fixed("Sibling links:", "%" PRIu64 " bad, %" PRIu64 " fixed", sib_bad, sib_fix);
                else
                    result_bad("Sibling links:", "%" PRIu64 " bad", sib_bad);
                        errors += (int)(sib_bad - sib_fix);
                    }
                    else
                    {
                        result_ok("Sibling links:", "");
                    }
                }
            }
            else
            {
                result_bad("Node checksums:", "could not walk tree");
                errors++;
            }
        }
    }

    phase_end();

    /* ---- Phase 3: Cross-references & consistency ---- */
    phase_begin("Cross-references & consistency");

    /* ---- Inode / Catalog cross-reference ---- */
    if(ctx->catalog_hdr.root_node_lba != 0 && ctx->inode_hdr.root_node_lba != 0)
        cross_check_inodes_catalog(ctx, auto_yes, auto_no, &errors);

    /* ---- next_inode_id validation ---- */
    if(ctx->inode_hdr.root_node_lba != 0)
    {
        uint64_t *all_ino_ids = NULL;
        uint64_t  all_ino_cnt = 0;
        int       ino_rc      = collect_inode_ids(ctx, &all_ino_ids, &all_ino_cnt);

        printf("\n  %snext_inode_id validation%s\n", CLR_BOLD, CLR_RESET);

        if(ino_rc != OBMAFS3_OK)
        {
            result_bad("Inode tree:", "could not walk (%d)", ino_rc);
            errors++;
        }
        else if(all_ino_cnt == 0)
        {
            result_info("Inodes:", "none found");
        }
        else
        {
            /* Find the maximum inode ID in use */
            uint64_t max_id = 0;
            for(uint64_t i = 0; i < all_ino_cnt; i++)
            {
                if(all_ino_ids[i] > max_id) max_id = all_ino_ids[i];
            }

            result_info("Highest inode ID:", "%" PRIu64, max_id);
            result_info("next_inode_id:", "%" PRIu64, ctx->sb.next_inode_id);

            if(ctx->sb.next_inode_id <= max_id)
            {
                printf("  ERROR: next_inode_id %" PRIu64 " <= highest inode %" PRIu64
                       " (would cause ID collisions)\n",
                       ctx->sb.next_inode_id, max_id);
                errors++;

                uint64_t correct = max_id + 1;
                char prompt[128];
                snprintf(prompt, sizeof(prompt),
                         "  Set next_inode_id to %" PRIu64 "?", correct);

                if(ask_fix(auto_yes, auto_no, prompt))
                {
                    ctx->sb.next_inode_id = correct;

                    /* Recompute superblock checksum and write */
                    memset(ctx->sb.checksum, 0, sizeof(ctx->sb.checksum));
                    obmafs3_checksum_block(&ctx->sb, sizeof(ctx->sb), ctx->sb.checksum);
                    ssize_t nn = pwrite(fd, &ctx->sb, sizeof(ctx->sb), 0);
                    if(nn < 0 || (size_t)nn != sizeof(ctx->sb))
                        fprintf(stderr, "  Error: could not write superblock fix\n");
                    else
                    {
                        printf("  next_inode_id fixed to %" PRIu64 ".\n", correct);
                        errors--;
                        /* Also update the backup superblock */
                        if(ctx->sb.total_bytes > 0 && ctx->sb.block_size > 0)
                        {
                            uint64_t blba = OBMAFS3_BACKUP_SB_LBA(ctx->sb.total_bytes, ctx->sb.block_size);
                            if(blba > 0)
                                pwrite(fd, &ctx->sb, sizeof(ctx->sb),
                                       (off_t)(blba * ctx->sb.block_size));
                        }
                    }
                }
            }
            else
            {
                result_ok("Status:", "");
            }
        }

        free(all_ino_ids);
    }

    /* ---- Extent validation ---- */
    if(ctx->inode_hdr.root_node_lba != 0)
        check_extent_validity(ctx, auto_yes, auto_no, &errors);

    /* ---- Refcount data validation ---- */
    if(ctx->inode_hdr.root_node_lba != 0)
    {
        printf("\n  %sRefcount validation%s\n", CLR_BOLD, CLR_RESET);
        uint64_t rc_bad = 0, rc_fix = 0;
        verify_refcount_tree(ctx, auto_yes, auto_no, &rc_bad, &rc_fix);
        if(rc_bad == 0)
        {
            result_ok("Refcounts:", "");
        }
        else
        {
            if(rc_fix > 0)
                result_fixed("Refcounts:", "%" PRIu64 " mismatch, %" PRIu64 " fixed", rc_bad, rc_fix);
            else
                result_bad("Refcounts:", "%" PRIu64 " mismatch", rc_bad);
            errors += (int)(rc_bad - rc_fix);
        }
    }

    phase_end();

    /* ---- Phase 4: Allocation bitmap ---- */
    phase_begin("Allocation bitmap");
    uint64_t total_blocks = ctx->sb.total_bytes / ctx->sb.block_size;

    if(ctx->sb.bitmap_lba != 0 && ctx->sb.bitmap_blocks != 0)
    {
        uint64_t bitmap_bytes = (total_blocks + 7) / 8;
        size_t   hdr_size     = sizeof(struct bitmap_header);

        /* Read bitmap header from first bitmap block */
        uint8_t             *bhdr_buf = malloc((size_t)ctx->sb.block_size);
        struct bitmap_header bhdr;
        memset(&bhdr, 0, sizeof(bhdr));
        int bhdr_ok = 0;
        if(bhdr_buf)
        {
            if(obmafs3_block_read(ctx, ctx->sb.bitmap_lba, bhdr_buf, (size_t)ctx->sb.block_size) == OBMAFS3_OK)
            {
                memcpy(&bhdr, bhdr_buf, hdr_size);
                bhdr_ok = 1;
            }
            free(bhdr_buf);
        }

        /* Read the raw bitmap data from disk (manually, since we skipped it) */
        uint8_t *disk_bitmap    = calloc(1, (size_t)bitmap_bytes);
        int      bitmap_read_ok = 0;
        if(disk_bitmap)
        {
            uint8_t *blk = malloc((size_t)ctx->sb.block_size);
            if(blk)
            {
                uint64_t remaining = bitmap_bytes;
                uint64_t offset    = 0;
                bitmap_read_ok     = 1;
                for(uint64_t i = 0; i < ctx->sb.bitmap_blocks; i++)
                {
                    if(obmafs3_block_read(ctx, ctx->sb.bitmap_lba + i, blk, (size_t)ctx->sb.block_size) != OBMAFS3_OK)
                    {
                        bitmap_read_ok = 0;
                        break;
                    }
                    if(i == 0)
                    {
                        size_t avail = (size_t)ctx->sb.block_size - hdr_size;
                        size_t copy  = remaining < avail ? (size_t)remaining : avail;
                        memcpy(disk_bitmap, blk + hdr_size, copy);
                        offset += copy;
                        remaining -= copy;
                    }
                    else
                    {
                        size_t copy = remaining < ctx->sb.block_size ? (size_t)remaining : (size_t)ctx->sb.block_size;
                        memcpy(disk_bitmap + offset, blk, copy);
                        offset += copy;
                        remaining -= copy;
                    }
                }
                free(blk);
            }
        }

        /* Verify bitmap checksum */
        int checksum_ok = 0;
        if(bitmap_read_ok && bhdr_ok)
        {
            uint8_t computed[32];
            obmafs3_checksum_block(disk_bitmap, (size_t)bitmap_bytes, computed);
            checksum_ok = (memcmp(bhdr.checksum, computed, 32) == 0);
        }

        uint64_t allocated = 0;
        if(bitmap_read_ok)
        {
            for(uint64_t b = 0; b < total_blocks; b++)
            {
                if((disk_bitmap[b / 8] >> (b % 8)) & 1) allocated++;
            }
        }

        printf("\n  %sAllocation bitmap%s\n", CLR_BOLD, CLR_RESET);
        if(bhdr.magic == OBMAFS3_BITMAP_MAGIC)
            result_ok("Magic:", "0x%016" PRIx64, bhdr.magic);
        else
            result_bad("Magic:", "0x%016" PRIx64, bhdr.magic);
        if(!bhdr_ok || !bitmap_read_ok)
            result_bad("Checksum:", "unreadable");
        else if(checksum_ok)
            result_ok("Checksum:", "");
        else
            result_bad("Checksum:", "mismatch");
        if(!checksum_ok && bitmap_read_ok) errors++;
        result_info("Bitmap LBA:", "%" PRIu64, ctx->sb.bitmap_lba);
        result_info("Bitmap blocks:", "%" PRIu64, ctx->sb.bitmap_blocks);
        result_info("Total blocks:", "%" PRIu64, total_blocks);
        result_info("Allocated:", "%" PRIu64, allocated);
        result_info("Free:", "%" PRIu64, total_blocks - allocated);

        /* ---- Build expected bitmap and compare ---- */
        if(bitmap_read_ok)
        {
            /* Set ctx->bitmap temporarily so build_expected_bitmap helpers work */
            ctx->bitmap      = disk_bitmap;
            ctx->bitmap_size = bitmap_bytes;

            int      build_err = 0;
            uint8_t *expected  = build_expected_bitmap(ctx, total_blocks, bitmap_bytes, &build_err);
            if(expected && !build_err)
            {
                /* Compare on-disk bitmap with expected */
                uint64_t missing       = 0;
                uint64_t extra         = 0;
                uint64_t first_missing = 0;
                uint64_t first_extra   = 0;

                for(uint64_t b = 0; b < total_blocks; b++)
                {
                    int on_disk     = (disk_bitmap[b / 8] >> (b % 8)) & 1;
                    int in_expected = (expected[b / 8] >> (b % 8)) & 1;

                    if(in_expected && !on_disk)
                    {
                        if(missing == 0) first_missing = b;
                        missing++;
                    }
                    if(!in_expected && on_disk)
                    {
                        if(extra == 0) first_extra = b;
                        extra++;
                    }
                }

                if(missing == 0 && extra == 0) { result_ok("Consistency:", ""); }
                else
                {
                    result_bad("Consistency:", "MISMATCH");
                    errors++;

                    if(missing > 0)
                        printf("    %" PRIu64 " block(s) used but not marked allocated"
                               " (first: LBA %" PRIu64 ")\n",
                               missing, first_missing);
                    if(extra > 0)
                        printf("    %" PRIu64 " block(s) marked allocated but not used"
                               " (first: LBA %" PRIu64 ")\n",
                               extra, first_extra);

                    uint64_t expected_alloc = 0;
                    for(uint64_t b = 0; b < total_blocks; b++)
                    {
                        if((expected[b / 8] >> (b % 8)) & 1) expected_alloc++;
                    }
                    printf("    Expected allocated: %" PRIu64 ", on-disk allocated: %" PRIu64 "\n", expected_alloc,
                           allocated);

                    if(ask_fix(auto_yes, auto_no, "Fix allocation bitmap?"))
                    {
                        memcpy(ctx->bitmap, expected, (size_t)bitmap_bytes);
                        rc = obmafs3_bitmap_write(ctx);
                        if(rc == OBMAFS3_OK)
                        {
                            printf("  Bitmap repaired.\n");
                            errors--;                              /* checksum error */
                            if(missing > 0 || extra > 0) errors--; /* consistency error */
                        }
                        else
                        {
                            fprintf(stderr, "  Error: failed to write bitmap: %d\n", rc);
                        }
                    }
                }
                free(expected);
            }
            else
            {
                fprintf(stderr, "Warning: could not build expected bitmap\n");
            }

            /* Clear temporary bitmap pointer (obmafs3_close will free) */
        }
        else
        {
            fprintf(stderr, "Warning: could not read bitmap data\n");
        }

        if(!bitmap_read_ok && disk_bitmap)
        {
            free(disk_bitmap);
            ctx->bitmap = NULL;
        }
    }

    phase_end();

    /* ---- Data block scrub ---- */
    if(do_scrub)
    {
        phase_begin("Data block scrub");
        uint64_t scrub_bad = scrub_data_blocks(ctx);
        if(scrub_bad > 0) errors += (int)scrub_bad;

        uint64_t dedup_bad = scrub_dedup_data_blocks(ctx);
        if(dedup_bad > 0) errors += (int)dedup_bad;
        phase_end();
    }

    /* ---- Hash verification (dedup + CD) ---- */
    if(do_verify_hashes)
    {
        phase_begin("Hash verification");
        uint64_t dedup_hash_bad = verify_dedup_hashes(ctx);
        if(dedup_hash_bad > 0) errors += (int)dedup_hash_bad;

        if(ctx->sb.cd_prefix_lba != 0)
        {
            uint64_t cd_bad = verify_cd_tree_hashes(ctx, &ctx->cd_prefix_hdr, "CD prefix",
                                                    sizeof(struct cd_prefix_record), CD_PREFIX_DATA_SIZE);
            if(cd_bad > 0) errors += (int)cd_bad;
        }

        if(ctx->sb.cd_suffix_lba != 0)
        {
            uint64_t cd_bad = verify_cd_tree_hashes(ctx, &ctx->cd_suffix_hdr, "CD suffix",
                                                    sizeof(struct cd_suffix_record), CD_SUFFIX_DATA_SIZE);
            if(cd_bad > 0) errors += (int)cd_bad;
        }

        if(ctx->sb.cd_subchannel_lba != 0)
        {
            uint64_t cd_bad = verify_cd_tree_hashes(ctx, &ctx->cd_subchannel_hdr, "CD subchannel",
                                                    sizeof(struct cd_subchannel_record), CD_SUBCHANNEL_DATA_SIZE);
            if(cd_bad > 0) errors += (int)cd_bad;
        }
        phase_end();
    }

    /* ---- Dedup statistics ---- */
    if(do_dedup_stats)
    {
        phase_begin("Dedup statistics");
        rc = compute_dedup_stats(ctx);
        if(rc != OBMAFS3_OK) fprintf(stderr, "Warning: could not compute dedup stats: %d\n", rc);
        phase_end();
    }

    /* ---- Summary ---- */
    {
        char total_dur[32];
        struct timespec now;
        timer_now(&now);
        fmt_duration(timer_elapsed(&g_start_time, &now), total_dur, sizeof(total_dur));
        printf("\n%s── Summary%s\n", CLR_BOLD, CLR_RESET);
        if(errors > 0)
        {
            printf("  %s %s%d error(s)%s found.\n", SYM_BAD, CLR_BOLD_RED, errors, CLR_RESET);
        }
        else
        {
            printf("  %s %sFilesystem is clean.%s\n", SYM_OK, CLR_BOLD_GRN, CLR_RESET);
        }
        printf("  %sCompleted in %s%s\n", CLR_DIM, total_dur, CLR_RESET);
    }

    obmafs3_close(ctx);
    return errors > 0 ? 1 : 0;
}
