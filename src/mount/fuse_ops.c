/*
 * fuse_ops.c - OBMAFS3 FUSE filesystem operations
 *
 * Implements read-write access for regular files:
 *   getattr, readdir, open, read, create, write, truncate, unlink, utimens
 */
#define FUSE_USE_VERSION 31

#include "fuse_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

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

/* ------------------------------------------------------------------ */
/*  Write operations                                                   */
/* ------------------------------------------------------------------ */

static int obmafs3_fuse_create(const char *path, mode_t mode,
                               struct fuse_file_info *fi)
{
    uint64_t parent_id;
    const char *name;
    struct btree_node_filename cat_entry;
    int rc;

    (void)fi;

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
    struct btree_node_filename new_cat;
    memset(&new_cat, 0, sizeof(new_cat));
    new_cat.header.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    new_cat.header.record_type = kBtreeDataTypeFilename;
    new_cat.header.node_keys   = 1;
    new_cat.header.keys_length = (uint16_t)(sizeof(new_cat) -
                                            sizeof(new_cat.header));
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

    struct btree_node_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.header.magic       = OBMAFS3_BTREE_NODE_MAGIC;
    new_inode.header.record_type = kBtreeDataTypeInode;
    new_inode.header.node_keys   = 1;
    new_inode.header.keys_length = (uint16_t)(sizeof(new_inode) -
                                              sizeof(new_inode.header));
    new_inode.inode_id          = new_inode_id;
    new_inode.uid               = fctx->uid;
    new_inode.gid               = fctx->gid;
    new_inode.mode              = mode & 07777;
    new_inode.creation_time     = now;
    new_inode.modification_time = now;
    new_inode.access_time       = now;
    new_inode.file_size         = 0;
    new_inode.file_type         = kFileTypeRegular;

    rc = obmafs3_inode_put(g_ctx, &new_inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_write(const char *path, const char *buf,
                              size_t size, off_t offset,
                              struct fuse_file_info *fi)
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

    rc = obmafs3_write_file_data(g_ctx, &inode,
                                 (uint64_t)offset, buf, size);
    if (rc == OBMAFS3_ERR_NOSPC)
        return -ENOSPC;
    if (rc != OBMAFS3_OK)
        return -EIO;

    /* Update inode timestamps and size */
    inode.modification_time = (uint64_t)time(NULL);
    rc = obmafs3_inode_put(g_ctx, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return (int)size;
}

static int obmafs3_fuse_truncate(const char *path, off_t newsize,
                                 struct fuse_file_info *fi)
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

    if ((uint64_t)newsize > inode.file_size) {
        /* Extending: write zeros to fill the gap */
        size_t gap = (size_t)((uint64_t)newsize - inode.file_size);
        void *zeros = calloc(1, gap);
        if (!zeros)
            return -ENOMEM;
        rc = obmafs3_write_file_data(g_ctx, &inode,
                                     inode.file_size, zeros, gap);
        free(zeros);
        if (rc != OBMAFS3_OK)
            return -EIO;
    } else {
        /* Shrinking: just update file_size.
         * Blocks remain allocated but data beyond newsize is ignored. */
        inode.file_size = (uint64_t)newsize;
    }

    inode.modification_time = (uint64_t)time(NULL);
    rc = obmafs3_inode_put(g_ctx, &inode);
    if (rc != OBMAFS3_OK)
        return -EIO;

    return 0;
}

static int obmafs3_fuse_unlink(const char *path)
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
    struct btree_node_filename cat_entry;
    struct btree_node_inode inode;
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

struct fuse_operations obmafs3_fuse_ops = {
    .getattr  = obmafs3_fuse_getattr,
    .readdir  = obmafs3_fuse_readdir,
    .open     = obmafs3_fuse_open,
    .read     = obmafs3_fuse_read,
    .create   = obmafs3_fuse_create,
    .write    = obmafs3_fuse_write,
    .truncate = obmafs3_fuse_truncate,
    .unlink   = obmafs3_fuse_unlink,
    .utimens  = obmafs3_fuse_utimens,
};
