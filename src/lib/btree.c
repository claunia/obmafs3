/*
 * btree.c - OBMAFS3 B+Tree operations
 *
 * Current implementation: each leaf node stores one record.
 * Nodes are linked via right_link forming a sorted linked list.
 * Full multi-key B+Tree indexing is planned for a future update.
 */
#include "obmafs.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int obmafs3_btree_header_read(struct obmafs3_ctx *ctx, uint64_t lba,
                              struct btree_header *hdr)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
    if (rc != OBMAFS3_OK) {
        free(buf);
        return rc;
    }

    memcpy(hdr, buf, sizeof(*hdr));
    free(buf);

    if (hdr->magic != OBMAFS3_BTREE_HDR_MAGIC)
        return OBMAFS3_ERR_BADMAGIC;

    return OBMAFS3_OK;
}

int obmafs3_btree_header_write(struct obmafs3_ctx *ctx, uint64_t lba,
                               const struct btree_header *hdr)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    memcpy(buf, hdr, sizeof(*hdr));
    int rc = obmafs3_block_write(ctx, lba, buf, (size_t)ctx->sb.block_size);
    free(buf);
    return rc;
}

int obmafs3_catalog_lookup(struct obmafs3_ctx *ctx, uint64_t parent_id,
                           const char *name,
                           struct btree_node_filename *entry)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = ctx->catalog_hdr.root_node_lba;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_filename node;
        memcpy(&node, buf, sizeof(node));

        if (node.header.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (node.parent_id == parent_id &&
            strcmp(node.name, name) == 0) {
            *entry = node;
            free(buf);
            return OBMAFS3_OK;
        }

        lba = node.header.right_link;
    }

    free(buf);
    return OBMAFS3_ERR_NOTFOUND;
}

int obmafs3_catalog_list(struct obmafs3_ctx *ctx, uint64_t parent_id,
                         struct btree_node_filename **entries,
                         uint32_t *count)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    struct btree_node_filename *result = NULL;
    uint32_t n = 0;
    uint32_t cap = 0;
    uint64_t lba = ctx->catalog_hdr.root_node_lba;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            free(result);
            return rc;
        }

        struct btree_node_filename node;
        memcpy(&node, buf, sizeof(node));

        if (node.header.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            free(result);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (node.parent_id == parent_id) {
            if (n >= cap) {
                cap = (cap == 0) ? 16 : cap * 2;
                struct btree_node_filename *tmp =
                    realloc(result, cap * sizeof(*tmp));
                if (!tmp) {
                    free(buf);
                    free(result);
                    return OBMAFS3_ERR_NOMEM;
                }
                result = tmp;
            }
            result[n++] = node;
        }

        lba = node.header.right_link;
    }

    free(buf);
    *entries = result;
    *count = n;
    return OBMAFS3_OK;
}

void obmafs3_catalog_list_free(struct btree_node_filename *entries)
{
    free(entries);
}

int obmafs3_inode_get(struct obmafs3_ctx *ctx, uint64_t inode_id,
                      struct btree_node_inode *inode)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = ctx->inode_hdr.root_node_lba;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_inode node;
        memcpy(&node, buf, sizeof(node));

        if (node.header.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (node.inode_id == inode_id) {
            *inode = node;
            free(buf);
            return OBMAFS3_OK;
        }

        lba = node.header.right_link;
    }

    free(buf);
    return OBMAFS3_ERR_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/*  Block allocation                                                   */
/* ------------------------------------------------------------------ */

int obmafs3_alloc_block(struct obmafs3_ctx *ctx, uint64_t *lba)
{
    uint64_t max_lba = ctx->sb.total_bytes / ctx->sb.block_size;
    if (ctx->sb.next_free_lba >= max_lba)
        return OBMAFS3_ERR_NOSPC;

    *lba = ctx->sb.next_free_lba;
    ctx->sb.next_free_lba++;

    /* Persist updated superblock */
    return obmafs3_sb_write(ctx->fd, &ctx->sb);
}

int obmafs3_alloc_blocks(struct obmafs3_ctx *ctx, uint64_t count,
                         uint64_t *start_lba)
{
    uint64_t max_lba = ctx->sb.total_bytes / ctx->sb.block_size;
    if (ctx->sb.next_free_lba + count > max_lba)
        return OBMAFS3_ERR_NOSPC;

    *start_lba = ctx->sb.next_free_lba;
    ctx->sb.next_free_lba += count;

    return obmafs3_sb_write(ctx->fd, &ctx->sb);
}

