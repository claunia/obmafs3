/*
 * fuse_attr.c - OBMAFS3 FUSE attribute and lifecycle operations
 *
 * Implements: utimens, chmod, chown, flush, release, statfs, statx
 */

#include "fuse_ops_internal.h"

/**
 * FUSE callback: set file access and modification times.
 *
 * Updates the access and modification timestamps stored in the
 * inode to the values specified in @p ts.
 */
int obmafs3_fuse_utimens(const char *path,
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

/**
 * FUSE callback: change file permission bits.
 *
 * Updates the inode's permission mode to @p mode (masked to the
 * lower 12 bits).
 */
int obmafs3_fuse_chmod(const char *path, mode_t mode,
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

/**
 * FUSE callback: change file owner and group.
 *
 * Updates the inode's UID and/or GID.  A value of @c (uid_t)-1 or
 * @c (gid_t)-1 leaves the corresponding field unchanged.
 */
int obmafs3_fuse_chown(const char *path, uid_t uid, gid_t gid,
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
int obmafs3_fuse_flush(const char *path, struct fuse_file_info *fi)
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

/**
 * FUSE callback: release an open file.
 *
 * Called when the last file descriptor referring to an open file is
 * closed.  Flushes all caches (dedup block, sector map, CD sector
 * map), writes the inode back if dirty, and frees the per-file
 * context.
 */
int obmafs3_fuse_release(const char *path, struct fuse_file_info *fi)
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

/**
 * FUSE callback: get filesystem statistics.
 *
 * Populates @p stbuf with block size, total/free block counts, and
 * the number of allocated inodes.
 */
int obmafs3_fuse_statfs(const char *path, struct statvfs *stbuf)
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

/**
 * FUSE callback: get extended file attributes (statx).
 *
 * Returns extended attribute information including birth time and the
 * @c STATX_ATTR_COMPRESSED flag for media image files stored with
 * compression.
 */
int obmafs3_fuse_statx(const char *path, int flags, int mask,
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
