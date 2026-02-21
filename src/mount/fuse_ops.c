/*
 * fuse_ops.c - OBMAFS3 FUSE filesystem operations (common)
 *
 * Contains globals, path resolution, disk image mapping helpers,
 * and the fuse_operations struct.  The actual callbacks are split
 * into separate files by category.
 */

#include "fuse_ops_internal.h"
#include "debug.h"

struct obmafs3_ctx *g_ctx = NULL;

/* Disk image extension-to-sector-size mappings */
struct disk_image_mapping g_disk_image_maps[OBMAFS3_MAX_DISK_IMAGE_MAPS];
int                       g_disk_image_map_count = 0;

/**
 * Parse the disk image extension-to-sector-size mapping string.
 *
 * Accepts a semicolon-separated list of "extension=sector_size" pairs
 * (e.g. "dsk=512;iso=2048") and populates @c g_disk_image_maps.
 *
 * @param spec  Mapping specification string (may be NULL or empty).
 * @return 0 on success, -1 on parse error.
 */
int parse_disk_image_maps(const char *spec)
{
    if(!spec || !*spec)
    {
        g_disk_image_map_count = 0;
        return 0;
    }

    /* Work on a copy so we can tokenise */
    char *buf = strdup(spec);
    if(!buf) return -1;

    int   count   = 0;
    char *saveptr = NULL;
    char *pair    = strtok_r(buf, ";", &saveptr);

    while(pair && count < OBMAFS3_MAX_DISK_IMAGE_MAPS)
    {
        char *eq = strchr(pair, '=');
        if(!eq || eq == pair || !*(eq + 1))
        {
            fprintf(stderr, "Error: bad disk_images pair: '%s'\n", pair);
            free(buf);
            return -1;
        }
        *eq              = '\0';
        const char *ext  = pair;
        const char *sval = eq + 1;

        char *endptr;
        long  val = strtol(sval, &endptr, 10);
        if(*endptr != '\0' || val <= 0 || val > 65535)
        {
            fprintf(stderr, "Error: invalid sector size '%s' for extension '%s'\n", sval, ext);
            free(buf);
            return -1;
        }

        strncpy(g_disk_image_maps[count].extension, ext, sizeof(g_disk_image_maps[count].extension) - 1);
        g_disk_image_maps[count].extension[sizeof(g_disk_image_maps[count].extension) - 1] = '\0';
        g_disk_image_maps[count].sector_size                                               = (uint16_t)val;
        count++;

        pair = strtok_r(NULL, ";", &saveptr);
    }

    g_disk_image_map_count = count;
    free(buf);
    return 0;
}

/**
 * Look up a filename against the disk image extension map.
 * Returns the sector size if the extension matches, or 0 if no match.
 */
