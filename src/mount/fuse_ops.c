/*
 * fuse_ops.c - OBMAFS3 FUSE filesystem operations (common)
 *
 * Contains globals, path resolution, disk image mapping helpers,
 * and the fuse_operations struct.  The actual callbacks are split
 * into separate files by category.
 */

#include "fuse_ops_internal.h"

struct obmafs3_ctx *g_ctx = NULL;

/* Disk image extension-to-sector-size mappings */
struct disk_image_mapping g_disk_image_maps[OBMAFS3_MAX_DISK_IMAGE_MAPS];
int g_disk_image_map_count = 0;

int parse_disk_image_maps(const char *spec)
{
    if (!spec || !*spec) {
        g_disk_image_map_count = 0;
        return 0;
    }

    /* Work on a copy so we can tokenise */
    char *buf = strdup(spec);
    if (!buf)
        return -1;

    int count = 0;
    char *saveptr = NULL;
    char *pair = strtok_r(buf, ";", &saveptr);

    while (pair && count < OBMAFS3_MAX_DISK_IMAGE_MAPS) {
        char *eq = strchr(pair, '=');
        if (!eq || eq == pair || !*(eq + 1)) {
            fprintf(stderr, "Error: bad disk_images pair: '%s'\n", pair);
            free(buf);
            return -1;
        }
        *eq = '\0';
        const char *ext  = pair;
        const char *sval = eq + 1;

        char *endptr;
        long val = strtol(sval, &endptr, 10);
        if (*endptr != '\0' || val <= 0 || val > 65535) {
            fprintf(stderr,
                "Error: invalid sector size '%s' for extension '%s'\n",
                sval, ext);
            free(buf);
            return -1;
        }

        strncpy(g_disk_image_maps[count].extension, ext,
                sizeof(g_disk_image_maps[count].extension) - 1);
        g_disk_image_maps[count].extension[
            sizeof(g_disk_image_maps[count].extension) - 1] = '\0';
        g_disk_image_maps[count].sector_size = (uint16_t)val;
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
    if (!dot || dot == name)
        return 0;
    const char *ext = dot + 1;

    for (int i = 0; i < g_disk_image_map_count; i++) {
        if (strcasecmp(ext, g_disk_image_maps[i].extension) == 0)
            return g_disk_image_maps[i].sector_size;
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
int resolve_path(const char *path,
                        uint64_t *parent_id, const char **name)
{
    if (strcmp(path, "/") == 0) {
        *parent_id = OBMAFS3_ROOT_INODE_ID;
        *name = "/";
        return 0;
    }

    const char *last_slash = strrchr(path, '/');
    if (!last_slash)
        return -ENOENT;

    *name = last_slash + 1;

    if (last_slash == path) {
        /* Path like "/filename" -> parent is root */
        *parent_id = OBMAFS3_ROOT_INODE_ID;
        return 0;
    }

    /*
     * For deeper paths, walk the catalog tree component by component.
     * Start at root and resolve each directory in the path.
     */
    size_t path_len = (size_t)(last_slash - path);
    char *parent_path = malloc(path_len + 1);
    if (!parent_path)
        return -ENOMEM;
    memcpy(parent_path, path, path_len);
    parent_path[path_len] = '\0';

    /* Walk from root */
    uint64_t current_id = OBMAFS3_ROOT_INODE_ID;
    char *saveptr = NULL;
    char *component = strtok_r(parent_path + 1, "/", &saveptr); /* skip leading / */

    while (component) {
        struct catalog_record entry;
        int rc = obmafs3_catalog_lookup(g_ctx, current_id,
                                        component, &entry);
        if (rc == OBMAFS3_ERR_NOTFOUND) {
            free(parent_path);
            return -ENOENT;
        }
        if (rc != OBMAFS3_OK) {
            free(parent_path);
            return -EIO;
        }
        if (!entry.directory_flag) {
            free(parent_path);
            return -ENOTDIR;
        }
        current_id = entry.inode_id;
        component = strtok_r(NULL, "/", &saveptr);
    }

    free(parent_path);
    *parent_id = current_id;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  FUSE operations struct                                             */
/* ------------------------------------------------------------------ */

struct fuse_operations obmafs3_fuse_ops = {
    .getattr  = obmafs3_fuse_getattr,
    .readdir  = obmafs3_fuse_readdir,
    .open     = obmafs3_fuse_open,
    .read     = obmafs3_fuse_read,
    .create   = obmafs3_fuse_create,
    .write    = obmafs3_fuse_write,
    .flush    = obmafs3_fuse_flush,
    .release  = obmafs3_fuse_release,
    .truncate = obmafs3_fuse_truncate,
    .unlink   = obmafs3_fuse_unlink,
    .link     = obmafs3_fuse_link,
    .symlink  = obmafs3_fuse_symlink,
    .readlink = obmafs3_fuse_readlink,
    .mkdir    = obmafs3_fuse_mkdir,
    .rmdir    = obmafs3_fuse_rmdir,
    .utimens  = obmafs3_fuse_utimens,
    .chmod    = obmafs3_fuse_chmod,
    .chown    = obmafs3_fuse_chown,
    .statfs   = obmafs3_fuse_statfs,
    .statx    = obmafs3_fuse_statx,
    .getxattr    = obmafs3_fuse_getxattr,
    .setxattr    = obmafs3_fuse_setxattr,
    .listxattr   = obmafs3_fuse_listxattr,
    .removexattr = obmafs3_fuse_removexattr,
    .ioctl       = obmafs3_fuse_ioctl,
};