uint64_t obmafs3_alloc_inode_id(struct obmafs3_ctx *ctx)
{
    uint64_t id = ctx->sb.next_inode_id;
    ctx->sb.next_inode_id++;
    obmafs3_sb_write(ctx->fd, &ctx->sb);
    return id;
}

/* ------------------------------------------------------------------ */
/*  Catalog insert                                                     */
/* ------------------------------------------------------------------ */

int obmafs3_catalog_insert(struct obmafs3_ctx *ctx,
                           const struct btree_node_filename *entry)
{
    uint64_t new_lba;
    int rc = obmafs3_alloc_block(ctx, &new_lba);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Write the new catalog node */
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    memcpy(buf, entry, sizeof(*entry));
    rc = obmafs3_block_write(ctx, new_lba, buf, (size_t)ctx->sb.block_size);
    free(buf);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Walk to the last node and link it */
    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!node_buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = ctx->catalog_hdr.root_node_lba;
    uint64_t prev_lba = lba;

    while (lba != 0) {
        rc = obmafs3_block_read(ctx, lba, node_buf,
                                (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(node_buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, node_buf, sizeof(hdr));
        prev_lba = lba;

        if (hdr.right_link == 0)
            break;

        lba = hdr.right_link;
    }

    /* Update the last node's right_link to point to the new node */
    struct btree_node_header hdr;
    memcpy(&hdr, node_buf, sizeof(hdr));
    hdr.right_link = new_lba;
    memcpy(node_buf, &hdr, sizeof(hdr));
    rc = obmafs3_block_write(ctx, prev_lba, node_buf,
                             (size_t)ctx->sb.block_size);
    free(node_buf);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Update catalog tree header */
    ctx->catalog_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba,
                                      &ctx->catalog_hdr);
}

/* ------------------------------------------------------------------ */
/*  Catalog delete                                                     */
/* ------------------------------------------------------------------ */

int obmafs3_catalog_delete(struct obmafs3_ctx *ctx, uint64_t parent_id,
                           const char *name)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = ctx->catalog_hdr.root_node_lba;
    uint64_t prev_lba = 0;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_filename node;
        memcpy(&node, buf, sizeof(node));

        if (node.header.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (node.parent_id == parent_id &&
            strcmp(node.name, name) == 0) {
            /* Found. Unlink this node from the chain. */
            if (prev_lba == 0) {
                /* It's the root node; promote the next node */
                if (node.header.right_link != 0) {
                    ctx->catalog_hdr.root_node_lba =
                        node.header.right_link;
                } else {
                    /* Last entry - shouldn't delete root "/" though */
                    free(buf);
                    return OBMAFS3_ERR_INVAL;
                }
            } else {
                /* Read previous node and update its right_link */
                uint8_t *prev_buf = calloc(1,
                                           (size_t)ctx->sb.block_size);
                if (!prev_buf) {
                    free(buf);
                    return OBMAFS3_ERR_NOMEM;
                }
                rc = obmafs3_block_read(ctx, prev_lba, prev_buf,
                                        (size_t)ctx->sb.block_size);
                if (rc != OBMAFS3_OK) {
                    free(prev_buf);
                    free(buf);
                    return rc;
                }
                struct btree_node_header prev_hdr;
                memcpy(&prev_hdr, prev_buf, sizeof(prev_hdr));
                prev_hdr.right_link = node.header.right_link;
                memcpy(prev_buf, &prev_hdr, sizeof(prev_hdr));
                rc = obmafs3_block_write(ctx, prev_lba, prev_buf,
                                         (size_t)ctx->sb.block_size);
                free(prev_buf);
                if (rc != OBMAFS3_OK) {
                    free(buf);
                    return rc;
                }
            }

            ctx->catalog_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.catalog_lba,
                                            &ctx->catalog_hdr);
            free(buf);
            return rc;
        }

        prev_lba = lba;
        lba = node.header.right_link;
    }

    free(buf);
    return OBMAFS3_ERR_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/*  Inode insert / update                                              */
/* ------------------------------------------------------------------ */

