/*
 * fuse_ops.c - OBMAFS3 FUSE filesystem operations
 *
 * Currently implements read-only access:
 *   getattr, readdir, open, read
 */
#define FUSE_USE_VERSION 31

#include "fuse_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

struct obmafs3_ctx *g_ctx = NULL;

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
        struct btree_node_filename entry;
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
    struct btree_node_inode inode;
    int rc;

    (void)fi;
    memset(stbuf, 0, sizeof(*stbuf));

    if (strcmp(path, "/") == 0) {
        rc = obmafs3_inode_get(g_ctx, OBMAFS3_ROOT_INODE_ID, &inode);
        if (rc != OBMAFS3_OK)
            return -EIO;

        stbuf->st_ino   = inode.inode_id;
        stbuf->st_mode  = S_IFDIR | inode.mode;
        stbuf->st_nlink = 2;
        stbuf->st_uid   = inode.uid;
        stbuf->st_gid   = inode.gid;
        stbuf->st_size  = (off_t)inode.file_size;
        stbuf->st_atime = (time_t)inode.access_time;
        stbuf->st_mtime = (time_t)inode.modification_time;
        stbuf->st_ctime = (time_t)inode.creation_time;
        return 0;
    }

    uint64_t parent_id;
    const char *name;
    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    struct btree_node_filename cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc == OBMAFS3_ERR_NOTFOUND)
        return -ENOENT;
    if (rc != OBMAFS3_OK)
        return -EIO;

    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    stbuf->st_ino = inode.inode_id;
    if (cat_entry.directory_flag || inode.file_type == kFileTypeDirectory)
        stbuf->st_mode = S_IFDIR | inode.mode;
    else
        stbuf->st_mode = S_IFREG | inode.mode;
    stbuf->st_nlink = cat_entry.directory_flag ? 2 : 1;
    stbuf->st_uid   = inode.uid;
    stbuf->st_gid   = inode.gid;
    stbuf->st_size  = (off_t)inode.file_size;
    stbuf->st_atime = (time_t)inode.access_time;
    stbuf->st_mtime = (time_t)inode.modification_time;
    stbuf->st_ctime = (time_t)inode.creation_time;
    return 0;
}

static int obmafs3_fuse_readdir(const char *path, void *buf,
                                fuse_fill_dir_t filler, off_t offset,
                                struct fuse_file_info *fi,
                                enum fuse_readdir_flags flags)
{
    struct btree_node_filename *entries = NULL;
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
        struct btree_node_filename cat_entry;

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
    struct btree_node_filename cat_entry;
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

    /* Read-only filesystem */
    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        return -EACCES;

    return 0;
}

static int obmafs3_fuse_read(const char *path, char *buf, size_t size,
                             off_t offset, struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct btree_node_filename cat_entry;
    struct btree_node_inode inode;
    int rc;

    (void)fi;

    rc = resolve_path(path, &parent_id, &name);
    if (rc != 0)
        return rc;

    rc = obmafs3_catalog_lookup(g_ctx, parent_id, name, &cat_entry);
    if (rc != OBMAFS3_OK)
        return -EIO;

    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    if ((uint64_t)offset >= inode.file_size)
        return 0;

    if ((uint64_t)offset + size > inode.file_size)
        size = (size_t)(inode.file_size - (uint64_t)offset);

    rc = obmafs3_read_file_data(g_ctx, &inode, (uint64_t)offset, buf, size);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return (int)size;
}

struct fuse_operations obmafs3_fuse_ops = {
    .getattr = obmafs3_fuse_getattr,
    .readdir = obmafs3_fuse_readdir,
    .open    = obmafs3_fuse_open,
    .read    = obmafs3_fuse_read,
};
