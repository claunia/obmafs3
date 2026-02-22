// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fuse_read.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : FUSE interface for OBMAFS3
//
// --[ Description ] ----------------------------------------------------------
//
//     FUSE read operations for OBMAFS3.
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
 * Implements: getattr, readdir, open, read
 */

#include "fuse_ops_internal.h"
#include "debug.h"

/**
 * FUSE callback: get file/directory attributes.
 *
 * Fills @p stbuf with the stat information for @p path.  Uses the
 * cached inode from the file handle when available.
 */
static int obmafs3_fuse_getattr_impl(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    struct inode_record  inode;
    struct inode_record *ip;
    int                  rc;

    memset(stbuf, 0, sizeof(*stbuf));

    struct fuse_file_ctx *ffctx = fi ? (struct fuse_file_ctx *)(uintptr_t)fi->fh : NULL;

    if(strcmp(path, "/") == 0)
    {
        rc = obmafs3_inode_get(g_ctx, OBMAFS3_ROOT_INODE_ID, &inode);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

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

    uint64_t    parent_id;
    const char *name;
    rc = resolve_path(path, &parent_id, &name);
    if(rc != 0) return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if(rc == OBMAFS3_ERR_NOTFOUND) return -ENOENT;
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    if(ffctx) { ip = &ffctx->inode; }
    else
    {
        rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
        ip = &inode;
    }

    stbuf->st_ino = ip->inode_id;
    if(cat_entry.directory_flag || ip->file_type == kFileTypeDirectory)
        stbuf->st_mode = S_IFDIR | ip->mode;
    else if(ip->file_type == kFileTypeSymlink)
        stbuf->st_mode = S_IFLNK | 0777;
    else
        stbuf->st_mode = S_IFREG | ip->mode;
    stbuf->st_nlink   = ip->ref_count;
    stbuf->st_uid     = ip->uid;
    stbuf->st_gid     = ip->gid;

    /* For subchannel files, compute file_size from the parent CD image's
     * sector_count since the sidecar stores no data of its own. */
    uint64_t reported_size = ip->file_size;
    if(ip->file_type == kFileTypeSubchannelFile)
    {
        struct inode_record parent_inode;
        if(obmafs3_inode_get(g_ctx, ip->sector_count, &parent_inode) == OBMAFS3_OK)
            reported_size = parent_inode.sector_count * CD_SUBCHANNEL_SIZE;
    }

    stbuf->st_size    = (off_t)reported_size;
    stbuf->st_blksize = (blksize_t)g_ctx->sb.block_size;
    stbuf->st_blocks  = (blkcnt_t)((reported_size + 511) / 512);
    stbuf->st_atime   = (time_t)ip->access_time;
    stbuf->st_mtime   = (time_t)ip->modification_time;
    stbuf->st_ctime   = (time_t)ip->creation_time;
    return 0;
}

/**
 * FUSE callback: list directory contents.
 *
 * Retrieves all catalog entries under the directory identified by
 * @p path and feeds them to the FUSE filler callback.
 */
static int obmafs3_fuse_readdir_impl(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                                     struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    struct catalog_record *entries = NULL;
    uint32_t               count   = 0;
    uint64_t               dir_inode_id;
    uint32_t               i;
    int                    rc;

    (void)offset;
    (void)fi;
    (void)flags;

    if(strcmp(path, "/") == 0) { dir_inode_id = OBMAFS3_ROOT_INODE_ID; }
    else
    {
        uint64_t              parent_id;
        const char           *name;
        struct catalog_record cat_entry;

        rc = resolve_path(path, &parent_id, &name);
        if(rc != 0) return rc;

        rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-ENOENT, "");

        if(!cat_entry.directory_flag) FUSE_RETURN(-ENOTDIR, "");

        dir_inode_id = cat_entry.inode_id;
    }

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    rc = obmafs3_catalog_list(g_ctx, dir_inode_id, &entries, &count);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    for(i = 0; i < count; i++)
    {
        /* Skip the root directory self-entry */
        if(entries[i].inode_id == OBMAFS3_ROOT_INODE_ID && entries[i].parent_id == OBMAFS3_ROOT_INODE_ID) continue;
        filler(buf, entries[i].name, NULL, 0, 0);
    }

    obmafs3_catalog_list_free(entries);
    return 0;
}

