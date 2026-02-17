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
    struct inode_record     inode;         /* cached inode */
    int                     inode_dirty;   /* needs write-back on release */
    struct sector_map_cache sme_cache;
    struct dedup_block_cache db_cache;     /* persistent dedup block accumulator */
    struct cd_sector_map_cache cd_sme_cache; /* CD sector map cache */
    void                   *ecc_ctx;       /* CD ECC context (lazy-init) */
    int64_t                 cd_next_sector; /* next expected CD sector LBA */
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

    /* Flush cached CD sector_map_entries */
    if (ffctx->cd_sme_cache.count > 0) {
        int crc = obmafs3_flush_cd_sector_map_cache(g_ctx, &ffctx->inode,
                                                    &ffctx->cd_sme_cache);
        if (rc == OBMAFS3_OK)
            rc = crc;
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

    /* Flush remaining CD sector map entries */
    if (ffctx->cd_sme_cache.count > 0) {
        int crc = obmafs3_flush_cd_sector_map_cache(g_ctx, &ffctx->inode,
                                                    &ffctx->cd_sme_cache);
        if (rc == OBMAFS3_OK)
            rc = crc;
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
    obmafs3_free_cd_sector_map_cache(&ffctx->cd_sme_cache);
    if (ffctx->ecc_ctx)
        ecc_cd_free(ffctx->ecc_ctx);
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

/* ---- ioctl for compact disc images ---- */

#define OBMAFS3_IOC_SET_CD_IMAGE \
    _IO('O', 3)

#define CD_RAW_SECTOR_SIZE  2352
#define CD_RAW_PLUS_SUB     2448
#define CD_SUBCHANNEL_SIZE  96
#define CD_PREFIX_SIZE      16
#define CD_SUFFIX_SIZE      288
#define CD_DATA_SIZE        2048   /* 2352 - 16 - 288 */

struct obmafs3_ioctl_cd_write_arg {
    uint32_t buffer_size;  /**< 2352 or 2448 */
    uint8_t  sector_mode;  /**< enum obmafs3_cd_sector_mode */
    uint8_t  buffer[CD_RAW_PLUS_SUB];
};

#define OBMAFS3_IOC_CD_WRITE_LONG \
    _IOW('O', 4, struct obmafs3_ioctl_cd_write_arg)

struct obmafs3_ioctl_cd_read_arg {
    int64_t  sector;                    /**< Sector LBA to read */
    uint8_t  buffer[CD_RAW_SECTOR_SIZE]; /**< Output: reconstructed 2352-byte sector */
};

#define OBMAFS3_IOC_CD_READ_LONG \
    _IOWR('O', 5, struct obmafs3_ioctl_cd_read_arg)

struct obmafs3_ioctl_cd_read_full_arg {
    int64_t  sector;                    /**< Sector LBA to read */
    uint8_t  buffer[CD_RAW_PLUS_SUB];   /**< Output: 2352 raw + 96 subchannel */
};

#define OBMAFS3_IOC_CD_READ_LONG_SUB \
    _IOWR('O', 6, struct obmafs3_ioctl_cd_read_full_arg)

/* ---- Image metadata ioctls ---- */

struct obmafs3_ioctl_metadata_set_arg {
    char key[METADATA_KEY_MAX];       /**< Metadata key (NUL-terminated) */
    char value[METADATA_VALUE_MAX];   /**< Metadata value (NUL-terminated) */
};

#define OBMAFS3_IOC_SET_METADATA \
    _IOW('O', 7, struct obmafs3_ioctl_metadata_set_arg)

struct obmafs3_ioctl_metadata_get_arg {
    char key[METADATA_KEY_MAX];       /**< Input: key to look up */
    char value[METADATA_VALUE_MAX];   /**< Output: value */
};

#define OBMAFS3_IOC_GET_METADATA \
    _IOWR('O', 8, struct obmafs3_ioctl_metadata_get_arg)

struct obmafs3_ioctl_metadata_delete_arg {
    char key[METADATA_KEY_MAX];       /**< Key to delete */
};

#define OBMAFS3_IOC_DELETE_METADATA \
    _IOW('O', 9, struct obmafs3_ioctl_metadata_delete_arg)

/**
 * List metadata keys for an image.  Paginated: set offset to 0 for the
 * first page, then advance by count for subsequent pages.  Returns
 * count == 0 when no more keys remain.
 */
struct obmafs3_ioctl_metadata_list_arg {
    uint32_t offset;             /**< Input: starting offset */
    uint32_t count;              /**< Output: keys returned */
    char     keys[16][METADATA_KEY_MAX]; /**< Output: up to 16 keys */
};

#define OBMAFS3_IOC_LIST_METADATA \
    _IOWR('O', 10, struct obmafs3_ioctl_metadata_list_arg)

/**
 * Query which images have a given key=value pair.
 * Returns a paginated list of inode_ids.
 */
struct obmafs3_ioctl_metadata_query_arg {
    char     key[METADATA_KEY_MAX];       /**< Input: key */
    char     value[METADATA_VALUE_MAX];   /**< Input: value */
    uint32_t offset;                      /**< Input: starting offset */
    uint32_t count;                       /**< Output: inode_ids returned */
    uint64_t inode_ids[64];               /**< Output: up to 64 inode_ids */
};

#define OBMAFS3_IOC_QUERY_METADATA \
    _IOWR('O', 11, struct obmafs3_ioctl_metadata_query_arg)

/**
 * Check if the 16-byte prefix of a raw CD sector matches the expected
 * sync + MSF + mode for the given LBA and track mode.
 */
static bool cd_prefix_is_generatable(const uint8_t *sector,
                                     int64_t lba, uint8_t mode)
{
    /* Build expected prefix */
    uint8_t expected[CD_PREFIX_SIZE];

    /* Sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00 */
    expected[0]  = 0x00;
    for (int i = 1; i <= 10; i++)
        expected[i] = 0xFF;
    expected[11] = 0x00;

    /* MSF in BCD */
    uint8_t minute, second, frame;
    cd_lba_to_msf(lba, &minute, &second, &frame);
    expected[12] = (uint8_t)(((minute / 10) << 4) + minute % 10);
    expected[13] = (uint8_t)(((second / 10) << 4) + second % 10);
    expected[14] = (uint8_t)(((frame  / 10) << 4) + frame  % 10);

    /* Mode byte */
    switch ((enum obmafs3_cd_sector_mode)mode) {
    case kCdSectorMode1:
        expected[15] = 0x01;
        break;
    case kCdSectorMode2:
    case kCdSectorMode2Form1:
    case kCdSectorMode2Form2:
        expected[15] = 0x02;
        break;
    default:
        return false;
    }

    return memcmp(sector, expected, CD_PREFIX_SIZE) == 0;
}

static int obmafs3_cd_write_long(struct fuse_file_ctx *ffctx,
                                 const struct obmafs3_ioctl_cd_write_arg *arg)
{
    if (!arg)
        return -EINVAL;

    uint32_t bufsz = arg->buffer_size;
    if (bufsz != CD_RAW_SECTOR_SIZE && bufsz != CD_RAW_PLUS_SUB)
        return -EINVAL;

    uint8_t mode = arg->sector_mode;
    if (mode > kCdSectorMode2Form2)
        return -EINVAL;

    const uint8_t *raw = arg->buffer;
    int64_t sector_lba = ffctx->cd_next_sector;

    /* Build the cd_sector_map_entry */
    struct cd_sector_map_entry sme;
    memset(&sme, 0, sizeof(sme));
    sme.sector      = sector_lba;
    sme.sector_mode = mode;

    /* --- Subchannel handling --- */
    if (bufsz == CD_RAW_PLUS_SUB) {
        const uint8_t *sub = raw + CD_RAW_SECTOR_SIZE;
        uint64_t sub_hash = obmafs3_checksum_xxh64(sub, CD_SUBCHANNEL_SIZE);
        sme.subchannel_hash = sub_hash;

        /* Store subchannel data in the tree (dedup by hash) */
        uint8_t existing[CD_SUBCHANNEL_DATA_SIZE];
        int rc = obmafs3_cd_subchannel_get(g_ctx, sub_hash, existing);
        if (rc == OBMAFS3_ERR_NOTFOUND) {
            rc = obmafs3_cd_subchannel_put(g_ctx, sub_hash, sub);
            if (rc != OBMAFS3_OK)
                return -EIO;
        } else if (rc != OBMAFS3_OK) {
            return -EIO;
        }
    }

    /* --- Audio mode: entire 2352 bytes stored as data, no prefix/suffix --- */
    if (mode == kCdSectorModeAudio) {
        sme.sector_size      = CD_RAW_SECTOR_SIZE;
        sme.generated_prefix = 0;
        sme.generated_suffix = 0;

        uint64_t hash = obmafs3_checksum_xxh64(raw, CD_RAW_SECTOR_SIZE);
        sme.hash = hash;

        /* Dedup the 2352-byte audio sector */
        struct btree_header dedup_hdr;
        uint64_t dedup_hdr_lba;
        int rc = obmafs3_dedup_get_tree(g_ctx, CD_RAW_SECTOR_SIZE,
                                        &dedup_hdr, &dedup_hdr_lba);
        if (rc != OBMAFS3_OK)
            return -EIO;

        struct dedup_entry existing;
        rc = obmafs3_dedup_lookup(g_ctx, &dedup_hdr, hash, &existing);
        if (rc == OBMAFS3_ERR_NOTFOUND) {
            /* New sector — store via the write path */
            if (!ffctx->db_cache.initialized) {
                /* Bootstrap dedup block cache for 2352-byte sectors */
                ffctx->sector_size = CD_RAW_SECTOR_SIZE;
            }
            struct dedup_block_cache *dbc = &ffctx->db_cache;
            if (!dbc->bg_compress) {
                int brc = obmafs3_bg_compress_start(g_ctx, dbc);
                if (brc != OBMAFS3_OK)
                    return -EIO;
            }

            /* Write via the media image data path (handles dedup storage) */
            uint64_t offset = (uint64_t)sector_lba * CD_RAW_SECTOR_SIZE;
            struct sector_map_cache dummy_cache; /* unused */
            memset(&dummy_cache, 0, sizeof(dummy_cache));
            rc = obmafs3_write_media_image_data(g_ctx, &ffctx->inode,
                                                offset, raw,
                                                CD_RAW_SECTOR_SIZE,
                                                CD_RAW_SECTOR_SIZE,
                                                &dummy_cache, dbc);
            obmafs3_free_sector_map_cache(&dummy_cache);
            if (rc != OBMAFS3_OK)
                return -EIO;
        } else if (rc != OBMAFS3_OK) {
            return -EIO;
        }

        goto cache_and_done;
    }

    /* --- Data modes (Mode 1, Mode 2, Mode 2 Form 1, Mode 2 Form 2) --- */

    /* Lazy-init the ECC context */
    if (!ffctx->ecc_ctx) {
        ffctx->ecc_ctx = ecc_cd_init();
        if (!ffctx->ecc_ctx)
            return -ENOMEM;
    }

    /* Check if prefix is generatable */
    bool pfx_gen = cd_prefix_is_generatable(raw, sector_lba, mode);
    sme.generated_prefix = pfx_gen ? 1 : 0;

    if (!pfx_gen) {
        /* Store the non-generatable prefix */
        uint64_t pfx_hash = obmafs3_checksum_xxh64(raw, CD_PREFIX_SIZE);
        sme.prefix_hash = pfx_hash;

        uint8_t existing[CD_PREFIX_DATA_SIZE];
        int rc = obmafs3_cd_prefix_get(g_ctx, pfx_hash, existing);
        if (rc == OBMAFS3_ERR_NOTFOUND) {
            rc = obmafs3_cd_prefix_put(g_ctx, pfx_hash, raw);
            if (rc != OBMAFS3_OK)
                return -EIO;
        } else if (rc != OBMAFS3_OK) {
            return -EIO;
        }
    }

    /* Check if suffix is generatable (only for modes with ECC/EDC) */
    bool sfx_gen = false;
    switch ((enum obmafs3_cd_sector_mode)mode) {
    case kCdSectorMode1:
        sfx_gen = ecc_cd_is_suffix_correct(ffctx->ecc_ctx, raw);
        break;
    case kCdSectorMode2Form1:
    case kCdSectorMode2Form2:
        sfx_gen = ecc_cd_is_suffix_correct_mode2(ffctx->ecc_ctx, raw);
        break;
    case kCdSectorMode2:
        /* Raw Mode 2 has no ECC/EDC suffix — the entire remaining
         * 2336 bytes is data.  Suffix is trivially "generatable" (empty). */
        sfx_gen = true;
        break;
    default:
        break;
    }

    sme.generated_suffix = sfx_gen ? 1 : 0;

    if (!sfx_gen) {
        /* Store the non-generatable suffix (last 288 bytes of raw sector) */
        const uint8_t *suffix = raw + CD_RAW_SECTOR_SIZE - CD_SUFFIX_SIZE;
        uint64_t sfx_hash = obmafs3_checksum_xxh64(suffix, CD_SUFFIX_SIZE);
        sme.suffix_hash = sfx_hash;

        uint8_t existing[CD_SUFFIX_DATA_SIZE];
        int rc = obmafs3_cd_suffix_get(g_ctx, sfx_hash, existing);
        if (rc == OBMAFS3_ERR_NOTFOUND) {
            rc = obmafs3_cd_suffix_put(g_ctx, sfx_hash, suffix);
            if (rc != OBMAFS3_OK)
                return -EIO;
        } else if (rc != OBMAFS3_OK) {
            return -EIO;
        }
    }

    /* Store subheader for Mode 2 variants (bytes 16-23) */
    if (mode == kCdSectorMode2 ||
        mode == kCdSectorMode2Form1 ||
        mode == kCdSectorMode2Form2) {
        memcpy(sme.subheader, raw + CD_PREFIX_SIZE, 8);
    }

    /* Determine data portion and its size */
    const uint8_t *data_ptr;
    uint16_t data_size;
    switch ((enum obmafs3_cd_sector_mode)mode) {
    case kCdSectorMode1:
        /* prefix(16) + data(2048) + suffix(288) */
        data_ptr  = raw + CD_PREFIX_SIZE;
        data_size = CD_DATA_SIZE;
        break;
    case kCdSectorMode2:
        /* prefix(16) + data(2336) — no suffix */
        data_ptr  = raw + CD_PREFIX_SIZE;
        data_size = 2336;
        break;
    case kCdSectorMode2Form1:
        /* prefix(16) + subheader(8) + data(2048) + EDC(4) + ECC(276)
         * The subheader is stored separately; data is the 2048 user bytes */
        data_ptr  = raw + CD_PREFIX_SIZE + 8;
        data_size = CD_DATA_SIZE;
        break;
    case kCdSectorMode2Form2:
        /* prefix(16) + subheader(8) + data(2328) — no ECC, optional EDC
         * For Form 2, the 2328 bytes after subheader are user data */
        data_ptr  = raw + CD_PREFIX_SIZE + 8;
        data_size = 2328;
        break;
    default:
        return -EINVAL;
    }

    sme.sector_size = data_size;

    /* Hash and dedup the data portion */
    uint64_t hash = obmafs3_checksum_xxh64(data_ptr, data_size);
    sme.hash = hash;

    {
        struct btree_header dedup_hdr;
        uint64_t dedup_hdr_lba;
        int rc = obmafs3_dedup_get_tree(g_ctx, data_size,
                                        &dedup_hdr, &dedup_hdr_lba);
        if (rc != OBMAFS3_OK)
            return -EIO;

        struct dedup_entry existing;
        rc = obmafs3_dedup_lookup(g_ctx, &dedup_hdr, hash, &existing);
        if (rc == OBMAFS3_ERR_NOTFOUND) {
            /* New data — store via media image write path */
            if (!ffctx->db_cache.initialized)
                ffctx->sector_size = data_size;

            struct dedup_block_cache *dbc = &ffctx->db_cache;
            if (!dbc->bg_compress) {
                int brc = obmafs3_bg_compress_start(g_ctx, dbc);
                if (brc != OBMAFS3_OK)
                    return -EIO;
            }

            uint64_t offset = (uint64_t)sector_lba * data_size;
            struct sector_map_cache dummy_cache;
            memset(&dummy_cache, 0, sizeof(dummy_cache));
            rc = obmafs3_write_media_image_data(g_ctx, &ffctx->inode,
                                                offset, data_ptr,
                                                data_size, data_size,
                                                &dummy_cache, dbc);
            obmafs3_free_sector_map_cache(&dummy_cache);
            if (rc != OBMAFS3_OK)
                return -EIO;
        } else if (rc != OBMAFS3_OK) {
            return -EIO;
        }
    }

cache_and_done:
    /* Append cd_sector_map_entry to the cache */
    {
        struct cd_sector_map_cache *cache = &ffctx->cd_sme_cache;
        if (cache->count >= cache->capacity) {
            uint64_t new_cap = cache->capacity;
            if (new_cap == 0)
                new_cap = 1024;
            else
                new_cap *= 2;
            struct cd_sector_map_entry *tmp =
                realloc(cache->entries,
                        (size_t)(new_cap * sizeof(*tmp)));
            if (!tmp)
                return -ENOMEM;
            cache->entries  = tmp;
            cache->capacity = new_cap;
        }
        cache->entries[cache->count++] = sme;
    }

    /* Update inode sector count and advance the sector LBA */
    ffctx->cd_next_sector++;
    if ((uint64_t)ffctx->cd_next_sector > ffctx->inode.sector_count)
        ffctx->inode.sector_count = (uint64_t)ffctx->cd_next_sector;
    ffctx->inode_dirty = 1;

    return 0;
}

/**
 * Read a single CD sector by LBA, reconstructing the full 2352-byte
 * raw sector.  For audio sectors the dedup data is the full 2352 bytes.
 * For data sectors: prefix is generated or fetched from the CD prefix
 * tree, data is read from dedup, suffix is generated or fetched from
 * the CD suffix tree, and the subheader is restored as applicable.
 */
static int obmafs3_cd_read_long(struct fuse_file_ctx *ffctx,
                                struct obmafs3_ioctl_cd_read_arg *arg)
{
    if (!arg)
        return -EINVAL;

    int64_t sector_lba = arg->sector;
    if (sector_lba < 0 || (uint64_t)sector_lba >= ffctx->inode.sector_count)
        return -EINVAL;

    /* Read the cd_sector_map_entry for this sector from inode data */
    struct inode_record map_inode;
    memcpy(&map_inode, &ffctx->inode, sizeof(map_inode));
    map_inode.file_size = ffctx->inode.sector_map_size *
                          sizeof(struct cd_sector_map_entry);

    struct cd_sector_map_entry sme;
    uint64_t sme_offset = (uint64_t)sector_lba *
                          sizeof(struct cd_sector_map_entry);
    int rc = obmafs3_read_file_data(g_ctx, &map_inode, sme_offset,
                                    &sme, sizeof(sme));
    if (rc != OBMAFS3_OK)
        return -EIO;

    uint8_t *out = arg->buffer;
    memset(out, 0, CD_RAW_SECTOR_SIZE);

    /* --- Audio: dedup data IS the full 2352 bytes --- */
    if (sme.sector_mode == kCdSectorModeAudio) {
        /* Look up and read the 2352-byte sector from dedup */
        struct btree_header dedup_hdr;
        uint64_t dedup_hdr_lba;
        rc = obmafs3_dedup_get_tree(g_ctx, CD_RAW_SECTOR_SIZE,
                                    &dedup_hdr, &dedup_hdr_lba);
        if (rc != OBMAFS3_OK)
            return -EIO;

        rc = obmafs3_read_media_image_data(g_ctx, &ffctx->inode,
                                           (uint64_t)sector_lba * CD_RAW_SECTOR_SIZE,
                                           out, CD_RAW_SECTOR_SIZE,
                                           CD_RAW_SECTOR_SIZE);
        return (rc == OBMAFS3_OK) ? 0 : -EIO;
    }

    /* --- Data modes --- */

    /* Determine data portion size */
    uint16_t data_size;
    int has_subheader = 0;
    switch ((enum obmafs3_cd_sector_mode)sme.sector_mode) {
    case kCdSectorMode1:
        data_size = CD_DATA_SIZE;     /* 2048 */
        break;
    case kCdSectorMode2:
        data_size = 2336;
        break;
    case kCdSectorMode2Form1:
        data_size = CD_DATA_SIZE;     /* 2048 */
        has_subheader = 1;
        break;
    case kCdSectorMode2Form2:
        data_size = 2328;
        has_subheader = 1;
        break;
    default:
        return -EINVAL;
    }

    /* 1. Reconstruct prefix (bytes 0-15) */
    if (sme.generated_prefix) {
        /* Generate sync + MSF + mode from LBA */
        ecc_cd_reconstruct_prefix(out, sme.sector_mode, sector_lba);
    } else {
        /* Fetch stored prefix from the CD prefix tree */
        uint8_t pfx[CD_PREFIX_DATA_SIZE];
        rc = obmafs3_cd_prefix_get(g_ctx, sme.prefix_hash, pfx);
        if (rc != OBMAFS3_OK)
            return -EIO;
        memcpy(out, pfx, CD_PREFIX_SIZE);
    }

    /* 2. Restore subheader (bytes 16-23) for Mode 2 variants */
    if (has_subheader) {
        /* subheader[8] contains both the subheader and its copy */
        memcpy(out + CD_PREFIX_SIZE, sme.subheader, 4);
        memcpy(out + CD_PREFIX_SIZE + 4, sme.subheader + 4, 4);
    }

    /* 3. Read data portion from dedup */
    {
        int data_offset_in_sector;
        if (has_subheader)
            data_offset_in_sector = CD_PREFIX_SIZE + 8;  /* after prefix + subheader */
        else if (sme.sector_mode == kCdSectorMode2)
            data_offset_in_sector = CD_PREFIX_SIZE;      /* Mode 2 raw: data starts after prefix */
        else
            data_offset_in_sector = CD_PREFIX_SIZE;      /* Mode 1: data starts after prefix */

        /* Read from media image data path using the data portion's
         * sector size for dedup tree lookup */
        struct inode_record data_inode;
        memcpy(&data_inode, &ffctx->inode, sizeof(data_inode));
        /* The data was stored with offset = sector_lba * data_size */
        rc = obmafs3_read_media_image_data(g_ctx, &data_inode,
                                           (uint64_t)sector_lba * data_size,
                                           out + data_offset_in_sector,
                                           data_size, data_size);
        if (rc != OBMAFS3_OK)
            return -EIO;
    }

    /* 4. Reconstruct suffix (last 288 bytes, position 2064-2351) */
    if (sme.sector_mode == kCdSectorMode2) {
        /* Raw Mode 2 has no suffix — all 2336 bytes after prefix are data */
    } else if (sme.generated_suffix) {
        /* Generate EDC/ECC from the data using ecc_cd facilities */
        if (!ffctx->ecc_ctx) {
            ffctx->ecc_ctx = ecc_cd_init();
            if (!ffctx->ecc_ctx)
                return -ENOMEM;
        }
        ecc_cd_reconstruct(ffctx->ecc_ctx, out, sme.sector_mode);
    } else {
        /* Fetch stored suffix from the CD suffix tree */
        uint8_t sfx[CD_SUFFIX_DATA_SIZE];
        rc = obmafs3_cd_suffix_get(g_ctx, sme.suffix_hash, sfx);
        if (rc != OBMAFS3_OK)
            return -EIO;
        memcpy(out + CD_RAW_SECTOR_SIZE - CD_SUFFIX_SIZE,
               sfx, CD_SUFFIX_SIZE);
    }

    return 0;
}

/**
 * Read a single CD sector by LBA, returning 2352 raw bytes + 96 subchannel.
 * If subchannel is not available, the last 96 bytes are filled with zeros.
 */
static int obmafs3_cd_read_long_sub(struct fuse_file_ctx *ffctx,
                                    struct obmafs3_ioctl_cd_read_full_arg *arg)
{
    if (!arg)
        return -EINVAL;

    /* Reuse the read-long handler for the first 2352 bytes */
    struct obmafs3_ioctl_cd_read_arg rd;
    rd.sector = arg->sector;
    int rc = obmafs3_cd_read_long(ffctx, &rd);
    if (rc != 0)
        return rc;

    memcpy(arg->buffer, rd.buffer, CD_RAW_SECTOR_SIZE);

    /* Read the cd_sector_map_entry to get subchannel_hash */
    struct inode_record map_inode;
    memcpy(&map_inode, &ffctx->inode, sizeof(map_inode));
    map_inode.file_size = ffctx->inode.sector_map_size *
                          sizeof(struct cd_sector_map_entry);

    struct cd_sector_map_entry sme;
    uint64_t sme_offset = (uint64_t)arg->sector *
                          sizeof(struct cd_sector_map_entry);
    rc = obmafs3_read_file_data(g_ctx, &map_inode, sme_offset,
                                &sme, sizeof(sme));
    if (rc != OBMAFS3_OK) {
        memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
        return 0;
    }

    if (sme.subchannel_hash != 0) {
        uint8_t sub[CD_SUBCHANNEL_DATA_SIZE];
        rc = obmafs3_cd_subchannel_get(g_ctx, sme.subchannel_hash, sub);
        if (rc == OBMAFS3_OK) {
            memcpy(arg->buffer + CD_RAW_SECTOR_SIZE, sub, CD_SUBCHANNEL_SIZE);
        } else {
            memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
        }
    } else {
        memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
    }

    return 0;
}

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

    switch (cmd) {

    /* ---- media tag ioctls (media image files only) ---- */

    case OBMAFS3_IOC_SET_MEDIA_TAG: {
        if (ffctx->inode.file_type != kFileTypeMediaImage)
            return -ENOTTY;
        struct obmafs3_ioctl_tag_arg *tag_arg =
            (struct obmafs3_ioctl_tag_arg *)data;
        if (!tag_arg || tag_arg->data_length > OBMAFS3_IOC_MAX_TAG_DATA)
            return -EINVAL;
        int rc = obmafs3_media_tag_put(g_ctx, ffctx->inode_id,
                                        tag_arg->tag_type,
                                        tag_arg->data,
                                        tag_arg->data_length);
        return rc == OBMAFS3_OK ? 0 : -EIO;
    }

    case OBMAFS3_IOC_GET_MEDIA_TAG: {
        if (ffctx->inode.file_type != kFileTypeMediaImage)
            return -ENOTTY;
        struct obmafs3_ioctl_tag_arg *tag_arg =
            (struct obmafs3_ioctl_tag_arg *)data;
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

    /* ---- compact disc image ioctl ---- */

    case OBMAFS3_IOC_SET_CD_IMAGE: {
        /* Only allow conversion of regular empty files */
        if (ffctx->inode.file_type != kFileTypeRegular)
            return -ENOTTY;
        if (ffctx->inode.file_size != 0)
            return -ENOTEMPTY;

        ffctx->inode.file_type       = kFileTypeCompactDiscImage;
        ffctx->inode.sector_count    = 0;
        ffctx->inode.sector_map_size = 0;
        ffctx->cd_next_sector        = 0;

        int rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
        return rc == OBMAFS3_OK ? 0 : -EIO;
    }

    case OBMAFS3_IOC_CD_WRITE_LONG: {
        if (ffctx->inode.file_type != kFileTypeCompactDiscImage)
            return -ENOTTY;
        return obmafs3_cd_write_long(
            ffctx, (const struct obmafs3_ioctl_cd_write_arg *)data);
    }

    case OBMAFS3_IOC_CD_READ_LONG: {
        if (ffctx->inode.file_type != kFileTypeCompactDiscImage)
            return -ENOTTY;
        return obmafs3_cd_read_long(
            ffctx, (struct obmafs3_ioctl_cd_read_arg *)data);
    }

    case OBMAFS3_IOC_CD_READ_LONG_SUB: {
        if (ffctx->inode.file_type != kFileTypeCompactDiscImage)
            return -ENOTTY;
        return obmafs3_cd_read_long_sub(
            ffctx, (struct obmafs3_ioctl_cd_read_full_arg *)data);
    }

    /* ---- Image metadata ioctls ---- */

    case OBMAFS3_IOC_SET_METADATA: {
        if (ffctx->inode.file_type != kFileTypeMediaImage &&
            ffctx->inode.file_type != kFileTypeCompactDiscImage)
            return -ENOTTY;
        const struct obmafs3_ioctl_metadata_set_arg *sa =
            (const struct obmafs3_ioctl_metadata_set_arg *)data;
        int rc = obmafs3_metadata_put(g_ctx, ffctx->inode.inode_id,
                                      sa->key, sa->value);
        return rc == OBMAFS3_OK ? 0 : -EIO;
    }

    case OBMAFS3_IOC_GET_METADATA: {
        if (ffctx->inode.file_type != kFileTypeMediaImage &&
            ffctx->inode.file_type != kFileTypeCompactDiscImage)
            return -ENOTTY;
        struct obmafs3_ioctl_metadata_get_arg *ga =
            (struct obmafs3_ioctl_metadata_get_arg *)data;
        int rc = obmafs3_metadata_get(g_ctx, ffctx->inode.inode_id,
                                      ga->key, ga->value,
                                      METADATA_VALUE_MAX);
        if (rc == OBMAFS3_ERR_NOTFOUND) return -ENODATA;
        return rc == OBMAFS3_OK ? 0 : -EIO;
    }

    case OBMAFS3_IOC_DELETE_METADATA: {
        if (ffctx->inode.file_type != kFileTypeMediaImage &&
            ffctx->inode.file_type != kFileTypeCompactDiscImage)
            return -ENOTTY;
        const struct obmafs3_ioctl_metadata_delete_arg *da =
            (const struct obmafs3_ioctl_metadata_delete_arg *)data;
        int rc = obmafs3_metadata_delete(g_ctx, ffctx->inode.inode_id,
                                         da->key);
        if (rc == OBMAFS3_ERR_NOTFOUND) return -ENODATA;
        return rc == OBMAFS3_OK ? 0 : -EIO;
    }

    case OBMAFS3_IOC_LIST_METADATA: {
        if (ffctx->inode.file_type != kFileTypeMediaImage &&
            ffctx->inode.file_type != kFileTypeCompactDiscImage)
            return -ENOTTY;
        struct obmafs3_ioctl_metadata_list_arg *la =
            (struct obmafs3_ioctl_metadata_list_arg *)data;
        char **keys;
        uint32_t total;
        int rc = obmafs3_metadata_list(g_ctx, ffctx->inode.inode_id,
                                       &keys, &total);
        if (rc != OBMAFS3_OK)
            return -EIO;
        uint32_t start = la->offset;
        uint32_t n = 0;
        memset(la->keys, 0, sizeof(la->keys));
        for (uint32_t i = start; i < total && n < 16; i++, n++)
            strncpy(la->keys[n], keys[i], METADATA_KEY_MAX - 1);
        la->count = n;
        obmafs3_metadata_list_free(keys, total);
        return 0;
    }

    case OBMAFS3_IOC_QUERY_METADATA: {
        /* This query is filesystem-level; works on any open image file */
        struct obmafs3_ioctl_metadata_query_arg *qa =
            (struct obmafs3_ioctl_metadata_query_arg *)data;
        uint64_t *ids;
        uint32_t total;
        int rc = obmafs3_metadata_query(g_ctx, qa->key, qa->value,
                                        &ids, &total);
        if (rc != OBMAFS3_OK)
            return -EIO;
        uint32_t start = qa->offset;
        uint32_t n = 0;
        memset(qa->inode_ids, 0, sizeof(qa->inode_ids));
        for (uint32_t i = start; i < total && n < 64; i++, n++)
            qa->inode_ids[n] = ids[i];
        qa->count = n;
        free(ids);
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
