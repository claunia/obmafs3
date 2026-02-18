/*
 * fuse_dir.c - OBMAFS3 FUSE directory operations
 *
 * Implements: mkdir, rmdir
 */

#include "fuse_ops_internal.h"

/**
 * FUSE callback: create a directory.
 *
 * Allocates a new inode of type @c kFileTypeDirectory, inserts a
 * catalog entry, and stores the new directory inode.
 */
int obmafs3_fuse_mkdir(const char *path, mode_t mode)
{
    uint64_t              parent_id;
    const char           *name;
    struct catalog_record cat_entry;
    int                   rc;

    rc = resolve_path(path, &parent_id, &name);
    if(rc != 0) return rc;

    /* Check if the name already exists */
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if(rc == OBMAFS3_OK) return -EEXIST;
    if(rc != OBMAFS3_ERR_NOTFOUND) return -EIO;

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
    if(rc != OBMAFS3_OK) return -EIO;

    /* Create the inode */
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
    if(rc != OBMAFS3_OK) return -EIO;

    return 0;
}

/**
 * FUSE callback: remove a directory.
 *
 * Verifies that the directory is empty, then removes the catalog entry
 * and deletes the inode.
 */
int obmafs3_fuse_rmdir(const char *path)
{
    uint64_t              parent_id;
    const char           *name;
    struct catalog_record cat_entry;
    int                   rc;

    rc = resolve_path(path, &parent_id, &name);
    if(rc != 0) return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if(rc == OBMAFS3_ERR_NOTFOUND) return -ENOENT;
    if(rc != OBMAFS3_OK) return -EIO;

    if(!cat_entry.directory_flag) return -ENOTDIR;

    /* Check that the directory is empty */
    struct catalog_record *children    = NULL;
    uint32_t               child_count = 0;
    rc                                 = obmafs3_catalog_list(g_ctx, cat_entry.inode_id, &children, &child_count);
    if(rc != OBMAFS3_OK) return -EIO;
    obmafs3_catalog_list_free(children);

    if(child_count > 0) return -ENOTEMPTY;

    /* Remove the catalog entry */
    rc = obmafs3_catalog_delete(g_ctx, parent_id, name);
    if(rc != OBMAFS3_OK) return -EIO;

    /* Remove the inode */
    rc = obmafs3_inode_delete(g_ctx, cat_entry.inode_id);
    if(rc != OBMAFS3_OK) return -EIO;

    return 0;
}