/**
 * FUSE callback: open a file.
 *
 * Resolves @p path to an inode, allocates a per-file context
 * (@c fuse_file_ctx), and stores it in the file handle.  Detects
 * media image files and records their sector size.
 */
static int obmafs3_fuse_open_impl(const char *path, struct fuse_file_info *fi)
{
    uint64_t              parent_id;
    const char           *name;
    struct catalog_record cat_entry;
    int                   rc;

    rc = resolve_path(path, &parent_id, &name);
    if(rc != 0) return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENOENT, "");
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    if(cat_entry.directory_flag) FUSE_RETURN(-EISDIR, "");

    struct fuse_file_ctx *fctx = calloc(1, sizeof(*fctx));
    if(!fctx) FUSE_RETURN(-ENOMEM, "");

    fctx->inode_id = cat_entry.inode_id;

    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &fctx->inode);
    if(rc != OBMAFS3_OK)
    {
        free(fctx);
        FUSE_RETURN(-EIO, "");
    }

    if(fctx->inode.file_type == kFileTypeMediaImage) fctx->sector_size = lookup_disk_image_sector_size(name);

    fi->fh = (uint64_t)(uintptr_t)fctx;
    return 0;
}

/**
 * FUSE callback: read file data.
 *
 * Reads up to @p size bytes at @p offset from the file identified by
 * @p path.  Uses the cached inode from the file handle when available.
 * Dispatches to the media image read path for media images.
 */
static int obmafs3_fuse_read_impl(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct fuse_file_ctx *ffctx = fi ? (struct fuse_file_ctx *)(uintptr_t)fi->fh : NULL;
    struct inode_record   inode;
    struct inode_record  *ip;
    int                   rc;

    if(ffctx) { ip = &ffctx->inode; }
    else
    {
        uint64_t              parent_id;
        const char           *name;
        struct catalog_record cat_entry;
        rc = resolve_path(path, &parent_id, &name);
        if(rc != 0) return rc;
        rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
        rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
        ip = &inode;
    }

    if((uint64_t)offset >= ip->file_size) return 0;

    if((uint64_t)offset + size > ip->file_size) size = (size_t)(ip->file_size - (uint64_t)offset);

    if(ip->file_type == kFileTypeMediaImage)
    {
        uint16_t ss = ffctx ? ffctx->sector_size : 0;
        if(ss == 0)
        {
            uint64_t    parent_id;
            const char *name;
            rc = resolve_path(path, &parent_id, &name);
            if(rc != 0) return rc;
            ss = lookup_disk_image_sector_size(name);
        }
        if(ss == 0) FUSE_RETURN(-EINVAL, "");
        rc = obmafs3_read_media_image_data(g_ctx, ip, (uint64_t)offset, buf, size, ss);
    }
    else if(ip->file_type == kFileTypeCompactDiscImage)
    {
        rc = obmafs3_read_cd_image_data(g_ctx, ip, (uint64_t)offset, buf, size);
    }
    else if(ip->file_type == kFileTypeSubchannelFile)
    {
        rc = obmafs3_read_subchannel_data(g_ctx, ip, (uint64_t)offset, buf, size);
    }
    else
    {
        rc = obmafs3_read_file_data(g_ctx, ip, (uint64_t)offset, buf, size);
    }

    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "[fuse_read] RETURNING -EIO rc=%d path=%s offset=%ld size=%zu file_type=%u\n",
                rc, path ? path : "(null)", (long)offset, size, ip->file_type);
        FUSE_RETURN(-EIO, "");
    }

    return (int)size;
}

/* ------------------------------------------------------------------ */
/*  Thread-safe wrappers — take shared (read) lock on tree_lock        */
/* ------------------------------------------------------------------ */

int obmafs3_fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    pthread_rwlock_rdlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_getattr_impl(path, stbuf, fi);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}

int obmafs3_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags)
{
    pthread_rwlock_rdlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_readdir_impl(path, buf, filler, offset, fi, flags);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}

int obmafs3_fuse_open(const char *path, struct fuse_file_info *fi)
{
    pthread_rwlock_rdlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_open_impl(path, fi);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}

int obmafs3_fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    pthread_rwlock_rdlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_read_impl(path, buf, size, offset, fi);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}