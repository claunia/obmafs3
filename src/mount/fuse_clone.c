/*
 * fuse_clone.c - OBMAFS3 FUSE copy_file_range (clone/reflink) support
 *
 * Implements the FUSE3 copy_file_range callback, which the kernel
 * invokes for the copy_file_range(2) syscall.  Tools such as
 * `cp --reflink` fall back to copy_file_range when FICLONERANGE is
 * not directly supported (as is the case for FUSE filesystems).
 *
 * Shared blocks are reference-counted; writes to shared blocks trigger
 * copy-on-write in the write path (see block.c).
 */

#include "fuse_ops_internal.h"

/**
 * FUSE callback: copy a range of data from one file to another by
 * sharing physical blocks and incrementing their refcounts.
 *
 * Both offsets and the length must be aligned to the filesystem's
 * per-block data capacity.  Source data may reside in inline extents
 * or the overflow B+Tree.  Only regular files are supported.
 *
 * @param path_in    Source file path.
 * @param fi_in      Source file handle.
 * @param offset_in  Byte offset in the source file.
 * @param path_out   Destination file path.
 * @param fi_out     Destination file handle.
 * @param offset_out Byte offset in the destination file.
 * @param size       Number of bytes to clone.
 * @param flags      Reserved (must be 0).
 * @return Number of bytes cloned on success, or a negative errno.
 */
ssize_t obmafs3_fuse_copy_file_range(const char *path_in,
                                     struct fuse_file_info *fi_in,
                                     off_t offset_in,
                                     const char *path_out,
                                     struct fuse_file_info *fi_out,
                                     off_t offset_out,
                                     size_t size,
                                     int flags)
{
    (void)path_in;
    (void)path_out;

    if (flags != 0)
        return -EINVAL;

    if (!fi_in || !fi_out)
        return -EBADF;

    struct fuse_file_ctx *src_ctx =
        (struct fuse_file_ctx *)(uintptr_t)fi_in->fh;
    struct fuse_file_ctx *dst_ctx =
        (struct fuse_file_ctx *)(uintptr_t)fi_out->fh;

    if (!src_ctx || !dst_ctx)
        return -EBADF;

    /* Only regular files may be cloned */
    if (src_ctx->inode.file_type != kFileTypeRegular ||
        dst_ctx->inode.file_type != kFileTypeRegular)
        return -EINVAL;

    /* Clamp size to source file bounds */
    if ((uint64_t)offset_in >= src_ctx->inode.file_size)
        return 0;
    if ((uint64_t)offset_in + size > src_ctx->inode.file_size)
        size = (size_t)(src_ctx->inode.file_size - (uint64_t)offset_in);
    if (size == 0)
        return 0;

    /* Alignment check (data_capacity) */
    size_t data_cap = (size_t)(g_ctx->sb.block_size -
                               sizeof(struct block_header));
    if ((uint64_t)offset_in % data_cap != 0 ||
        (uint64_t)offset_out % data_cap != 0 ||
        size % data_cap != 0)
        return -EINVAL;

    int rc = obmafs3_clone_file_range(g_ctx,
                                      &src_ctx->inode,
                                      (uint64_t)offset_in,
                                      &dst_ctx->inode,
                                      (uint64_t)offset_out,
                                      (uint64_t)size);
    if (rc == OBMAFS3_ERR_NOSPC)
        return -ENOSPC;
    if (rc == OBMAFS3_ERR_INVAL)
        return -EINVAL;
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Persist both inodes */
    rc = obmafs3_inode_put(g_ctx, &src_ctx->inode);
    if (rc != OBMAFS3_OK)
        return -EIO;
    src_ctx->inode_dirty = 0;

    rc = obmafs3_inode_put(g_ctx, &dst_ctx->inode);
    if (rc != OBMAFS3_OK)
        return -EIO;
    dst_ctx->inode_dirty = 0;

    return (ssize_t)size;
}
