/*
 * fuse_ops.c - OBMAFS3 FUSE filesystem operations
 *
 * Implements read-write access for regular files:
 *   getattr, readdir, open, read, create, write, truncate, unlink, utimens
 */

#include "fuse_ops.h"
#include "tags.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <linux/stat.h>
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
    struct inode_record inode;         /* cached inode */
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
/*  FUSE callbacks                                                     */
/* ------------------------------------------------------------------ */

static int obmafs3_fuse_getattr(const char *path, struct stat *stbuf,
                                struct fuse_file_info *fi)
{
    struct inode_record inode;
    struct inode_record *ip;
    int rc;

    memset(stbuf, 0, sizeof(*stbuf));

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    if (strcmp(path, "/") == 0) {
        rc = obmafs3_inode_get(g_ctx, OBMAFS3_ROOT_INODE_ID, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;

        stbuf->st_ino     = inode.inode_id;
        stbuf->st_mode    = S_IFDIR | inode.mode;
        stbuf->st_nlink   = inode.ref_count;
        stbuf->st_uid     = inode.uid;
        stbuf->st_gid     = inode.gid;
        stbuf->st_size    = (off_t)inode.file_size;
        stbuf->st_blksize = (blksize_t)g_ctx->sb.block_size;
        stbuf->st_blocks  = (blkcnt_t)((inode.file_size + 511) / 512);
        stbuf->st_atime   = (time_t)inode.access_time;
        stbuf->st_mtime   = (time_t)inode.modification_time;
        stbuf->st_ctime   = (time_t)inode.creation_time;
        return 0;
    }

    uint64_t parent_id;
    const char *name;
    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    struct catalog_record cat_entry;
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
    else if (ip->file_type == kFileTypeSymlink)
        stbuf->st_mode = S_IFLNK | 0777;
    else
        stbuf->st_mode = S_IFREG | ip->mode;
    stbuf->st_nlink   = ip->ref_count;
    stbuf->st_uid     = ip->uid;
    stbuf->st_gid     = ip->gid;
    stbuf->st_size    = (off_t)ip->file_size;
    stbuf->st_blksize = (blksize_t)g_ctx->sb.block_size;
    stbuf->st_blocks  = (blkcnt_t)((ip->file_size + 511) / 512);
    stbuf->st_atime   = (time_t)ip->access_time;
    stbuf->st_mtime   = (time_t)ip->modification_time;
    stbuf->st_ctime   = (time_t)ip->creation_time;
    return 0;
}

static int obmafs3_fuse_readdir(const char *path, void *buf,
                                fuse_fill_dir_t filler, off_t offset,
                                struct fuse_file_info *fi,
                                enum fuse_readdir_flags flags)
{
    struct catalog_record *entries = NULL;
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
        struct catalog_record cat_entry;

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
    struct catalog_record cat_entry;
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
    struct inode_record inode;
    struct inode_record *ip;
    int rc;

