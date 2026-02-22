/*
 * fsck_crosscheck.c — Cross-reference: orphan inodes & dangling catalog
 *                     entries for obmafsck.
 */
#include "fsck.h"

/* ------------------------------------------------------------------ */
/*  Cross-reference: orphan inodes & dangling catalog entries          */
/* ------------------------------------------------------------------ */

/**
 * Catalog entry reference collected from a catalog leaf node.
 * Stores enough information to identify and delete the entry.
 */
struct catalog_ref
{
    uint64_t inode_id;
    uint64_t parent_id;
    char     name[256];
};

/**
 * Walk all catalog B+Tree leaf nodes and collect every
 * (inode_id, parent_id, name) tuple.
 *
 * @param ctx       Filesystem context.
 * @param out_refs  Output: heap-allocated array of catalog_ref.
 * @param out_count Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
int collect_catalog_refs(struct obmafs3_ctx *ctx, struct catalog_ref **out_refs, uint64_t *out_count)
{
    *out_refs  = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->catalog_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    struct catalog_ref *refs = NULL;
    uint64_t count = 0, cap = 0;

    /* Iterative DFS */
    uint64_t *stack = malloc(64 * sizeof(uint64_t));
    uint64_t stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(refs); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct catalog_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect catalog records */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                struct catalog_ref *tmp = realloc(refs, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(refs); return OBMAFS3_ERR_NOMEM; }
                refs = tmp;
            }
            struct catalog_record crec;
            memcpy(&crec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(crec), sizeof(crec));
            refs[count].inode_id  = crec.inode_id;
            refs[count].parent_id = crec.parent_id;
            memset(refs[count].name, 0, sizeof(refs[count].name));
            memcpy(refs[count].name, crec.name, sizeof(crec.name));
            count++;
        }
    }

    free(buf);
    free(stack);
    *out_refs  = refs;
    *out_count = count;
    return OBMAFS3_OK;
}

/**
 * Walk all inode B+Tree leaf nodes and collect every inode_id.
 *
 * @param ctx        Filesystem context.
 * @param out_ids    Output: heap-allocated array of inode_id values.
 * @param out_count  Output: number of elements.
 * @return @c OBMAFS3_OK on success.
 */
int collect_inode_ids(struct obmafs3_ctx *ctx, uint64_t **out_ids, uint64_t *out_count)
{
    *out_ids   = NULL;
    *out_count = 0;

    uint64_t root_lba = ctx->inode_hdr.root_node_lba;
    if(root_lba == 0) return OBMAFS3_OK;

    size_t bsz = (size_t)ctx->sb.block_size;
    uint8_t *buf = calloc(1, bsz);
    if(!buf) return OBMAFS3_ERR_NOMEM;

    uint64_t *ids = NULL;
    uint64_t count = 0, cap = 0;

    uint64_t *stack = malloc(64 * sizeof(uint64_t));
    uint64_t stk_size = 0, stk_cap = 64;
    if(!stack) { free(buf); return OBMAFS3_ERR_NOMEM; }

    stack[stk_size++] = root_lba;

    while(stk_size > 0)
    {
        uint64_t lba = stack[--stk_size];

        int rc = obmafs3_block_read(ctx, lba, buf, bsz);
        if(rc != OBMAFS3_OK) { free(buf); free(stack); free(ids); return rc; }

        struct btree_node_header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if(hdr.magic != OBMAFS3_BTREE_NODE_MAGIC) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_BADMAGIC; }

        if(hdr.level > 0)
        {
            for(uint16_t i = 0; i < hdr.node_keys; i++)
            {
                struct btree_index_entry ie;
                memcpy(&ie, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(ie), sizeof(ie));
                if(stk_size >= stk_cap)
                {
                    stk_cap *= 2;
                    uint64_t *tmp = realloc(stack, stk_cap * sizeof(*tmp));
                    if(!tmp) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_NOMEM; }
                    stack = tmp;
                }
                stack[stk_size++] = ie.child_lba;
            }
            continue;
        }

        /* Leaf: collect inode_id from each inode_record */
        for(uint16_t i = 0; i < hdr.node_keys; i++)
        {
            if(count >= cap)
            {
                cap = cap == 0 ? 256 : cap * 2;
                uint64_t *tmp = realloc(ids, cap * sizeof(*tmp));
                if(!tmp) { free(buf); free(stack); free(ids); return OBMAFS3_ERR_NOMEM; }
                ids = tmp;
            }
            struct inode_record irec;
            memcpy(&irec, buf + sizeof(struct btree_node_header) + (size_t)i * sizeof(irec), sizeof(irec));
            ids[count++] = irec.inode_id;
        }
    }

    free(buf);
    free(stack);
    *out_ids   = ids;
    *out_count = count;
    return OBMAFS3_OK;
}

