// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : user_index.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     User-defined secondary index management.
//     Registry is stored as a linked list of on-disk blocks.
//     Each per-key index reuses the existing metadata_idx or
//     metadata_numeric_idx B+Tree infrastructure.
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

#include "btree_internal.h"
#include "debug.h"

/* Maximum entries per registry block:
 * (block_size - sizeof(user_index_registry_block)) / sizeof(user_index_registry_entry)
 * For 4096-byte blocks: (4096 - 16) / 272 = 15 */
static uint32_t registry_max_entries(const struct obmafs3_ctx *ctx)
{
    return (uint32_t)((ctx->sb.block_size - sizeof(struct user_index_registry_block)) /
                      sizeof(struct user_index_registry_entry));
}

/* ------------------------------------------------------------------ */
/*  Load registry from disk into ctx                                   */
/* ------------------------------------------------------------------ */

int obmafs3_user_index_load(struct obmafs3_ctx *ctx)
{
    ctx->user_indexes     = NULL;
    ctx->user_index_count = 0;

    if(ctx->sb.user_index_registry_lba == 0) return OBMAFS3_OK;

    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    uint32_t cap = 8;
    struct user_index_ctx *arr = malloc(cap * sizeof(struct user_index_ctx));
    if(!arr) { free(buf); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }

    uint64_t lba = ctx->sb.user_index_registry_lba;
    uint32_t n   = 0;

    while(lba != 0)
    {
        int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) { free(arr); free(buf); return rc; }

        struct user_index_registry_block blk;
        memcpy(&blk, buf, sizeof(blk));

        const uint8_t *data = buf + sizeof(struct user_index_registry_block);
        for(uint32_t i = 0; i < blk.entry_count; i++)
        {
            struct user_index_registry_entry entry;
            memcpy(&entry, data + (size_t)i * sizeof(entry), sizeof(entry));

            if(n >= cap)
            {
                cap *= 2;
                struct user_index_ctx *tmp = realloc(arr, cap * sizeof(struct user_index_ctx));
                if(!tmp) { free(arr); free(buf); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }
                arr = tmp;
            }

            memset(&arr[n], 0, sizeof(arr[n]));
            memcpy(arr[n].key, entry.key, METADATA_KEY_MAX);
            arr[n].value_type  = entry.value_type;
            arr[n].header_lba  = entry.index_header_lba;

            /* Read the per-key B+Tree header */
            if(entry.index_header_lba != 0)
            {
                rc = obmafs3_btree_header_read(ctx, entry.index_header_lba, &arr[n].hdr);
                if(rc != OBMAFS3_OK)
                {
                    /* Non-fatal: header might be corrupt; fsck will fix */
                    memset(&arr[n].hdr, 0, sizeof(arr[n].hdr));
                }
            }
            n++;
        }

        lba = blk.next_lba;
    }

    free(buf);
    ctx->user_indexes     = arr;
    ctx->user_index_count = n;
    return OBMAFS3_OK;
}

void obmafs3_user_index_free(struct obmafs3_ctx *ctx)
{
    free(ctx->user_indexes);
    ctx->user_indexes     = NULL;
    ctx->user_index_count = 0;
}

/* ------------------------------------------------------------------ */
/*  Find a user index by key                                           */
/* ------------------------------------------------------------------ */

