/*
 * fuse_write.c - OBMAFS3 FUSE write operations
 *
 * Implements: create, write, truncate, link, symlink, readlink, unlink, rename
 */

#include "fuse_ops_internal.h"
#include <linux/fs.h>   /* RENAME_NOREPLACE, RENAME_EXCHANGE */

/**
 * FUSE callback: create a new file.
 *
 * Allocates a new inode, inserts a catalog entry, and opens the file.
 * Detects media image files by extension and sets the file type
 * accordingly.
 */
int obmafs3_fuse_create(const char *path, mode_t mode,
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

/**
 * FUSE callback: write data to a file.
 *
 * Writes @p size bytes from @p buf at @p offset.  For media image
 * files the write is dispatched through the dedup-aware media image
 * write path with background compression.
 */
int obmafs3_fuse_write(const char *path, const char *buf,
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

/**
 * FUSE callback: change file size.
 *
 * Extends the file with zero-filled data or shrinks it by freeing
 * trailing extent blocks.
 */
int obmafs3_fuse_truncate(const char *path, off_t newsize,
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
            rc = obmafs3_truncate_file_blocks(g_ctx, ip,
                                              new_blocks_needed);
            if (rc != OBMAFS3_OK)
                return -EIO;
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

/**
 * FUSE callback: create a hard link.
 *
 * Creates a new catalog entry pointing to the same inode as @p oldpath
 * and increments the reference count.
 */
int obmafs3_fuse_link(const char *oldpath, const char *newpath)
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

/**
 * FUSE callback: create a symbolic link.
 *
 * Allocates a new inode of type @c kFileTypeSymlink, stores the
 * symlink target as file data, and inserts the catalog entry.
 */
int obmafs3_fuse_symlink(const char *target, const char *linkpath)
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

/**
 * FUSE callback: read the target of a symbolic link.
 *
 * Reads the symlink target from the inode's file data into @p buf.
 */
int obmafs3_fuse_readlink(const char *path, char *buf, size_t size)
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

/**
 * FUSE callback: remove a file.
 *
 * Removes the catalog entry, decrements the inode reference count, and
 * deletes the inode (along with its media tags) when the last reference
 * is removed.
 */
int obmafs3_fuse_unlink(const char *path)
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
        /* Last reference — free data blocks, media tags, and inode */
        obmafs3_free_file_blocks(g_ctx, &inode);

        if (inode.file_type == kFileTypeMediaImage)
            obmafs3_media_tag_delete_all(g_ctx, cat_entry.inode_id);

        rc = obmafs3_inode_delete(g_ctx, cat_entry.inode_id);
        if (rc != OBMAFS3_OK)
            return -EIO;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helper: unlink a destination file/directory to make room for       */
/*          a rename that replaces an existing target.                 */
/* ------------------------------------------------------------------ */
static int replace_dest(const struct catalog_record *dst,
                        uint64_t dst_parent, const char *dst_name)
{
    int rc;

    if (dst->directory_flag) {
        /* Destination directory must be empty */
        struct catalog_record *children = NULL;
        uint32_t child_count = 0;
        rc = obmafs3_catalog_list(g_ctx, dst->inode_id,
                                  &children, &child_count);
        if (rc != OBMAFS3_OK)
            return -EIO;
        obmafs3_catalog_list_free(children);
        if (child_count > 0)
            return -ENOTEMPTY;

        rc = obmafs3_catalog_delete(g_ctx, dst_parent, dst_name);
        if (rc != OBMAFS3_OK)
            return -EIO;

        rc = obmafs3_inode_delete(g_ctx, dst->inode_id);
        if (rc != OBMAFS3_OK)
            return -EIO;
    } else {
        rc = obmafs3_catalog_delete(g_ctx, dst_parent, dst_name);
        if (rc != OBMAFS3_OK)
            return -EIO;

        struct inode_record inode;
        rc = obmafs3_inode_get(g_ctx, dst->inode_id, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;

        if (inode.ref_count > 1) {
            inode.ref_count--;
            rc = obmafs3_inode_put(g_ctx, &inode);
            if (rc != OBMAFS3_OK)
                return -EIO;
        } else {
            obmafs3_free_file_blocks(g_ctx, &inode);
            if (inode.file_type == kFileTypeMediaImage)
                obmafs3_media_tag_delete_all(g_ctx, dst->inode_id);
            rc = obmafs3_inode_delete(g_ctx, dst->inode_id);
            if (rc != OBMAFS3_OK)
                return -EIO;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helper: check if @ancestor_id is an ancestor of @dir_id           */
/*          in the catalog tree (to prevent moving a directory into    */
/*          itself).  Returns 1 if ancestor, 0 if not, <0 on error.   */
/* ------------------------------------------------------------------ */

/**
 * Find the parent inode ID of directory @p child_id by scanning the
 * catalog tree.  Returns OBMAFS3_OK and sets *parent_out, or
 * OBMAFS3_ERR_NOTFOUND if @p child_id is the root or not found.
 */
static int find_parent_of_dir(uint64_t child_id, uint64_t *parent_out)
{
    /*
     * BFS from root.  For each directory, list its children and check
     * if any child's inode_id == child_id.  Depth is typically small.
     */
    uint64_t queue[256];
    int head = 0, tail = 0;
    queue[tail++] = OBMAFS3_ROOT_INODE_ID;

    while (head < tail) {
        uint64_t pid = queue[head++];
        struct catalog_record *children = NULL;
        uint32_t count = 0;
        int rc = obmafs3_catalog_list(g_ctx, pid, &children, &count);
        if (rc != OBMAFS3_OK)
            return rc;

        for (uint32_t i = 0; i < count; i++) {
            if (!children[i].directory_flag)
                continue;
            if (children[i].inode_id == child_id) {
                *parent_out = pid;
                obmafs3_catalog_list_free(children);
                return OBMAFS3_OK;
            }
            if (tail < 256)
                queue[tail++] = children[i].inode_id;
        }
        obmafs3_catalog_list_free(children);
    }
    return OBMAFS3_ERR_NOTFOUND;
}

static int is_ancestor(uint64_t ancestor_id, uint64_t dir_id)
{
    uint64_t current = dir_id;
    for (int depth = 0; depth < 256; depth++) {
        if (current == ancestor_id)
            return 1;
        if (current == OBMAFS3_ROOT_INODE_ID)
            return 0;

        uint64_t parent;
        int rc = find_parent_of_dir(current, &parent);
        if (rc == OBMAFS3_ERR_NOTFOUND)
            return 0;
        if (rc != OBMAFS3_OK)
            return -EIO;
        current = parent;
    }
    return 0;
}

/**
 * FUSE callback: rename or move a file or directory.
 *
 * Supports three modes via @p flags:
 *   - 0               : replace destination if it exists
 *   - RENAME_NOREPLACE : fail with -EEXIST if destination exists
 *   - RENAME_EXCHANGE  : atomically swap the two entries
 *
 * Cross-directory renames of a directory into its own subtree are
 * rejected with -EINVAL.
 */
int obmafs3_fuse_rename(const char *oldpath, const char *newpath,
                        unsigned int flags)
{
    uint64_t old_parent, new_parent;
    const char *old_name, *new_name;
    struct catalog_record src, dst;
    int rc;

    /* Reject unsupported flags */
    if (flags & ~((unsigned int)RENAME_NOREPLACE | (unsigned int)RENAME_EXCHANGE))
        return -EINVAL;

    /* Resolve source */
    rc = resolve_path(oldpath, &old_parent, &old_name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, old_parent, old_name, &src);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Resolve destination */
    rc = resolve_path(newpath, &new_parent, &new_name);
    if (rc != 0)
        return rc;

    /* Check if destination already exists */
    int dst_exists = 0;
    rc = obmafs3_catalog_lookup(g_ctx, new_parent, new_name, &dst);
    if (rc == OBMAFS3_OK)
        dst_exists = 1;
    else if (rc != OBMAFS3_ERR_NOTFOUND)
        return -EIO;

    /* ------ RENAME_EXCHANGE ------ */
    if (flags & RENAME_EXCHANGE) {
        if (!dst_exists)
            return -ENOENT;

        /* Cannot exchange a directory with a non-directory */
        if (src.directory_flag != dst.directory_flag)
            return -ENOTDIR;

        /* Remove both old entries */
        rc = obmafs3_catalog_delete(g_ctx, old_parent, old_name);
        if (rc != OBMAFS3_OK)
            return -EIO;
        rc = obmafs3_catalog_delete(g_ctx, new_parent, new_name);
        if (rc != OBMAFS3_OK)
            return -EIO;

        /* Insert swapped entries */
        struct catalog_record new_src;
        memset(&new_src, 0, sizeof(new_src));
        new_src.inode_id       = dst.inode_id;
        new_src.parent_id      = old_parent;
        new_src.directory_flag = dst.directory_flag;
        strncpy(new_src.name, old_name, sizeof(new_src.name) - 1);

        struct catalog_record new_dst;
        memset(&new_dst, 0, sizeof(new_dst));
        new_dst.inode_id       = src.inode_id;
        new_dst.parent_id      = new_parent;
        new_dst.directory_flag = src.directory_flag;
        strncpy(new_dst.name, new_name, sizeof(new_dst.name) - 1);

        rc = obmafs3_catalog_insert(g_ctx, &new_src);
        if (rc != OBMAFS3_OK)
            return -EIO;
        rc = obmafs3_catalog_insert(g_ctx, &new_dst);
        if (rc != OBMAFS3_OK)
            return -EIO;

        return 0;
    }

    /* ------ RENAME_NOREPLACE ------ */
    if ((flags & RENAME_NOREPLACE) && dst_exists)
        return -EEXIST;

    /* ------ Regular rename (flags == 0) ------ */

    /* Cannot rename a file over a directory or vice versa */
    if (dst_exists) {
        if (src.directory_flag && !dst.directory_flag)
            return -ENOTDIR;
        if (!src.directory_flag && dst.directory_flag)
            return -EISDIR;
    }

    /* Prevent moving a directory into its own subtree */
    if (src.directory_flag && old_parent != new_parent) {
        int anc = is_ancestor(src.inode_id, new_parent);
        if (anc < 0)
            return anc;  /* I/O error */
        if (anc)
            return -EINVAL;
    }

    /* If destination exists, remove it first */
    if (dst_exists) {
        rc = replace_dest(&dst, new_parent, new_name);
        if (rc != 0)
            return rc;
    }

    /* Remove old catalog entry */
    rc = obmafs3_catalog_delete(g_ctx, old_parent, old_name);
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Insert new catalog entry with the same inode_id */
    struct catalog_record new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
    new_cat.inode_id       = src.inode_id;
    new_cat.parent_id      = new_parent;
    new_cat.directory_flag = src.directory_flag;
    strncpy(new_cat.name, new_name, sizeof(new_cat.name) - 1);

    rc = obmafs3_catalog_insert(g_ctx, &new_cat);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}