int obmafs3_inode_put(struct obmafs3_ctx *ctx,
                      const struct btree_node_inode *inode)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    /* Try to find and update existing inode */
    uint64_t lba = ctx->inode_hdr.root_node_lba;
    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_inode node;
        memcpy(&node, buf, sizeof(node));

        if (node.header.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (node.inode_id == inode->inode_id) {
            /* Update in place */
            memset(buf, 0, (size_t)ctx->sb.block_size);
            memcpy(buf, inode, sizeof(*inode));
            rc = obmafs3_block_write(ctx, lba, buf,
                                     (size_t)ctx->sb.block_size);
            free(buf);
            return rc;
        }

        lba = node.header.right_link;
    }

    free(buf);

    /* Inode not found, insert a new node */
    uint64_t new_lba;
    int rc = obmafs3_alloc_block(ctx, &new_lba);
    if (rc != OBMAFS3_OK)
        return rc;

    buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    memcpy(buf, inode, sizeof(*inode));
    rc = obmafs3_block_write(ctx, new_lba, buf, (size_t)ctx->sb.block_size);
    free(buf);
    if (rc != OBMAFS3_OK)
        return rc;

    /* Walk to the last inode node and link */
    uint8_t *node_buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!node_buf)
        return OBMAFS3_ERR_NOMEM;

    lba = ctx->inode_hdr.root_node_lba;
    uint64_t prev_lba = lba;

    while (lba != 0) {
        rc = obmafs3_block_read(ctx, lba, node_buf,
                                (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(node_buf);
            return rc;
        }

        struct btree_node_header hdr;
        memcpy(&hdr, node_buf, sizeof(hdr));
        prev_lba = lba;

        if (hdr.right_link == 0)
            break;

        lba = hdr.right_link;
    }

    struct btree_node_header nhdr;
    memcpy(&nhdr, node_buf, sizeof(nhdr));
    nhdr.right_link = new_lba;
    memcpy(node_buf, &nhdr, sizeof(nhdr));
    rc = obmafs3_block_write(ctx, prev_lba, node_buf,
                             (size_t)ctx->sb.block_size);
    free(node_buf);
    if (rc != OBMAFS3_OK)
        return rc;

    ctx->inode_hdr.total_nodes++;
    return obmafs3_btree_header_write(ctx, ctx->sb.inode_lba,
                                      &ctx->inode_hdr);
}

int obmafs3_inode_delete(struct obmafs3_ctx *ctx, uint64_t inode_id)
{
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if (!buf)
        return OBMAFS3_ERR_NOMEM;

    uint64_t lba = ctx->inode_hdr.root_node_lba;
    uint64_t prev_lba = 0;

    while (lba != 0) {
        int rc = obmafs3_block_read(ctx, lba, buf,
                                    (size_t)ctx->sb.block_size);
        if (rc != OBMAFS3_OK) {
            free(buf);
            return rc;
        }

        struct btree_node_inode node;
        memcpy(&node, buf, sizeof(node));

        if (node.header.magic != OBMAFS3_BTREE_NODE_MAGIC) {
            free(buf);
            return OBMAFS3_ERR_BADMAGIC;
        }

        if (node.inode_id == inode_id) {
            if (prev_lba == 0) {
                if (node.header.right_link != 0) {
                    ctx->inode_hdr.root_node_lba =
                        node.header.right_link;
                } else {
                    free(buf);
                    return OBMAFS3_ERR_INVAL;
                }
            } else {
                uint8_t *prev_buf = calloc(1,
                                           (size_t)ctx->sb.block_size);
                if (!prev_buf) {
                    free(buf);
                    return OBMAFS3_ERR_NOMEM;
                }
                rc = obmafs3_block_read(ctx, prev_lba, prev_buf,
                                        (size_t)ctx->sb.block_size);
                if (rc != OBMAFS3_OK) {
                    free(prev_buf);
                    free(buf);
                    return rc;
                }
                struct btree_node_header prev_hdr;
                memcpy(&prev_hdr, prev_buf, sizeof(prev_hdr));
                prev_hdr.right_link = node.header.right_link;
                memcpy(prev_buf, &prev_hdr, sizeof(prev_hdr));
                rc = obmafs3_block_write(ctx, prev_lba, prev_buf,
                                         (size_t)ctx->sb.block_size);
                free(prev_buf);
                if (rc != OBMAFS3_OK) {
                    free(buf);
                    return rc;
                }
            }

            ctx->inode_hdr.total_nodes--;
            rc = obmafs3_btree_header_write(ctx, ctx->sb.inode_lba,
                                            &ctx->inode_hdr);
            free(buf);
            return rc;
        }

        prev_lba = lba;
        lba = node.header.right_link;
    }

    free(buf);
    return OBMAFS3_ERR_NOTFOUND;
}
