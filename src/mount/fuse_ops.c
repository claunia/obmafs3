/*
 * fuse_ops.c - OBMAFS3 FUSE filesystem operations
 *
 * Implements read-write access for regular files:
 *   getattr, readdir, open, read, create, write, truncate, unlink, utimens
 */
#define FUSE_USE_VERSION 31

#include "fuse_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>

struct obmafs3_ctx *g_ctx = NULL;

/**
 * Per-file-handle context.  For media image files, this caches
 * sector_map_entries in RAM across multiple FUSE write calls and
 * flushes them to disk when the file is closed (release).
 */
struct fuse_file_ctx {
    uint64_t                inode_id;
    uint16_t                sector_size;   /* 0 for non-media-image files */
    struct btree_node_inode inode;         /* cached inode */
    int                     inode_dirty;   /* needs write-back on release */
    struct sector_map_cache sme_cache;
    struct dedup_block_cache db_cache;     /* persistent dedup block accumulator */
};

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
static uint16_t lookup_disk_image_sector_size(const char *name)
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
static int resolve_path(const char *path,
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
        struct btree_node_filename entry;
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
/*  FUSE callbacks                                                     */
/* ------------------------------------------------------------------ */

static int obmafs3_fuse_getattr(const char *path, struct stat *stbuf,
                                struct fuse_file_info *fi)
{
    struct btree_node_inode inode;
    struct btree_node_inode *ip;
    int rc;

    memset(stbuf, 0, sizeof(*stbuf));

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    if (strcmp(path, "/") == 0) {
        rc = obmafs3_inode_get(g_ctx, OBMAFS3_ROOT_INODE_ID, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;

        stbuf->st_ino   = inode.inode_id;
        stbuf->st_mode  = S_IFDIR | inode.mode;
        stbuf->st_nlink = 2;
        stbuf->st_uid   = inode.uid;
        stbuf->st_gid   = inode.gid;
        stbuf->st_size  = (off_t)inode.file_size;
        stbuf->st_atime = (time_t)inode.access_time;
        stbuf->st_mtime = (time_t)inode.modification_time;
        stbuf->st_ctime = (time_t)inode.creation_time;
        return 0;
    }

    uint64_t parent_id;
    const char *name;
    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    struct btree_node_filename cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (ffctx) {
        ip = &ffctx->inode;
    } else {
        rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
        ip = &inode;
    }

    stbuf->st_ino = ip->inode_id;
    if (cat_entry.directory_flag || ip->file_type == kFileTypeDirectory)
        stbuf->st_mode = S_IFDIR | ip->mode;
    else
        stbuf->st_mode = S_IFREG | ip->mode;
    stbuf->st_nlink = cat_entry.directory_flag ? 2 : 1;
    stbuf->st_uid   = ip->uid;
    stbuf->st_gid   = ip->gid;
    stbuf->st_size  = (off_t)ip->file_size;
    stbuf->st_atime = (time_t)ip->access_time;
    stbuf->st_mtime = (time_t)ip->modification_time;
    stbuf->st_ctime = (time_t)ip->creation_time;
    return 0;
}

static int obmafs3_fuse_readdir(const char *path, void *buf,
                                fuse_fill_dir_t filler, off_t offset,
                                struct fuse_file_info *fi,
                                enum fuse_readdir_flags flags)
{
    struct btree_node_filename *entries = NULL;
    uint32_t count = 0;
    uint64_t dir_inode_id;
    uint32_t i;
    int rc;

    (void)offset;
    (void)fi;
    (void)flags;

    if (strcmp(path, "/") == 0) {
        dir_inode_id = OBMAFS3_ROOT_INODE_ID;
    } else {
        uint64_t parent_id;
        const char *name;
        struct btree_node_filename cat_entry;

        rc = resolve_path(path, &parent_id, &name);
        if (rc != 0)
            return rc;

        rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
        if (rc != OBMAFS3_OK)
            return -ENOENT;

        if (!cat_entry.directory_flag)
            return -ENOTDIR;

        dir_inode_id = cat_entry.inode_id;
    }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    rc = obmafs3_catalog_list(g_ctx, dir_inode_id, &entries, &count);
    if (rc != OBMAFS3_OK)
        return -EIO;

    for (i = 0; i < count; i++) {
        /* Skip the root directory self-entry */
        if (entries[i].inode_id == OBMAFS3_ROOT_INODE_ID &&
            entries[i].parent_id == OBMAFS3_ROOT_INODE_ID)
            continue;
        filler(buf, entries[i].name, NULL, 0, 0);
    }

    obmafs3_catalog_list_free(entries);
    return 0;
}

static int obmafs3_fuse_open(const char *path, struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct btree_node_filename cat_entry;
    int rc;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (cat_entry.directory_flag)
        return -EISDIR;

    struct fuse_file_ctx *fctx = calloc(1, sizeof(*fctx));
    if (!fctx)
        return -ENOMEM;

    fctx->inode_id = cat_entry.inode_id;

    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &fctx->inode);
    if (rc != OBMAFS3_OK) {
        free(fctx);
        return -EIO;
    }

    if (fctx->inode.file_type == kFileTypeMediaImage)
        fctx->sector_size = lookup_disk_image_sector_size(name);

    fi->fh = (uint64_t)(uintptr_t)fctx;
    return 0;
}

static int obmafs3_fuse_read(const char *path, char *buf, size_t size,
                             off_t offset, struct fuse_file_info *fi)
{
    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;
    struct btree_node_inode inode;
    struct btree_node_inode *ip;
    int rc;

    if (ffctx) {
        ip = &ffctx->inode;
    } else {
        uint64_t parent_id;
        const char *name;
        struct btree_node_filename cat_entry;
        rc = resolve_path(path, &parent_id, &name);
        if (rc != 0)
            return rc;
        rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
        if (rc != OBMAFS3_OK)
            return -EIO;
        rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
        ip = &inode;
    }

    if ((uint64_t)offset >= ip->file_size)
        return 0;

    if ((uint64_t)offset + size > ip->file_size)
        size = (size_t)(ip->file_size - (uint64_t)offset);

    if (ip->file_type == kFileTypeMediaImage) {
        uint16_t ss = ffctx ? ffctx->sector_size : 0;
        if (ss == 0) {
            uint64_t parent_id;
            const char *name;
            rc = resolve_path(path, &parent_id, &name);
            if (rc != 0)
                return rc;
            ss = lookup_disk_image_sector_size(name);
        }
        if (ss == 0)
            return -EINVAL;
        rc = obmafs3_read_media_image_data(g_ctx, ip,
                                           (uint64_t)offset, buf,
                                           size, ss);
    } else {
        rc = obmafs3_read_file_data(g_ctx, ip, (uint64_t)offset,
                                    buf, size);
    }

    if (rc != OBMAFS3_OK)
        return -EIO;

    return (int)size;
}

/* ------------------------------------------------------------------ */
/*  Write operations                                                   */
/* ------------------------------------------------------------------ */

static int obmafs3_fuse_create(const char *path, mode_t mode,
                               struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct btree_node_filename cat_entry;
    int rc;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    /* Check if the file already exists */
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_OK)
        return -EEXIST;
    if (rc != OBMAFS3_ERR_NOTFOUND)
        return -EIO;

    /* Allocate a new inode ID */
    uint64_t new_inode_id = obmafs3_alloc_inode_id(g_ctx);

    /* Create the catalog entry */
    struct btree_node_filename new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
    new_cat.header.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    new_cat.header.record_type = kBtreeDataTypeFilename;
    new_cat.header.node_keys   = 1;
    new_cat.header.keys_length = (uint16_t)(sizeof(new_cat) -
                                            sizeof(new_cat.header));
    new_cat.inode_id       = new_inode_id;
    new_cat.parent_id      = parent_id;
    new_cat.directory_flag = 0;
    strncpy(new_cat.name, name, sizeof(new_cat.name) - 1);

    rc = obmafs3_catalog_insert(g_ctx, &new_cat);
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Create the inode */
    uint64_t now = (uint64_t)time(NULL);
    struct fuse_context *fctx = fuse_get_context();

    struct btree_node_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.header.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    new_inode.header.record_type = kBtreeDataTypeInode;
    new_inode.header.node_keys   = 1;
    new_inode.header.keys_length = (uint16_t)(sizeof(new_inode) -
                                              sizeof(new_inode.header));
    new_inode.inode_id          = new_inode_id;
    new_inode.uid               = fctx->uid;
    new_inode.gid               = fctx->gid;
    new_inode.mode              = mode & 07777;
    new_inode.creation_time     = now;
    new_inode.modification_time = now;
    new_inode.access_time       = now;
    new_inode.file_size         = 0;
    new_inode.file_type         = kFileTypeRegular;
    new_inode.sector_count      = 0;
    new_inode.sector_map_size   = 0;

    /* Check if this file should be treated as a media/disk image */
    uint16_t ss = lookup_disk_image_sector_size(name);
    if (ss > 0)
        new_inode.file_type = kFileTypeMediaImage;

    rc = obmafs3_inode_put(g_ctx, &new_inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    struct fuse_file_ctx *ffctx = calloc(1, sizeof(*ffctx));
    if (!ffctx)
        return -ENOMEM;
    ffctx->inode_id    = new_inode_id;
    ffctx->sector_size = ss;
    ffctx->inode       = new_inode;
    fi->fh = (uint64_t)(uintptr_t)ffctx;

    return 0;
}

static int obmafs3_fuse_write(const char *path, const char *buf,
                              size_t size, off_t offset,
                              struct fuse_file_info *fi)
{
    struct btree_node_inode inode;
    struct btree_node_inode *ip;
    int rc;

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    if (ffctx) {
        ip = &ffctx->inode;
    } else {
        uint64_t parent_id;
        const char *name;
        struct btree_node_filename cat_entry;
        rc = resolve_path(path, &parent_id, &name);
        if (rc != 0)
            return rc;
        rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
        if (rc != OBMAFS3_OK)
            return -EIO;
        rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
        ip = &inode;
    }

    if (ip->file_type == kFileTypeMediaImage) {
        uint16_t ss = ffctx ? ffctx->sector_size : 0;
        if (ss == 0) {
            uint64_t parent_id;
            const char *name;
            rc = resolve_path(path, &parent_id, &name);
            if (rc != 0)
                return rc;
            ss = lookup_disk_image_sector_size(name);
        }
        if (ss == 0)
            return -EINVAL;

        struct sector_map_cache *cache =
            (ffctx && ffctx->sector_size) ? &ffctx->sme_cache : NULL;
        struct dedup_block_cache *dbc =
            (ffctx && ffctx->sector_size) ? &ffctx->db_cache : NULL;

        rc = obmafs3_write_media_image_data(g_ctx, ip,
                                            (uint64_t)offset, buf,
                                            size, ss, cache, dbc);
    } else {
        rc = obmafs3_write_file_data(g_ctx, ip,
                                     (uint64_t)offset, buf, size);
    }

    if (rc == OBMAFS3_ERR_NOSPC)
        return -ENOSPC;
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Always persist the inode so that other processes (e.g. ls) see
     * the up-to-date file_size and extents.  We keep the in-memory
     * cached copy for our own reads to avoid a B+Tree lookup. */
    if (ffctx) {
        rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
        ffctx->inode_dirty = 0;
    } else {
        rc = obmafs3_inode_put(g_ctx, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
    }

    return (int)size;
}

static int obmafs3_fuse_truncate(const char *path, off_t newsize,
                                 struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct btree_node_filename cat_entry;
    struct btree_node_inode inode;
    struct btree_node_inode *ip;
    int rc;

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (ffctx) {
        ip = &ffctx->inode;
    } else {
        rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
        ip = &inode;
    }

    if ((uint64_t)newsize > ip->file_size) {
        /* Extending: write zeros to fill the gap */
        size_t gap = (size_t)((uint64_t)newsize - ip->file_size);
        void *zeros = calloc(1, gap);
        if (!zeros)
            return -ENOMEM;
        rc = obmafs3_write_file_data(g_ctx, ip,
                                     ip->file_size, zeros, gap);
        free(zeros);
        if (rc != OBMAFS3_OK)
            return -EIO;
    } else {
        /* Shrinking: free blocks that are no longer needed */
        uint64_t block_size = g_ctx->sb.block_size;
        size_t data_capacity = (size_t)block_size -
                               sizeof(struct block_header);
        uint64_t old_blocks =
            (ip->file_size + data_capacity - 1) / data_capacity;
        uint64_t new_blocks_needed = (newsize > 0)
            ? ((uint64_t)newsize + data_capacity - 1) / data_capacity
            : 0;

        if (new_blocks_needed < old_blocks) {
            /* Walk extents and free trailing blocks */
            uint64_t block_idx = 0;
            for (int ei = 0; ei < 8; ei++) {
                if (ip->extents[ei].block_count == 0)
                    continue;
                uint64_t ext_end = block_idx +
                                   ip->extents[ei].block_count;
                if (ext_end <= new_blocks_needed) {
                    block_idx = ext_end;
                    continue;
                }
                if (block_idx >= new_blocks_needed) {
                    /* Free entire extent */
                    obmafs3_free_blocks(
                        g_ctx,
                        ip->extents[ei].start_block,
                        ip->extents[ei].block_count);
                    ip->extents[ei].start_block = 0;
                    ip->extents[ei].block_count = 0;
                } else {
                    /* Partially free this extent */
                    uint64_t keep = new_blocks_needed - block_idx;
                    uint64_t free_count =
                        ip->extents[ei].block_count - keep;
                    obmafs3_free_blocks(
                        g_ctx,
                        ip->extents[ei].start_block + keep,
                        free_count);
                    ip->extents[ei].block_count = keep;
                }
                block_idx = ext_end;
            }
        }

        ip->file_size = (uint64_t)newsize;
    }

    ip->modification_time = (uint64_t)time(NULL);
    rc = obmafs3_inode_put(g_ctx, ip);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (ffctx)
        ffctx->inode_dirty = 0;

    return 0;
}

static int obmafs3_fuse_unlink(const char *path)
{
    uint64_t parent_id;
    const char *name;
    struct btree_node_filename cat_entry;
    int rc;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (cat_entry.directory_flag)
        return -EISDIR;

    /* Remove the catalog entry */
    rc = obmafs3_catalog_delete(g_ctx, parent_id, name);
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Remove the inode */
    rc = obmafs3_inode_delete(g_ctx, cat_entry.inode_id);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_utimens(const char *path,
                                const struct timespec ts[2],
                                struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct btree_node_filename cat_entry;
    struct btree_node_inode inode;
    int rc;

    (void)fi;

    if (strcmp(path, "/") == 0) {
        rc = obmafs3_inode_get(g_ctx, OBMAFS3_ROOT_INODE_ID, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
    } else {
        rc = resolve_path(path, &parent_id, &name);
        if (rc != 0)
            return rc;

        rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
        if (rc != OBMAFS3_OK)
            return -EIO;

        rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
    }

    inode.access_time       = (uint64_t)ts[0].tv_sec;
    inode.modification_time = (uint64_t)ts[1].tv_sec;

    rc = obmafs3_inode_put(g_ctx, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

/**
 * Flush is called on every close() of a file descriptor.  Unlike
 * release, flush is synchronous — close() blocks until flush returns.
 * We must persist all cached sector_map_entries and the inode here so
 * that a subsequent open() (possibly by another process) sees the
 * up-to-date on-disk state.
 */
static int obmafs3_fuse_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    if (!ffctx)
        return 0;

    int rc = 0;

    /* Flush the dedup block accumulator to disk */
    if (ffctx->sector_size && ffctx->db_cache.initialized) {
        int drc = obmafs3_flush_dedup_block_cache(g_ctx,
                      ffctx->sector_size, &ffctx->db_cache);
        if (rc == OBMAFS3_OK)
            rc = drc;
    }

    /* Flush cached sector_map_entries for media image files */
    if (ffctx->sector_size && ffctx->sme_cache.count > 0) {
        int src = obmafs3_flush_sector_map_cache(g_ctx, &ffctx->inode,
                                                 &ffctx->sme_cache);
        if (rc == OBMAFS3_OK)
            rc = src;
        ffctx->inode_dirty = 1;
    }

    /* Write back the cached inode */
    if (ffctx->inode_dirty) {
        int put_rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
        if (rc == OBMAFS3_OK)
            rc = put_rc;
        ffctx->inode_dirty = 0;
    }

    return (rc == OBMAFS3_OK) ? 0 : -EIO;
}

static int obmafs3_fuse_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    if (!ffctx)
        return 0;

    int rc = 0;

    /* Flush the dedup block accumulator */
    if (ffctx->sector_size && ffctx->db_cache.initialized) {
        int drc = obmafs3_flush_dedup_block_cache(g_ctx,
                      ffctx->sector_size, &ffctx->db_cache);
        if (rc == OBMAFS3_OK)
            rc = drc;
    }

    /* Flush any remaining cached data (in case flush was not called) */
    if (ffctx->sector_size && ffctx->sme_cache.count > 0) {
        int src = obmafs3_flush_sector_map_cache(g_ctx, &ffctx->inode,
                                                 &ffctx->sme_cache);
        if (rc == OBMAFS3_OK)
            rc = src;
        ffctx->inode_dirty = 1;
    }

    /* Write back the cached inode */
    if (ffctx->inode_dirty) {
        int put_rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
        if (rc == OBMAFS3_OK)
            rc = put_rc;
    }

    obmafs3_free_dedup_block_cache(&ffctx->db_cache);
    obmafs3_free_sector_map_cache(&ffctx->sme_cache);
    free(ffctx);
    fi->fh = 0;

    return (rc == OBMAFS3_OK) ? 0 : -EIO;
}

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
    .utimens  = obmafs3_fuse_utimens,
};
