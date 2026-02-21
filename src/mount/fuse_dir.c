/*
 * fuse_dir.c - OBMAFS3 FUSE directory operations
 *
 * Implements: mkdir, rmdir
 */

#include "fuse_ops_internal.h"
#include "debug.h"

/**
 * FUSE callback: create a directory.
 *
 * Allocates a new inode of type @c kFileTypeDirectory, inserts a
 * catalog entry, and stores the new directory inode.
 */
static int obmafs3_fuse_mkdir_impl(const char *path, mode_t mode)
{
    uint64_t              parent_id;
    const char           *name;
    struct catalog_record cat_entry;
    int                   rc;

    rc = resolve_path(path, &parent_id, &name);
    if(rc != 0) return rc;

    /* Check if the name already exists */
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if(rc == OBMAFS3_OK) FUSE_RETURN(-EEXIST, "");
    if(rc != OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-EIO, "");

    /* Allocate a new inode ID */
    uint64_t new_inode_id = obmafs3_alloc_inode_id(g_ctx);

    /* Create the inode FIRST — an orphan inode (no catalog ref) is
     * harmless and can be cleaned up by fsck, whereas a dangling
     * catalog entry (no inode) causes -EIO on every access. */
    uint64_t             now  = (uint64_t)time(NULL);
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
    new_inode.ref_count         = 1;

    rc = obmafs3_inode_put(g_ctx, &new_inode);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    /* Now create the catalog entry */
    struct catalog_record new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
    new_cat.inode_id       = new_inode_id;
    new_cat.parent_id      = parent_id;
    new_cat.directory_flag = 1;
    strncpy(new_cat.name, name, sizeof(new_cat.name) - 1);

    rc = obmafs3_catalog_insert(g_ctx, &new_cat);
    if(rc != OBMAFS3_OK)
    {
        /* Roll back the inode to avoid an orphan */
        obmafs3_inode_delete(g_ctx, new_inode_id);
        FUSE_RETURN(-EIO, "");
    }

    return 0;
}

/**
 * FUSE callback: remove a directory.
 *
 * Verifies that the directory is empty, then removes the catalog entry
 * and deletes the inode.
 */
static int obmafs3_fuse_rmdir_impl(const char *path)
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

    if(!cat_entry.directory_flag) FUSE_RETURN(-ENOTDIR, "");

    /* Check that the directory is empty */
    struct catalog_record *children    = NULL;
    uint32_t               child_count = 0;
    rc                                 = obmafs3_catalog_list(g_ctx, cat_entry.inode_id, &children, &child_count);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
    obmafs3_catalog_list_free(children);

    if(child_count > 0) FUSE_RETURN(-ENOTEMPTY, "");

    /* Remove the catalog entry */
    rc = obmafs3_catalog_delete(g_ctx, parent_id, name);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    /* Remove the inode */
    rc = obmafs3_inode_delete(g_ctx, cat_entry.inode_id);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Thread-safe wrappers — serialise write-side callbacks              */
/* ------------------------------------------------------------------ */

int obmafs3_fuse_mkdir(const char *path, mode_t mode)
{
    pthread_rwlock_wrlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_mkdir_impl(path, mode);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}

int obmafs3_fuse_rmdir(const char *path)
{
    pthread_rwlock_wrlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_rmdir_impl(path);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}