/** qsort comparator for uint64_t values. */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

/** qsort comparator for catalog_ref by inode_id. */
static int cmp_catalog_ref(const void *a, const void *b)
{
    const struct catalog_ref *ra = (const struct catalog_ref *)a;
    const struct catalog_ref *rb = (const struct catalog_ref *)b;
    return (ra->inode_id < rb->inode_id) ? -1 : (ra->inode_id > rb->inode_id) ? 1 : 0;
}

/** Binary search: return non-zero if @p id is present in the sorted array. */
static int u64_sorted_contains(const uint64_t *arr, uint64_t count, uint64_t id)
{
    uint64_t lo = 0, hi = count;
    while(lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if(arr[mid] < id)      lo = mid + 1;
        else if(arr[mid] > id) hi = mid;
        else                   return 1;
    }
    return 0;
}

/**
 * Cross-reference the inode and catalog trees.
 *
 * Detects:
 * - **Orphan inodes**: inode_id present in the inode tree but not
 *   referenced by any catalog entry.  Fixed by deleting the inode.
 * - **Dangling catalog entries**: a catalog entry whose inode_id does
 *   not exist in the inode tree.  Fixed by deleting the catalog entry.
 *
 * @param ctx        Filesystem context.
 * @param auto_yes   If nonzero, always repair without prompting.
 * @param auto_no    If nonzero, never repair.
 * @param errors     In/out: incremented for each unfixed error.
 */