struct user_index_ctx *obmafs3_user_index_find(struct obmafs3_ctx *ctx, const char *key)
{
    for(uint32_t i = 0; i < ctx->user_index_count; i++)
        if(strncmp(ctx->user_indexes[i].key, key, METADATA_KEY_MAX) == 0)
            return &ctx->user_indexes[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Write the full registry to disk                                    */
/* ------------------------------------------------------------------ */

static int registry_write(struct obmafs3_ctx *ctx)
{
    uint32_t max_per_block = registry_max_entries(ctx);
    uint32_t n             = ctx->user_index_count;
    uint32_t blocks_needed = (n + max_per_block - 1) / max_per_block;
    if(blocks_needed == 0) blocks_needed = 1; /* keep at least one block */

    /* Collect LBAs for existing registry blocks */
    uint64_t *block_lbas = NULL;
    uint32_t  block_count = 0;

    if(ctx->sb.user_index_registry_lba != 0)
    {
        uint32_t bc = 0, bcap = 4;
        block_lbas = malloc(bcap * sizeof(uint64_t));
        if(!block_lbas) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        uint64_t lba = ctx->sb.user_index_registry_lba;
        uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
        if(!buf) { free(block_lbas); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }

        while(lba != 0)
        {
            if(bc >= bcap) { bcap *= 2; block_lbas = realloc(block_lbas, bcap * sizeof(uint64_t)); }
            block_lbas[bc++] = lba;
            int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) break;
            struct user_index_registry_block blk;
            memcpy(&blk, buf, sizeof(blk));
            lba = blk.next_lba;
        }
        free(buf);
        block_count = bc;
    }

    /* Allocate additional blocks if needed */
    while(block_count < blocks_needed)
    {
        uint64_t new_lba;
        int rc = obmafs3_alloc_block(ctx, &new_lba);
        if(rc != OBMAFS3_OK) { free(block_lbas); return rc; }
        uint64_t *tmp = realloc(block_lbas, (block_count + 1) * sizeof(uint64_t));
        if(!tmp) { free(block_lbas); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }
        block_lbas = tmp;
        block_lbas[block_count++] = new_lba;
    }

    /* Free excess blocks */
    while(block_count > blocks_needed)
    {
        block_count--;
        obmafs3_free_block(ctx, block_lbas[block_count]);
    }

    /* Write entries across blocks */
    uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
    if(!buf) { free(block_lbas); DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory"); }

    uint32_t entry_idx = 0;
    for(uint32_t b = 0; b < blocks_needed; b++)
    {
        memset(buf, 0, (size_t)ctx->sb.block_size);

        struct user_index_registry_block blk;
        memset(&blk, 0, sizeof(blk));
        blk.next_lba    = (b + 1 < blocks_needed) ? block_lbas[b + 1] : 0;
        blk.entry_count = 0;

        uint8_t *data = buf + sizeof(struct user_index_registry_block);
        while(entry_idx < n && blk.entry_count < max_per_block)
        {
            struct user_index_registry_entry entry;
            memset(&entry, 0, sizeof(entry));
            memcpy(entry.key, ctx->user_indexes[entry_idx].key, METADATA_KEY_MAX);
            entry.value_type       = ctx->user_indexes[entry_idx].value_type;
            entry.index_header_lba = ctx->user_indexes[entry_idx].header_lba;

            memcpy(data + (size_t)blk.entry_count * sizeof(entry), &entry, sizeof(entry));
            blk.entry_count++;
            entry_idx++;
        }

        memcpy(buf, &blk, sizeof(blk));
        int rc = obmafs3_block_write(ctx, block_lbas[b], buf, (size_t)ctx->sb.block_size);
        if(rc != OBMAFS3_OK) { free(buf); free(block_lbas); return rc; }
    }

    ctx->sb.user_index_registry_lba = block_lbas[0];
    free(buf);
    free(block_lbas);
    return obmafs3_sb_write(ctx->fd, &ctx->sb);
}

/* ------------------------------------------------------------------ */
/*  Create a new user-defined index                                    */
/* ------------------------------------------------------------------ */

int obmafs3_user_index_create(struct obmafs3_ctx *ctx, const char *key, uint8_t value_type)
{
    /* Check if already exists */
    if(obmafs3_user_index_find(ctx, key) != NULL)
        DBG_RETURN(OBMAFS3_ERR_INVAL, "index already exists for this key");

    /* Allocate a header block for the new per-key B+Tree */
    uint64_t hdr_lba;
    int rc = obmafs3_alloc_block(ctx, &hdr_lba);
    if(rc != OBMAFS3_OK) return rc;

    struct btree_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = OBMAFS3_BTREE_HDR_MAGIC;
    if(value_type == USER_INDEX_VALUE_TYPE_NUMERIC)
    {
        hdr.data_type = kBtreeDataTypeMetadataNumericIndexEntry;
        hdr.tree_type = kBtreeTypeMetadataNumericIndex;
    }
    else
    {
        hdr.data_type = kBtreeDataTypeMetadataIndexEntry;
        hdr.tree_type = kBtreeTypeMetadataIndex;
    }
    hdr.node_size = (uint16_t)(METADATA_NODE_BLOCKS * ctx->sb.block_size);
    obmafs3_checksum_block(&hdr, sizeof(hdr), hdr.checksum);

    rc = obmafs3_block_write(ctx, hdr_lba, &hdr, sizeof(hdr));
    if(rc != OBMAFS3_OK) return rc;

    /* Add to in-memory array */
    struct user_index_ctx *tmp = realloc(ctx->user_indexes,
                                         (ctx->user_index_count + 1) * sizeof(struct user_index_ctx));
    if(!tmp) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    ctx->user_indexes = tmp;

    struct user_index_ctx *uidx = &ctx->user_indexes[ctx->user_index_count];
    memset(uidx, 0, sizeof(*uidx));
    strncpy(uidx->key, key, METADATA_KEY_MAX - 1);
    uidx->value_type = value_type;
    uidx->header_lba = hdr_lba;
    uidx->hdr        = hdr;
    ctx->user_index_count++;

    /* Set rocompat flag */
    if(!(ctx->sb.rocompat_flags & OBMAFS3_ROCOMPAT_USER_INDEXES))
        ctx->sb.rocompat_flags |= OBMAFS3_ROCOMPAT_USER_INDEXES;

    /* Write registry */
    rc = registry_write(ctx);
    if(rc != OBMAFS3_OK) return rc;

    /* Populate the index by scanning the reverse-index tree for this key */
    if(ctx->sb.metadata_idx_lba == 0 || ctx->metadata_idx_hdr.root_node_lba == 0)
        return OBMAFS3_OK;

    size_t   nsz = (size_t)METADATA_NODE_BLOCKS * (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, nsz);
    if(!buf) return OBMAFS3_OK; /* non-fatal: tree created but empty */

    /* Navigate to the leaf containing (key, "", 0) */
    uint64_t lba = ctx->metadata_idx_hdr.root_node_lba;
    while(1)
    {
        rc = obmafs3_block_read(ctx, lba, buf, nsz);
        if(rc != OBMAFS3_OK) { free(buf); return OBMAFS3_OK; }

        struct btree_node_header bhdr;
        memcpy(&bhdr, buf, sizeof(bhdr));
        if(bhdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); return OBMAFS3_OK; }
        if(bhdr.level == 0) break;

        /* Find the slot for (key, "", 0) using binary search in index entries */
        const uint8_t *data = buf + sizeof(struct btree_node_header);
        uint16_t slot = 0;
        for(uint16_t s = 0; s < bhdr.node_keys; s++)
        {
            struct metadata_idx_index_entry ie;
            memcpy(&ie, data + (size_t)s * sizeof(ie), sizeof(ie));
            if(strncmp(ie.key, key, METADATA_KEY_MAX) <= 0) slot = s;
            else break;
        }
        struct metadata_idx_index_entry ie;
        memcpy(&ie, data + (size_t)slot * sizeof(ie), sizeof(ie));
        lba = ie.child_lba;
    }

    /* Scan leaf chain for records matching this key */
    while(1)
    {
        struct btree_node_header bhdr;
        memcpy(&bhdr, buf, sizeof(bhdr));

        const uint8_t *data = buf + sizeof(struct btree_node_header);
        for(uint16_t i = 0; i < bhdr.node_keys; i++)
        {
            struct metadata_idx_record rec;
            memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));

            int kcmp = strncmp(rec.key, key, METADATA_KEY_MAX);
            if(kcmp < 0) continue;
            if(kcmp > 0) goto populate_done;

            if(value_type == USER_INDEX_VALUE_TYPE_NUMERIC)
            {
                char *end;
                int64_t num = strtoll(rec.value, &end, 10);
                if(*end == '\0' && rec.value[0] != '\0')
                    obmafs3_numidx_put_ex(ctx, &uidx->hdr, uidx->header_lba, key, num, rec.inode_id);
            }
            /* String indexes: TODO in future — would need parameterized midx_tree_put */
        }

