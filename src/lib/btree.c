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
