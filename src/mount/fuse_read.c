/*
 * fuse_read.c - OBMAFS3 FUSE read callbacks
 *
 * Implements: getattr, readdir, open, read
 */

#include "fuse_ops_internal.h"

int obmafs3_fuse_getattr(const char *path, struct stat *stbuf,
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

int obmafs3_fuse_readdir(const char *path, void *buf,
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

int obmafs3_fuse_open(const char *path, struct fuse_file_info *fi)
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

int obmafs3_fuse_read(const char *path, char *buf, size_t size,
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