uint16_t lookup_disk_image_sector_size(const char *name)
{
    const char *dot = strrchr(name, '.');
    if(!dot || dot == name) return 0;
    const char *ext = dot + 1;

    for(int i = 0; i < g_disk_image_map_count; i++)
    {
        if(strcasecmp(ext, g_disk_image_maps[i].extension) == 0) return g_disk_image_maps[i].sector_size;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Path resolution                                                    */
/* ------------------------------------------------------------------ */

/**
 * Resolve a FUSE path to (parent_inode_id, basename).
 * Currently supports the root and one level of nesting.
 */
int resolve_path(const char *path, uint64_t *parent_id, const char **name)
{
    if(strcmp(path, "/") == 0)
    {
        *parent_id = OBMAFS3_ROOT_INODE_ID;
        *name      = "/";
        return 0;
    }

    const char *last_slash = strrchr(path, '/');
    if(!last_slash) FUSE_RETURN(-ENOENT, "");

    *name = last_slash + 1;

    if(last_slash == path)
    {
        /* Path like "/filename" -> parent is root */
        *parent_id = OBMAFS3_ROOT_INODE_ID;
        return 0;
    }

    /*
     * For deeper paths, walk the catalog tree component by component.
     * Start at root and resolve each directory in the path.
     */
    size_t path_len    = (size_t)(last_slash - path);
    char  *parent_path = malloc(path_len + 1);
    if(!parent_path) FUSE_RETURN(-ENOMEM, "");
    memcpy(parent_path, path, path_len);
    parent_path[path_len] = '\0';

    /* Walk from root */
    uint64_t current_id = OBMAFS3_ROOT_INODE_ID;
    char    *saveptr    = NULL;
    char    *component  = strtok_r(parent_path + 1, "/", &saveptr); /* skip leading / */

    while(component)
    {
        struct catalog_record entry;
        int                   rc = obmafs3_catalog_lookup(g_ctx, current_id, component, &entry);
        if(rc == OBMAFS3_ERR_NOTFOUND)
        {
            free(parent_path);
            FUSE_RETURN(-ENOENT, "");
        }
        if(rc != OBMAFS3_OK)
        {
            free(parent_path);
            FUSE_RETURN(-EIO, "");
        }
        if(!entry.directory_flag)
        {
            free(parent_path);
            FUSE_RETURN(-ENOTDIR, "");
        }
        current_id = entry.inode_id;
        component  = strtok_r(NULL, "/", &saveptr);
    }

    free(parent_path);
    *parent_id = current_id;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  FUSE init – negotiate kernel capabilities                          */
/* ------------------------------------------------------------------ */

/**
 * FUSE init callback — negotiate larger write buffers and writeback cache.
 *
 * With larger max_write the kernel sends fewer, bigger WRITE requests,
 * and with FUSE_CAP_WRITEBACK_CACHE the kernel can batch dirty pages
 * before sending them.  Both are needed to give the parallel-compression
 * engine enough groups per call to saturate multiple CPU cores.
 */
static void *obmafs3_fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)cfg;

    /* Re-create the compression pool with live worker threads.
     * When FUSE daemonises (no -f flag) it forks, and the pool
     * workers from the parent do not survive into the child. */
    obmafs3_compress_pool_reinit(g_ctx);

    /* Start background warmup of the dedup key set.
     * This scans all dedup B+Tree leaves and populates an in-memory
     * hash set so that subsequent writes can skip tree traversal for
     * duplicate sectors.  Runs concurrently with early FUSE ops. */
    obmafs3_dedup_warmup_start(g_ctx);

    /* Request 1 MiB write buffers (default is 128 KiB). */
    conn->max_write     = 1048576;
    conn->max_readahead = 1048576;

    /*
     * NOTE: FUSE_CAP_WRITEBACK_CACHE is intentionally NOT enabled.
     * With writeback cache the kernel dispatches WRITE requests out of
     * order, which defeats the O(n) fast-append path in block.c.
     * Without it, writes arrive in sequential order and the fast path
     * is always taken.  The large max_write already gives us large
     * per-call parallelism (up to 16 compression groups per write).
     */

    /* Allow parallel directory operations. */
    if(conn->capable & FUSE_CAP_PARALLEL_DIROPS) conn->want |= FUSE_CAP_PARALLEL_DIROPS;

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  FUSE operations struct                                             */
/* ------------------------------------------------------------------ */

struct fuse_operations obmafs3_fuse_ops = {
    .init            = obmafs3_fuse_init,
    .getattr         = obmafs3_fuse_getattr,
    .readdir         = obmafs3_fuse_readdir,
    .open            = obmafs3_fuse_open,
    .read            = obmafs3_fuse_read,
    .create          = obmafs3_fuse_create,
    .write           = obmafs3_fuse_write,
    .flush           = obmafs3_fuse_flush,
    .release         = obmafs3_fuse_release,
    .truncate        = obmafs3_fuse_truncate,
    .unlink          = obmafs3_fuse_unlink,
    .rename          = obmafs3_fuse_rename,
    .link            = obmafs3_fuse_link,
    .symlink         = obmafs3_fuse_symlink,
    .readlink        = obmafs3_fuse_readlink,
    .mkdir           = obmafs3_fuse_mkdir,
    .rmdir           = obmafs3_fuse_rmdir,
    .utimens         = obmafs3_fuse_utimens,
    .chmod           = obmafs3_fuse_chmod,
    .chown           = obmafs3_fuse_chown,
    .statfs          = obmafs3_fuse_statfs,
    .statx           = obmafs3_fuse_statx,
    .getxattr        = obmafs3_fuse_getxattr,
    .setxattr        = obmafs3_fuse_setxattr,
    .listxattr       = obmafs3_fuse_listxattr,
    .removexattr     = obmafs3_fuse_removexattr,
    .ioctl           = obmafs3_fuse_ioctl,
    .copy_file_range = obmafs3_fuse_copy_file_range,
};