        if(bhdr.right_link == 0) break;
        rc = obmafs3_block_read(ctx, bhdr.right_link, buf, nsz);
        if(rc != OBMAFS3_OK) break;
    }

populate_done:
    free(buf);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  Drop a user-defined index                                          */
/* ------------------------------------------------------------------ */

int obmafs3_user_index_drop(struct obmafs3_ctx *ctx, const char *key)
{
    int found = -1;
    for(uint32_t i = 0; i < ctx->user_index_count; i++)
    {
        if(strncmp(ctx->user_indexes[i].key, key, METADATA_KEY_MAX) == 0)
        {
            found = (int)i;
            break;
        }
    }

    if(found < 0) DBG_RETURN(OBMAFS3_ERR_NOTFOUND, "no index for this key");

    /* Free the per-key tree header block.
     * TODO: walk and free all tree nodes (for now just free header) */
    if(ctx->user_indexes[found].header_lba != 0)
        obmafs3_free_block(ctx, ctx->user_indexes[found].header_lba);

    /* Remove from array */
    if((uint32_t)found < ctx->user_index_count - 1)
        memmove(&ctx->user_indexes[found], &ctx->user_indexes[found + 1],
                (ctx->user_index_count - (uint32_t)found - 1) * sizeof(struct user_index_ctx));
    ctx->user_index_count--;

    /* If no more indexes, free registry blocks and clear flag */
    if(ctx->user_index_count == 0)
    {
        /* Free all registry blocks */
        uint64_t lba = ctx->sb.user_index_registry_lba;
        uint8_t *buf = calloc(1, (size_t)ctx->sb.block_size);
        if(buf)
        {
            while(lba != 0)
            {
                struct user_index_registry_block blk;
                int rc = obmafs3_block_read(ctx, lba, buf, (size_t)ctx->sb.block_size);
                if(rc != OBMAFS3_OK) break;
                memcpy(&blk, buf, sizeof(blk));
                obmafs3_free_block(ctx, lba);
                lba = blk.next_lba;
            }
            free(buf);
        }
        ctx->sb.user_index_registry_lba = 0;
        ctx->sb.rocompat_flags &= ~OBMAFS3_ROCOMPAT_USER_INDEXES;
        return obmafs3_sb_write(ctx->fd, &ctx->sb);
    }

    return registry_write(ctx);
}