    if (ffctx) {
        ip = &ffctx->inode;
    } else {
        uint64_t parent_id;
        const char *name;
        struct catalog_record cat_entry;
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
    struct catalog_record cat_entry;
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
    struct catalog_record new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
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

    struct inode_record new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
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
    new_inode.ref_count          = 1;

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
    struct inode_record inode;
    struct inode_record *ip;
    int rc;

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    if (ffctx) {
        ip = &ffctx->inode;
    } else {
        uint64_t parent_id;
        const char *name;
        struct catalog_record cat_entry;
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

        /* Start the background compression worker on the first write */
        if (dbc && !dbc->bg_compress) {
            int brc = obmafs3_bg_compress_start(g_ctx, dbc);
            if (brc != OBMAFS3_OK)
                return -EIO;
        }

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
    struct catalog_record cat_entry;
    struct inode_record inode;
    struct inode_record *ip;
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

static int obmafs3_fuse_link(const char *oldpath, const char *newpath)
{
    uint64_t old_parent_id, new_parent_id;
    const char *old_name, *new_name;
    struct catalog_record cat_entry;
    int rc;

    /* Resolve source */
    rc = resolve_path(oldpath, &old_parent_id, &old_name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, old_parent_id, old_name, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Hardlinks to directories are not allowed */
    if (cat_entry.directory_flag)
        return -EPERM;

    /* Resolve destination */
    rc = resolve_path(newpath, &new_parent_id, &new_name);
    if (rc != 0)
        return rc;

    /* Check destination doesn't already exist */
    struct catalog_record tmp;
    rc = obmafs3_catalog_lookup(g_ctx, new_parent_id, new_name, &tmp);
    if (rc == OBMAFS3_OK)
        return -EEXIST;
    if (rc != OBMAFS3_ERR_NOTFOUND)
        return -EIO;

    /* Create new catalog entry pointing to the same inode */
    struct catalog_record new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
    new_cat.inode_id       = cat_entry.inode_id;
    new_cat.parent_id      = new_parent_id;
    new_cat.directory_flag = 0;
    strncpy(new_cat.name, new_name, sizeof(new_cat.name) - 1);

    rc = obmafs3_catalog_insert(g_ctx, &new_cat);
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Increment the reference count */
    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    inode.ref_count++;
    rc = obmafs3_inode_put(g_ctx, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_symlink(const char *target, const char *linkpath)
{
    uint64_t parent_id;
    const char *name;
    struct catalog_record cat_entry;
    int rc;

    rc = resolve_path(linkpath, &parent_id, &name);
    if (rc != 0)
        return rc;

    /* Check if the name already exists */
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_OK)
        return -EEXIST;
    if (rc != OBMAFS3_ERR_NOTFOUND)
        return -EIO;

    /* Allocate a new inode ID */
    uint64_t new_inode_id = obmafs3_alloc_inode_id(g_ctx);

    /* Create the catalog entry */
    struct catalog_record new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
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

    struct inode_record new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.inode_id          = new_inode_id;
    new_inode.uid               = fctx->uid;
    new_inode.gid               = fctx->gid;
    new_inode.mode              = 0777;
    new_inode.creation_time     = now;
    new_inode.modification_time = now;
    new_inode.access_time       = now;
    new_inode.file_size         = 0;
    new_inode.file_type         = kFileTypeSymlink;
    new_inode.ref_count         = 1;

    /* Write the symlink target as file data in the first extent */
    size_t target_len = strlen(target);
    rc = obmafs3_write_file_data(g_ctx, &new_inode, 0, target, target_len);
    if (rc != OBMAFS3_OK)
        return -EIO;

    rc = obmafs3_inode_put(g_ctx, &new_inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_readlink(const char *path, char *buf, size_t size)
{
    uint64_t parent_id;
    const char *name;
    struct catalog_record cat_entry;
    struct inode_record inode;
    int rc;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (inode.file_type != kFileTypeSymlink)
        return -EINVAL;

    /* Read the target from file data */
    size_t to_read = inode.file_size;
    if (to_read >= size)
        to_read = size - 1;

    rc = obmafs3_read_file_data(g_ctx, &inode, 0, buf, to_read);
    if (rc != OBMAFS3_OK)
        return -EIO;

    buf[to_read] = '\0';
    return 0;
}

static int obmafs3_fuse_unlink(const char *path)
{
    uint64_t parent_id;
    const char *name;
    struct catalog_record cat_entry;
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

    /* Decrement the reference count; delete inode only when it reaches 0 */
    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (inode.ref_count > 1) {
        inode.ref_count--;
        rc = obmafs3_inode_put(g_ctx, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
    } else {
        /* Last reference — delete media tags and then the inode */
        if (inode.file_type == kFileTypeMediaImage)
            obmafs3_media_tag_delete_all(g_ctx, cat_entry.inode_id);

        rc = obmafs3_inode_delete(g_ctx, cat_entry.inode_id);
        if (rc != OBMAFS3_OK)
            return -EIO;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Directory operations                                               */
/* ------------------------------------------------------------------ */

static int obmafs3_fuse_mkdir(const char *path, mode_t mode)
{
    uint64_t parent_id;
    const char *name;
    struct catalog_record cat_entry;
    int rc;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    /* Check if the name already exists */
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_OK)
        return -EEXIST;
    if (rc != OBMAFS3_ERR_NOTFOUND)
        return -EIO;

    /* Allocate a new inode ID */
    uint64_t new_inode_id = obmafs3_alloc_inode_id(g_ctx);

    /* Create the catalog entry */
    struct catalog_record new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
    new_cat.inode_id       = new_inode_id;
    new_cat.parent_id      = parent_id;
    new_cat.directory_flag = 1;
    strncpy(new_cat.name, name, sizeof(new_cat.name) - 1);

    rc = obmafs3_catalog_insert(g_ctx, &new_cat);
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Create the inode */
    uint64_t now = (uint64_t)time(NULL);
    struct fuse_context *fctx = fuse_get_context();

    struct inode_record new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.inode_id          = new_inode_id;
    new_inode.uid               = fctx->uid;
    new_inode.gid               = fctx->gid;
    new_inode.mode              = mode & 07777;
    new_inode.creation_time     = now;
    new_inode.modification_time = now;
    new_inode.access_time       = now;
    new_inode.file_size         = 0;
    new_inode.file_type         = kFileTypeDirectory;
    new_inode.ref_count          = 1;

    rc = obmafs3_inode_put(g_ctx, &new_inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_rmdir(const char *path)
{
    uint64_t parent_id;
    const char *name;
    struct catalog_record cat_entry;
    int rc;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (!cat_entry.directory_flag)
        return -ENOTDIR;

    /* Check that the directory is empty */
    struct catalog_record *children = NULL;
    uint32_t child_count = 0;
    rc = obmafs3_catalog_list(g_ctx, cat_entry.inode_id,
                              &children, &child_count);
    if (rc != OBMAFS3_OK)
        return -EIO;
    obmafs3_catalog_list_free(children);

    if (child_count > 0)
        return -ENOTEMPTY;

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
    struct catalog_record cat_entry;
    struct inode_record inode;
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

static int obmafs3_fuse_chmod(const char *path, mode_t mode,
                              struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct catalog_record cat_entry;
    struct inode_record inode;
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

    inode.mode = mode & 07777;

    rc = obmafs3_inode_put(g_ctx, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_chown(const char *path, uid_t uid, gid_t gid,
                              struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct catalog_record cat_entry;
    struct inode_record inode;
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

    if (uid != (uid_t)-1)
        inode.uid = uid;
    if (gid != (gid_t)-1)
        inode.gid = gid;

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

static int obmafs3_fuse_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;

    uint64_t block_size   = g_ctx->sb.block_size;
    uint64_t total_blocks = g_ctx->sb.total_bytes / block_size;

    /* Count free blocks from the in-memory allocation bitmap */
    uint64_t used = 0;
    for (uint64_t i = 0; i < g_ctx->bitmap_size; i++)
        used += (uint64_t)__builtin_popcount(g_ctx->bitmap[i]);

    uint64_t free_blocks = total_blocks > used ? total_blocks - used : 0;

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = block_size;
    stbuf->f_frsize  = block_size;
    stbuf->f_blocks  = total_blocks;
    stbuf->f_bfree   = free_blocks;
    stbuf->f_bavail  = free_blocks;
    stbuf->f_files   = g_ctx->sb.next_inode_id - 1;
    stbuf->f_ffree   = 0;    /* inodes allocated on demand, no fixed limit */
    stbuf->f_namemax = 255;

    return 0;
}

static int obmafs3_fuse_statx(const char *path, int flags, int mask,
                              struct statx *stxbuf,
                              struct fuse_file_info *fi)
{
    struct inode_record inode;
    struct inode_record *ip;
    int rc;
    int is_dir = 0;

    (void)flags;

    struct fuse_file_ctx *ffctx = fi
        ? (struct fuse_file_ctx *)(uintptr_t)fi->fh
        : NULL;

    memset(stxbuf, 0, sizeof(*stxbuf));

    if (strcmp(path, "/") == 0) {
        rc = obmafs3_inode_get(g_ctx, OBMAFS3_ROOT_INODE_ID, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;
        ip = &inode;
        is_dir = 1;
    } else {
        uint64_t parent_id;
        const char *name;
        rc = resolve_path(path, &parent_id, &name);
        if (rc != 0)
            return rc;

        struct catalog_record cat_entry;
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
        is_dir = cat_entry.directory_flag ||
                 ip->file_type == kFileTypeDirectory;
    }

    stxbuf->stx_mask = STATX_BASIC_STATS | STATX_BTIME;
    stxbuf->stx_blksize = (__u32)g_ctx->sb.block_size;
    stxbuf->stx_nlink = (__u32)ip->ref_count;
    stxbuf->stx_uid   = (__u32)ip->uid;
    stxbuf->stx_gid   = (__u32)ip->gid;
    stxbuf->stx_ino   = ip->inode_id;
    stxbuf->stx_size  = ip->file_size;
    stxbuf->stx_blocks = (ip->file_size + 511) / 512;

    if (is_dir)
        stxbuf->stx_mode = S_IFDIR | ip->mode;
    else if (ip->file_type == kFileTypeSymlink)
        stxbuf->stx_mode = S_IFLNK | 0777;
    else
        stxbuf->stx_mode = S_IFREG | ip->mode;

    stxbuf->stx_atime.tv_sec  = (__s64)ip->access_time;
    stxbuf->stx_mtime.tv_sec  = (__s64)ip->modification_time;
    stxbuf->stx_ctime.tv_sec  = (__s64)ip->creation_time;
    stxbuf->stx_btime.tv_sec  = (__s64)ip->creation_time;

    stxbuf->stx_attributes_mask |= STATX_ATTR_COMPRESSED;
    if (ip->file_type == kFileTypeMediaImage && g_ctx->compression)
        stxbuf->stx_attributes |= STATX_ATTR_COMPRESSED;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Media tag xattr / ioctl support                                    */
/* ------------------------------------------------------------------ */

#define MEDIATAG_XATTR_PREFIX     "user.mediatag."
#define MEDIATAG_XATTR_PREFIX_LEN 14   /* strlen("user.mediatag.") */

/** Map MediaTagType ordinal to xattr suffix name.  NULL = unused slot. */
static const char *media_tag_xattr_names[] = {
    [kCdTableOfContents]               = "cd_toc",
    [kCdSessionInfo]                    = "cd_session_info",
    [kCdFullTOC]                        = "cd_full_toc",
    [kCdPMA]                            = "cd_pma",
    [kCdATIP]                           = "cd_atip",
    [kCdTEXT]                           = "cd_text",
    [kCdMCN]                            = "cd_mcn",
    [kDvdPFI]                           = "dvd_pfi",
    [kDvdCMI]                           = "dvd_cmi",
    [kDvdDiscKey]                       = "dvd_disc_key",
    [kDvdBCA]                           = "dvd_bca",
    [kDvdDMI]                           = "dvd_dmi",
    [kDvdMediaIdentifier]               = "dvd_media_id",
    [kDvdMKB]                           = "dvd_mkb",
    [kDvdRamDDS]                        = "dvd_ram_dds",
    [kDvdRamMediumStatus]               = "dvd_ram_medium_status",
    [kDvdRamSpareArea]                  = "dvd_ram_spare_area",
    [kDvdRecordableRMD]                 = "dvd_recordable_rmd",
    [kDvdRecordablePreRecordedInfo]     = "dvd_recordable_pre_recorded_info",
    [kDvdRecordableMediaIdentifier]     = "dvd_recordable_media_id",
    [kDvdRecordablePFI]                 = "dvd_recordable_pfi",
    [kDvdADIP]                          = "dvd_adip",
    [kHdDvdCPI]                         = "hddvd_cpi",
    [kHdDvdMediumStatus]                = "hddvd_medium_status",
    [kDvdDlLayerCapacity]               = "dvd_dl_layer_capacity",
    [kDvdDlMiddleZoneAddress]           = "dvd_dl_middle_zone_address",
    [kDvdDlJumpIntervalSize]            = "dvd_dl_jump_interval_size",
    [kDvdDlManualLayerJumpLBA]          = "dvd_dl_manual_layer_jump_lba",
    [kBdDI]                             = "bd_di",
    [kBdBCA]                            = "bd_bca",
    [kBdDDS]                            = "bd_dds",
    [kBdCartridgeStatus]                = "bd_cartridge_status",
    [kBdSpareArea]                      = "bd_spare_area",
    [kAACS_VolumeIdentifier]            = "aacs_volume_id",
    [kAACS_SerialNumber]                = "aacs_serial_number",
    [kAACS_MediaIdentifier]             = "aacs_media_id",
    [kAACS_MKB]                         = "aacs_mkb",
    [kAACS_DataKeys]                    = "aacs_data_keys",
    [kAACS_LBAExtents]                  = "aacs_lba_extents",
    [kAACS_CPRM_MKB]                    = "aacs_cprm_mkb",
    [kHybrid_RecognizedLayers]          = "hybrid_recognized_layers",
    [kMMC_WriteProtection]              = "mmc_write_protection",
    [kMMC_DiscInformation]              = "mmc_disc_information",
    [kMMC_TrackResourcesInformation]    = "mmc_track_resources",
    [kMMC_POWResourcesInformation]      = "mmc_pow_resources",
    [kSCSI_INQUIRY]                     = "scsi_inquiry",
    [kSCSI_MODEPAGE_2A]                 = "scsi_modepage_2a",
    [kATA_IDENTIFY]                     = "ata_identify",
    [kATAPI_IDENTIFY]                   = "atapi_identify",
    [kPCMCIA_CIS]                       = "pcmcia_cis",
    [kSecureDigital_CID]                = "sd_cid",
    [kSecureDigital_CSD]                = "sd_csd",
    [kSecureDigital_SCR]                = "sd_scr",
    [kSecureDigital_OCR]                = "sd_ocr",
    [kMMC_CID]                          = "mmc_cid",
    [kMMC_CSD]                          = "mmc_csd",
    [kMMC_OCR]                          = "mmc_ocr",
    [kMMC_ExtendedCSD]                  = "mmc_extended_csd",
    [kXbox_SecuritySector]              = "xbox_security_sector",
    [kFloppy_LeadOut]                   = "floppy_lead_out",
    [kDiscControlBlock]                 = "disc_control_block",
    [kCD_FirstTrackPregap]              = "cd_first_track_pregap",
    [kCD_LeadOut]                       = "cd_lead_out",
    [kSCSI_MODESENSE_6]                 = "scsi_mode_sense_6",
    [kSCSI_MODESENSE_10]                = "scsi_mode_sense_10",
    [kUSB_Descriptors]                  = "usb_descriptors",
    [kXbox_DMI]                         = "xbox_dmi",
    [kXbox_PFI]                         = "xbox_pfi",
    [kMiniDiscType]                     = "minidisc_type",
    [kMiniDiscD5]                       = "minidisc_d5",
    [kMiniDiscUTOC]                     = "minidisc_utoc",
    [kMiniDiscDTOC]                     = "minidisc_dtoc",
    [kDVD_DiscKey_Decrypted]            = "dvd_disc_key_decrypted",
    [kDVD_PFI_2ndLayer]                 = "dvd_pfi_2nd_layer",
    [kFloppy_WriteProtect]              = "floppy_write_protect",
};

#define MEDIA_TAG_XATTR_COUNT \
    (sizeof(media_tag_xattr_names) / sizeof(media_tag_xattr_names[0]))

/** Parse "user.mediatag.<name>" and return the tag type, or -1 on error. */
static int parse_mediatag_xattr(const char *name)
{
    if (strncmp(name, MEDIATAG_XATTR_PREFIX, MEDIATAG_XATTR_PREFIX_LEN) != 0)
        return -1;

    const char *tag_name = name + MEDIATAG_XATTR_PREFIX_LEN;

    for (size_t i = 0; i < MEDIA_TAG_XATTR_COUNT; i++) {
        if (media_tag_xattr_names[i] &&
            strcmp(tag_name, media_tag_xattr_names[i]) == 0)
            return (int)i;
    }

    return -1;
}

/** Check whether a name starts with the mediatag xattr prefix. */
static int is_mediatag_xattr(const char *name)
{
    return strncmp(name, MEDIATAG_XATTR_PREFIX,
                   MEDIATAG_XATTR_PREFIX_LEN) == 0;
}

static int obmafs3_fuse_getxattr(const char *path, const char *name,
                                  char *value, size_t size)
{
    if (!is_mediatag_xattr(name))
        return -ENODATA;

    int tag_type = parse_mediatag_xattr(name);
    if (tag_type < 0)
        return -ENODATA;

    if (strcmp(path, "/") == 0)
        return -ENODATA;

    uint64_t parent_id;
    const char *fname;
    int rc = resolve_path(path, &parent_id, &fname);
    if (rc != 0)
        return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (inode.file_type != kFileTypeMediaImage)
        return -ENODATA;

    void *data;
    uint32_t data_length;
    rc = obmafs3_media_tag_get(g_ctx, cat_entry.inode_id,
                               (uint16_t)tag_type, &data, &data_length);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENODATA;
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (size == 0) {
        obmafs3_media_tag_data_free(data);
        return (int)data_length;
    }

    if (size < data_length) {
        obmafs3_media_tag_data_free(data);
        return -ERANGE;
    }

    memcpy(value, data, data_length);
    obmafs3_media_tag_data_free(data);
    return (int)data_length;
}

static int obmafs3_fuse_setxattr(const char *path, const char *name,
                                  const char *value, size_t size, int flags)
{
    (void)flags;

    if (!is_mediatag_xattr(name))
        return -ENOTSUP;

    int tag_type = parse_mediatag_xattr(name);
    if (tag_type < 0)
        return -ENOTSUP;

    if (strcmp(path, "/") == 0)
        return -ENOTSUP;

    uint64_t parent_id;
    const char *fname;
    int rc = resolve_path(path, &parent_id, &fname);
    if (rc != 0)
        return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (inode.file_type != kFileTypeMediaImage)
        return -ENOTSUP;

    rc = obmafs3_media_tag_put(g_ctx, cat_entry.inode_id,
                                (uint16_t)tag_type, value,
                                (uint32_t)size);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_listxattr(const char *path, char *list, size_t size)
{
    if (strcmp(path, "/") == 0)
        return 0;

    uint64_t parent_id;
    const char *fname;
    int rc = resolve_path(path, &parent_id, &fname);
    if (rc != 0)
        return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (inode.file_type != kFileTypeMediaImage)
        return 0;

    uint16_t *tag_types;
    uint32_t count;
    rc = obmafs3_media_tag_list(g_ctx, cat_entry.inode_id,
                                &tag_types, &count);
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Calculate total size of all xattr names */
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (tag_types[i] < MEDIA_TAG_XATTR_COUNT &&
            media_tag_xattr_names[tag_types[i]]) {
            total += MEDIATAG_XATTR_PREFIX_LEN +
                     strlen(media_tag_xattr_names[tag_types[i]]) + 1;
        }
    }

    if (size == 0) {
        obmafs3_media_tag_list_free(tag_types);
        return (int)total;
    }

    if (size < total) {
        obmafs3_media_tag_list_free(tag_types);
        return -ERANGE;
    }

    char *p = list;
    for (uint32_t i = 0; i < count; i++) {
        if (tag_types[i] < MEDIA_TAG_XATTR_COUNT &&
            media_tag_xattr_names[tag_types[i]]) {
            int n = snprintf(p, size - (size_t)(p - list),
                             "%s%s", MEDIATAG_XATTR_PREFIX,
                             media_tag_xattr_names[tag_types[i]]);
            p += n + 1;   /* include NUL terminator */
        }
    }

    obmafs3_media_tag_list_free(tag_types);
    return (int)total;
}

static int obmafs3_fuse_removexattr(const char *path, const char *name)
{
    if (!is_mediatag_xattr(name))
        return -ENOTSUP;

    int tag_type = parse_mediatag_xattr(name);
    if (tag_type < 0)
        return -ENOTSUP;

    if (strcmp(path, "/") == 0)
        return -ENOTSUP;

    uint64_t parent_id;
    const char *fname;
    int rc = resolve_path(path, &parent_id, &fname);
    if (rc != 0)
        return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if (inode.file_type != kFileTypeMediaImage)
        return -ENOTSUP;

    rc = obmafs3_media_tag_delete(g_ctx, cat_entry.inode_id,
                                   (uint16_t)tag_type);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENODATA;
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

/* ---- ioctl for media tags ---- */

#define OBMAFS3_IOC_MAX_TAG_DATA 16368

struct obmafs3_ioctl_tag_arg {
    uint16_t tag_type;
    uint32_t data_length;
    uint8_t  data[OBMAFS3_IOC_MAX_TAG_DATA];
};

#define OBMAFS3_IOC_SET_MEDIA_TAG \
    _IOW('O', 1, struct obmafs3_ioctl_tag_arg)
#define OBMAFS3_IOC_GET_MEDIA_TAG \
    _IOWR('O', 2, struct obmafs3_ioctl_tag_arg)

static int obmafs3_fuse_ioctl(const char *path, unsigned int cmd,
                               void *arg, struct fuse_file_info *fi,
                               unsigned int flags, void *data)
{
    (void)path;
    (void)arg;
    (void)flags;

    struct fuse_file_ctx *ffctx =
        fi ? (struct fuse_file_ctx *)(uintptr_t)fi->fh : NULL;
    if (!ffctx)
        return -EBADF;

    if (ffctx->inode.file_type != kFileTypeMediaImage)
        return -ENOTTY;

    struct obmafs3_ioctl_tag_arg *tag_arg =
        (struct obmafs3_ioctl_tag_arg *)data;

    switch (cmd) {
    case OBMAFS3_IOC_SET_MEDIA_TAG: {
        if (!tag_arg || tag_arg->data_length > OBMAFS3_IOC_MAX_TAG_DATA)
            return -EINVAL;
        int rc = obmafs3_media_tag_put(g_ctx, ffctx->inode_id,
                                        tag_arg->tag_type,
                                        tag_arg->data,
                                        tag_arg->data_length);
        return rc == OBMAFS3_OK ? 0 : -EIO;
    }

    case OBMAFS3_IOC_GET_MEDIA_TAG: {
        if (!tag_arg)
            return -EINVAL;
        void *buf;
        uint32_t length;
        int rc = obmafs3_media_tag_get(g_ctx, ffctx->inode_id,
                                        tag_arg->tag_type,
                                        &buf, &length);
        if (rc == OBMAFS3_ERR_NOTFOUND)
            return -ENODATA;
        if (rc != OBMAFS3_OK)
            return -EIO;
        if (length > OBMAFS3_IOC_MAX_TAG_DATA) {
            obmafs3_media_tag_data_free(buf);
            return -ERANGE;
        }
        tag_arg->data_length = length;
        memcpy(tag_arg->data, buf, length);
        obmafs3_media_tag_data_free(buf);
        return 0;
    }

    default:
        return -ENOTTY;
    }
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