void cross_check_inodes_catalog(struct obmafs3_ctx *ctx, int auto_yes, int auto_no, int *errors)
{
    struct catalog_ref *cat_refs  = NULL;
    uint64_t           *inode_ids = NULL;
    uint64_t            cat_count = 0, ino_count = 0;
    int                 rc;

    printf("\n  %sInode / Catalog cross-reference%s\n", CLR_BOLD, CLR_RESET);

    rc = collect_catalog_refs(ctx, &cat_refs, &cat_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Catalog tree:", "could not walk (%d)", rc);
        (*errors)++;
        return;
    }

    rc = collect_inode_ids(ctx, &inode_ids, &ino_count);
    if(rc != OBMAFS3_OK)
    {
        result_bad("Inode tree:", "could not walk (%d)", rc);
        free(cat_refs);
        (*errors)++;
        return;
    }

    /* Build sorted inode_id set from catalog refs */
    uint64_t *cat_ids = NULL;
    uint64_t  cat_unique = 0;
    if(cat_count > 0)
    {
        qsort(cat_refs, (size_t)cat_count, sizeof(cat_refs[0]), cmp_catalog_ref);

        /* Deduplicate to get unique inode_ids referenced by catalog */
        cat_ids = malloc(cat_count * sizeof(*cat_ids));
        if(cat_ids)
        {
            cat_ids[0] = cat_refs[0].inode_id;
            cat_unique = 1;
            for(uint64_t i = 1; i < cat_count; i++)
            {
                if(cat_refs[i].inode_id != cat_ids[cat_unique - 1])
                    cat_ids[cat_unique++] = cat_refs[i].inode_id;
            }
        }
    }

    /* Sort inode_ids */
    if(ino_count > 0)
        qsort(inode_ids, (size_t)ino_count, sizeof(inode_ids[0]), cmp_u64);

    /* ---- Detect orphan inodes ---- */
    uint64_t orphan_count = 0;
    if(inode_ids && cat_ids)
    {
        for(uint64_t i = 0; i < ino_count; i++)
        {
            uint64_t id = inode_ids[i];
            if(id == OBMAFS3_ROOT_INODE_ID) continue; /* root dir always exists */
            if(!u64_sorted_contains(cat_ids, cat_unique, id))
                orphan_count++;
        }
    }

    if(orphan_count == 0)
    {
        result_ok("Orphan inodes:", "");
    }
    else
    {
        result_bad("Orphan inodes:", "%" PRIu64 " found", orphan_count);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Re-link orphan inodes into lost+found?"))
        {
            /* Ensure lost+found directory exists under root */
            struct catalog_record lf_cat;
            uint64_t lf_inode_id = 0;
            rc = obmafs3_catalog_lookup(ctx, OBMAFS3_ROOT_INODE_ID, "lost+found", &lf_cat);
            if(rc == OBMAFS3_OK)
            {
                lf_inode_id = lf_cat.inode_id;
            }
            else
            {
                /* Create lost+found directory */
                lf_inode_id = obmafs3_alloc_inode_id(ctx);

                struct catalog_record new_cat;
                memset(&new_cat, 0, sizeof(new_cat));
                new_cat.inode_id       = lf_inode_id;
                new_cat.parent_id      = OBMAFS3_ROOT_INODE_ID;
                new_cat.directory_flag  = 1;
                strncpy(new_cat.name, "lost+found", sizeof(new_cat.name) - 1);
                rc = obmafs3_catalog_insert(ctx, &new_cat);
                if(rc != OBMAFS3_OK)
                {
                    printf("    Error: could not create lost+found directory (%d)\n", rc);
                    goto skip_orphan_fix;
                }

                uint64_t now = (uint64_t)time(NULL);
                struct inode_record lf_inode;
                memset(&lf_inode, 0, sizeof(lf_inode));
                lf_inode.inode_id          = lf_inode_id;
                lf_inode.uid               = 0;
                lf_inode.gid               = 0;
                lf_inode.mode              = 0755;
                lf_inode.creation_time     = now;
                lf_inode.modification_time = now;
                lf_inode.access_time       = now;
                lf_inode.file_type         = kFileTypeDirectory;
                lf_inode.ref_count         = 1;

                rc = obmafs3_inode_put(ctx, &lf_inode);
                if(rc != OBMAFS3_OK)
                {
                    printf("    Error: could not create lost+found inode (%d)\n", rc);
                    goto skip_orphan_fix;
                }
                printf("    Created lost+found directory (inode %" PRIu64 ")\n", lf_inode_id);
            }

            /* Re-link each orphan inode into lost+found */
            uint64_t fixed = 0;
            for(uint64_t i = 0; i < ino_count; i++)
            {
                uint64_t id = inode_ids[i];
                if(id == OBMAFS3_ROOT_INODE_ID) continue;
                if(id == lf_inode_id) continue;  /* skip lost+found itself */
                if(!u64_sorted_contains(cat_ids, cat_unique, id))
                {
                    /* Read the inode to determine file type */
                    struct inode_record irec;
                    rc = obmafs3_inode_get(ctx, id, &irec);
                    if(rc != OBMAFS3_OK) continue;

                    /* Build a name: "inode_<id>" */
                    char name_buf[64];
                    snprintf(name_buf, sizeof(name_buf), "inode_%" PRIu64, id);

                    struct catalog_record new_entry;
                    memset(&new_entry, 0, sizeof(new_entry));
                    new_entry.inode_id      = id;
                    new_entry.parent_id     = lf_inode_id;
                    new_entry.directory_flag = (irec.file_type == kFileTypeDirectory) ? 1 : 0;
                    strncpy(new_entry.name, name_buf, sizeof(new_entry.name) - 1);

                    rc = obmafs3_catalog_insert(ctx, &new_entry);
                    if(rc == OBMAFS3_OK)
                    {
                        /* Ensure ref_count is at least 1 */
                        if(irec.ref_count == 0)
                        {
                            irec.ref_count = 1;
                            obmafs3_inode_put(ctx, &irec);
                        }
                        fixed++;
                    }
                }
            }
            printf("    Re-linked %" PRIu64 " orphan inode(s) into lost+found.\n", fixed);
            if(fixed == orphan_count) (*errors)--;
        }
    }
skip_orphan_fix:

    /* ---- Detect dangling catalog entries ---- */
    uint64_t dangling_count = 0;
    if(cat_refs && inode_ids)
    {
        for(uint64_t i = 0; i < cat_count; i++)
        {
            if(!u64_sorted_contains(inode_ids, ino_count, cat_refs[i].inode_id))
                dangling_count++;
        }
    }

    if(dangling_count == 0)
    {
        result_ok("Dangling catalog:", "");
    }
    else
    {
        result_bad("Dangling catalog:", "%" PRIu64 " found", dangling_count);
        (*errors)++;

        if(ask_fix(auto_yes, auto_no, "  Delete dangling catalog entries?"))
        {
            uint64_t fixed = 0;
            for(uint64_t i = 0; i < cat_count; i++)
            {
                if(!u64_sorted_contains(inode_ids, ino_count, cat_refs[i].inode_id))
                {
                    rc = obmafs3_catalog_delete(ctx, cat_refs[i].parent_id, cat_refs[i].name);
                    if(rc == OBMAFS3_OK) fixed++;
                }
            }
            printf("    Deleted %" PRIu64 " dangling catalog entry(ies).\n", fixed);
            if(fixed == dangling_count) (*errors)--;
        }
    }

    free(cat_ids);
    free(cat_refs);
    free(inode_ids);
}

/* ------------------------------------------------------------------ */
