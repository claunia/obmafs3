/*
 * fuse_ops_internal.h - shared internal definitions for FUSE operation
 *                       implementation files
 */
#ifndef OBMAFS3_FUSE_OPS_INTERNAL_H
#define OBMAFS3_FUSE_OPS_INTERNAL_H

#include "fuse_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

/**
 * Per-file-handle context.  For media image files, this caches
 * sector_map_entries in RAM across multiple FUSE write calls and
 * flushes them to disk when the file is closed (release).
 */
struct fuse_file_ctx
{
    uint64_t                   inode_id;
    uint16_t                   sector_size;  ///< 0 for non-media-image files
    struct inode_record        inode;        ///< cached inode
    int                        inode_dirty;  ///< needs write-back on release
    struct sector_map_cache    sme_cache;
    struct dedup_block_cache   db_cache;        ///< persistent dedup block accumulator
    struct cd_sector_map_cache cd_sme_cache;    ///< CD sector map cache
    void                      *ecc_ctx;         ///< CD ECC context (lazy-init)
    int64_t                    cd_next_sector;  ///< next expected CD sector LBA
};

/* ---- helpers defined in fuse_ops.c ---- */

int      resolve_path(const char *path, uint64_t *parent_id, const char **name);
uint16_t lookup_disk_image_sector_size(const char *name);

/* ---- FUSE callback declarations ---- */

int obmafs3_fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int obmafs3_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags);
int obmafs3_fuse_open(const char *path, struct fuse_file_info *fi);
int obmafs3_fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);

int obmafs3_fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int obmafs3_fuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int obmafs3_fuse_truncate(const char *path, off_t newsize, struct fuse_file_info *fi);
int obmafs3_fuse_link(const char *oldpath, const char *newpath);
int obmafs3_fuse_symlink(const char *target, const char *linkpath);
int obmafs3_fuse_readlink(const char *path, char *buf, size_t size);
int obmafs3_fuse_unlink(const char *path);
int obmafs3_fuse_rename(const char *oldpath, const char *newpath, unsigned int flags);

int obmafs3_fuse_mkdir(const char *path, mode_t mode);
int obmafs3_fuse_rmdir(const char *path);

int obmafs3_fuse_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi);
int obmafs3_fuse_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int obmafs3_fuse_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi);

int obmafs3_fuse_flush(const char *path, struct fuse_file_info *fi);
int obmafs3_fuse_release(const char *path, struct fuse_file_info *fi);

int obmafs3_fuse_statfs(const char *path, struct statvfs *stbuf);
int obmafs3_fuse_statx(const char *path, int flags, int mask, struct statx *stxbuf, struct fuse_file_info *fi);

int obmafs3_fuse_getxattr(const char *path, const char *name, char *value, size_t size);
int obmafs3_fuse_setxattr(const char *path, const char *name, const char *value, size_t size, int flags);
int obmafs3_fuse_listxattr(const char *path, char *list, size_t size);
int obmafs3_fuse_removexattr(const char *path, const char *name);

int obmafs3_fuse_ioctl(const char *path, unsigned int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags,
                       void *data);

ssize_t obmafs3_fuse_copy_file_range(const char *path_in, struct fuse_file_info *fi_in, off_t offset_in,
                                     const char *path_out, struct fuse_file_info *fi_out, off_t offset_out, size_t size,
                                     int flags);

#endif /* OBMAFS3_FUSE_OPS_INTERNAL_H */
