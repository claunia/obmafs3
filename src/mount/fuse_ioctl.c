/*
 * fuse_ioctl.c - OBMAFS3 FUSE ioctl operations
 *
 * Implements: media tag, CD image, media image, and metadata ioctls
 */

#include "fuse_ops_internal.h"
#include "obmafs3_ioctl.h"
#include "debug.h"

#include <limits.h>

/* ------------------------------------------------------------------ */
/*  CD image helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * Check if the 16-byte prefix of a raw CD sector matches the expected
 * sync + MSF + mode for the given LBA and track mode.
 */
static bool cd_prefix_is_generatable(const uint8_t *sector, int64_t lba, uint8_t mode)
{
    /* Build expected prefix */
    uint8_t expected[CD_PREFIX_SIZE];

    /* Sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00 */
    expected[0] = 0x00;
    for(int i = 1; i <= 10; i++) expected[i] = 0xFF;
    expected[11] = 0x00;

    /* MSF in BCD */
    uint8_t minute, second, frame;
    cd_lba_to_msf(lba, &minute, &second, &frame);
    expected[12] = (uint8_t)(((minute / 10) << 4) + minute % 10);
    expected[13] = (uint8_t)(((second / 10) << 4) + second % 10);
    expected[14] = (uint8_t)(((frame / 10) << 4) + frame % 10);

    /* Mode byte */
    switch((enum obmafs3_cd_sector_mode)mode)
    {
        case kCdSectorMode1:
            expected[15] = 0x01;
            break;
        case kCdSectorMode2:
        case kCdSectorMode2Form1:
        case kCdSectorMode2Form2:
            expected[15] = 0x02;
            break;
        default:
            return false;
    }

    return memcmp(sector, expected, CD_PREFIX_SIZE) == 0;
}

/**
 * Write a raw CD sector to the filesystem.
 *
 * Processes a 2352-byte (or 2448-byte with subchannel) raw CD sector.
 * The prefix and suffix are checked for generatability; non-generatable
 * portions are stored in the CD prefix / suffix dedup trees.  Subchannel
 * data is stored in the subchannel tree, and the user data portion is
 * written through the media image dedup path.  A @c cd_sector_map_entry
 * is appended to the per-file cache.
 *
 * @param ffctx  Per-file FUSE context (must be a CD image file).
 * @param arg    CD write argument containing buffer, size, and sector mode.
 * @return 0 on success, negative errno on failure.
 */
static int obmafs3_cd_write_long(struct fuse_file_ctx *ffctx, const char *path,
                                 const struct obmafs3_ioctl_cd_write_arg *arg)
{
    if(!arg) FUSE_RETURN(-EINVAL, "");

    uint32_t bufsz = arg->buffer_size;
    if(bufsz != CD_RAW_SECTOR_SIZE && bufsz != CD_RAW_PLUS_SUB) FUSE_RETURN(-EINVAL, "");

    uint8_t mode = arg->sector_mode;
    if(mode > kCdSectorMode2Form2) FUSE_RETURN(-EINVAL, "");
    if(arg->sector < 0) FUSE_RETURN(-EINVAL, "");

    const uint8_t *raw        = arg->buffer;
    int64_t        sector_lba = arg->sector;

    /* Build the cd_sector_map_entry */
    struct cd_sector_map_entry sme;
    memset(&sme, 0, sizeof(sme));
    sme.sector      = sector_lba;
    sme.sector_mode = mode;

    /* --- Subchannel handling --- */
    if(bufsz == CD_RAW_PLUS_SUB)
    {
        const uint8_t *sub      = raw + CD_RAW_SECTOR_SIZE;
        uint64_t       sub_hash = obmafs3_checksum_xxh64(sub, CD_SUBCHANNEL_SIZE);
        sme.subchannel_hash     = sub_hash;

        /* Store subchannel data in the tree (dedup by hash).
         * Lock around shared B+Tree operations. */
        pthread_rwlock_wrlock(&g_ctx->tree_lock);
        uint8_t existing[CD_SUBCHANNEL_DATA_SIZE];
        int     rc = obmafs3_cd_subchannel_get(g_ctx, sub_hash, existing);
        if(rc == OBMAFS3_ERR_NOTFOUND)
        {
            rc = obmafs3_cd_subchannel_put(g_ctx, sub_hash, sub);
            if(rc != OBMAFS3_OK) { pthread_rwlock_unlock(&g_ctx->tree_lock); FUSE_RETURN(-EIO, ""); }
        }
        else if(rc != OBMAFS3_OK) { pthread_rwlock_unlock(&g_ctx->tree_lock); FUSE_RETURN(-EIO, ""); }
        pthread_rwlock_unlock(&g_ctx->tree_lock);

        /* --- Create .sub sidecar file (kFileTypeSubchannelFile) on first subchannel --- */
        if(ffctx->sub_inode_id == 0 && path)
        {
            /* Build the .sub sidecar path: replace the extension with .sub */
            char sub_path[PATH_MAX];
            strncpy(sub_path, path, sizeof(sub_path) - 1);
            sub_path[sizeof(sub_path) - 1] = '\0';
            char *dot = strrchr(sub_path, '.');
            char *slash = strrchr(sub_path, '/');
            if(dot && (!slash || dot > slash))
                strcpy(dot, ".sub");
            else
                strncat(sub_path, ".sub", sizeof(sub_path) - strlen(sub_path) - 1);

            /* Resolve parent directory and sidecar name */
            uint64_t    sub_parent_id;
            const char *sub_name;
            rc = resolve_path(sub_path, &sub_parent_id, &sub_name);
            if(rc == 0)
            {
                struct catalog_record sub_cat;
                rc = obmafs3_catalog_lookup(g_ctx, sub_parent_id, sub_name, &sub_cat);
                if(rc == OBMAFS3_OK)
                {
                    /* Sidecar already exists — load its inode */
                    ffctx->sub_inode_id = sub_cat.inode_id;
                    obmafs3_inode_get(g_ctx, sub_cat.inode_id, &ffctx->sub_inode);
                }
                else if(rc == OBMAFS3_ERR_NOTFOUND)
                {
                    /* Create the subchannel sidecar file.
                     * file_type = kFileTypeSubchannelFile
                     * sector_count = parent CD image inode_id (to find the sector map)
                     * file_size = parent sector_count * CD_SUBCHANNEL_SIZE (updated below) */
                    uint64_t             sub_id  = obmafs3_alloc_inode_id(g_ctx);
                    uint64_t             now     = (uint64_t)time(NULL);
                    struct fuse_context  *fusectx = fuse_get_context();

                    memset(&ffctx->sub_inode, 0, sizeof(ffctx->sub_inode));
                    ffctx->sub_inode.inode_id          = sub_id;
                    ffctx->sub_inode.uid               = fusectx->uid;
                    ffctx->sub_inode.gid               = fusectx->gid;
                    ffctx->sub_inode.mode              = ffctx->inode.mode;
                    ffctx->sub_inode.creation_time     = now;
                    ffctx->sub_inode.modification_time = now;
                    ffctx->sub_inode.access_time       = now;
                    ffctx->sub_inode.file_size         = 0;
                    ffctx->sub_inode.file_type         = kFileTypeSubchannelFile;
                    ffctx->sub_inode.sector_count      = ffctx->inode_id; /* parent CD inode */
                    ffctx->sub_inode.ref_count         = 1;

                    rc = obmafs3_inode_put(g_ctx, &ffctx->sub_inode);
                    if(rc == OBMAFS3_OK)
                    {
                        memset(&sub_cat, 0, sizeof(sub_cat));
                        sub_cat.inode_id       = sub_id;
                        sub_cat.parent_id      = sub_parent_id;
                        sub_cat.directory_flag  = 0;
                        strncpy(sub_cat.name, sub_name, sizeof(sub_cat.name) - 1);
                        rc = obmafs3_catalog_insert(g_ctx, &sub_cat);
                        if(rc == OBMAFS3_OK) { ffctx->sub_inode_id = sub_id; }
                        else
                        {
                            /* Roll back orphan inode */
                            obmafs3_inode_delete(g_ctx, sub_id);
                        }
                    }
                }
            }
        }
    }

    /* --- Audio mode: entire 2352 bytes stored as data, no prefix/suffix --- */
    if(mode == kCdSectorModeAudio)
    {
        sme.sector_size      = CD_RAW_SECTOR_SIZE;
        sme.generated_prefix = 0;
        sme.generated_suffix = 0;

        uint64_t hash = obmafs3_checksum_xxh64(raw, CD_RAW_SECTOR_SIZE);
        sme.hash      = hash;

        /* Bootstrap dedup block cache for 2352-byte sectors */
        if(!ffctx->db_cache.initialized) ffctx->sector_size = CD_RAW_SECTOR_SIZE;

        struct dedup_block_cache *dbc = &ffctx->db_cache;

        /* Write via the media image data path — dedup_upsert_find handles
         * both hit (skip data) and miss (store + insert) in one traversal,
         * avoiding the overhead of a separate dedup_get_tree + dedup_lookup.
         * Pass NULL cache: CD images use cd_sector_map_entries instead. */
        uint64_t offset = (uint64_t)sector_lba * CD_RAW_SECTOR_SIZE;
        int rc = obmafs3_write_media_image_data(g_ctx, &ffctx->inode, offset, raw, CD_RAW_SECTOR_SIZE,
                                                CD_RAW_SECTOR_SIZE, NULL, dbc);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

        goto cache_and_done;
    }

    /* --- Data modes (Mode 1, Mode 2, Mode 2 Form 1, Mode 2 Form 2) --- */

    /* Lazy-init the ECC context */
    if(!ffctx->ecc_ctx)
    {
        ffctx->ecc_ctx = ecc_cd_init();
        if(!ffctx->ecc_ctx) FUSE_RETURN(-ENOMEM, "");
    }

    /* Check if prefix is generatable */
    bool pfx_gen         = cd_prefix_is_generatable(raw, sector_lba, mode);
    sme.generated_prefix = pfx_gen ? 1 : 0;

    if(!pfx_gen)
    {
        /* Store the non-generatable prefix — lock for B+Tree access */
        uint64_t pfx_hash = obmafs3_checksum_xxh64(raw, CD_PREFIX_SIZE);
        sme.prefix_hash   = pfx_hash;

        pthread_rwlock_wrlock(&g_ctx->tree_lock);
        uint8_t existing[CD_PREFIX_DATA_SIZE];
        int     rc = obmafs3_cd_prefix_get(g_ctx, pfx_hash, existing);
        if(rc == OBMAFS3_ERR_NOTFOUND)
        {
            rc = obmafs3_cd_prefix_put(g_ctx, pfx_hash, raw);
            if(rc != OBMAFS3_OK) { pthread_rwlock_unlock(&g_ctx->tree_lock); FUSE_RETURN(-EIO, ""); }
        }
        else if(rc != OBMAFS3_OK) { pthread_rwlock_unlock(&g_ctx->tree_lock); FUSE_RETURN(-EIO, ""); }
        pthread_rwlock_unlock(&g_ctx->tree_lock);
    }

    /* Check if suffix is generatable (only for modes with ECC/EDC) */
    bool sfx_gen = false;
    switch((enum obmafs3_cd_sector_mode)mode)
    {
        case kCdSectorMode1:
            sfx_gen = ecc_cd_is_suffix_correct(ffctx->ecc_ctx, raw);
            break;
        case kCdSectorMode2Form1:
        case kCdSectorMode2Form2:
            sfx_gen = ecc_cd_is_suffix_correct_mode2(ffctx->ecc_ctx, raw);
            break;
        case kCdSectorMode2:
            /* Raw Mode 2 has no ECC/EDC suffix — the entire remaining
             * 2336 bytes is data.  Suffix is trivially "generatable" (empty). */
            sfx_gen = true;
            break;
        default:
            break;
    }

    sme.generated_suffix = sfx_gen ? 1 : 0;

    if(!sfx_gen)
    {
        /* Store the non-generatable suffix — lock for B+Tree access */
        const uint8_t *suffix   = raw + CD_RAW_SECTOR_SIZE - CD_SUFFIX_SIZE;
        uint64_t       sfx_hash = obmafs3_checksum_xxh64(suffix, CD_SUFFIX_SIZE);
        sme.suffix_hash         = sfx_hash;

        pthread_rwlock_wrlock(&g_ctx->tree_lock);
        uint8_t existing[CD_SUFFIX_DATA_SIZE];
        int     rc = obmafs3_cd_suffix_get(g_ctx, sfx_hash, existing);
        if(rc == OBMAFS3_ERR_NOTFOUND)
        {
            rc = obmafs3_cd_suffix_put(g_ctx, sfx_hash, suffix);
            if(rc != OBMAFS3_OK) { pthread_rwlock_unlock(&g_ctx->tree_lock); FUSE_RETURN(-EIO, ""); }
        }
        else if(rc != OBMAFS3_OK) { pthread_rwlock_unlock(&g_ctx->tree_lock); FUSE_RETURN(-EIO, ""); }
        pthread_rwlock_unlock(&g_ctx->tree_lock);
    }

    /* Store subheader for Mode 2 variants (bytes 16-23) */
    if(mode == kCdSectorMode2 || mode == kCdSectorMode2Form1 || mode == kCdSectorMode2Form2)
    {
        memcpy(sme.subheader, raw + CD_PREFIX_SIZE, 8);
    }

    /* Determine data portion and its size */
    const uint8_t *data_ptr;
    uint16_t       data_size;
    switch((enum obmafs3_cd_sector_mode)mode)
    {
        case kCdSectorMode1:
            /* prefix(16) + data(2048) + suffix(288) */
            data_ptr  = raw + CD_PREFIX_SIZE;
            data_size = CD_DATA_SIZE;
            break;
        case kCdSectorMode2:
            /* prefix(16) + data(2336) — no suffix */
            data_ptr  = raw + CD_PREFIX_SIZE;
            data_size = 2336;
            break;
        case kCdSectorMode2Form1:
            /* prefix(16) + subheader(8) + data(2048) + EDC(4) + ECC(276)
             * The subheader is stored separately; data is the 2048 user bytes */
            data_ptr  = raw + CD_PREFIX_SIZE + 8;
            data_size = CD_DATA_SIZE;
            break;
        case kCdSectorMode2Form2:
            /* prefix(16) + subheader(8) + data(2328) — no ECC, optional EDC
             * For Form 2, the 2328 bytes after subheader are user data */
            data_ptr  = raw + CD_PREFIX_SIZE + 8;
            data_size = 2328;
            break;
        default:
            FUSE_RETURN(-EINVAL, "");
    }

    sme.sector_size = data_size;

    /* Hash the data portion (needed for cd_sector_map_entry) */
    uint64_t hash = obmafs3_checksum_xxh64(data_ptr, data_size);
    sme.hash      = hash;

    {
        /* Bootstrap dedup block cache */
        if(!ffctx->db_cache.initialized) ffctx->sector_size = data_size;

        struct dedup_block_cache *dbc = &ffctx->db_cache;

        /* Write via the media image data path — dedup_upsert_find handles
         * both hit (skip data) and miss (store + insert) in one traversal,
         * avoiding the overhead of a separate dedup_get_tree + dedup_lookup.
         * Pass NULL cache: CD images use cd_sector_map_entries instead. */
        uint64_t offset = (uint64_t)sector_lba * data_size;
        int rc = obmafs3_write_media_image_data(g_ctx, &ffctx->inode, offset, data_ptr, data_size, data_size,
                                                NULL, dbc);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
    }

cache_and_done:
    /* Append cd_sector_map_entry to the cache */
    {
        struct cd_sector_map_cache *cache = &ffctx->cd_sme_cache;
        if(cache->count >= cache->capacity)
        {
            uint64_t new_cap = cache->capacity;
            if(new_cap == 0)
                new_cap = 1024;
            else
                new_cap *= 2;
            struct cd_sector_map_entry *tmp = realloc(cache->entries, (size_t)(new_cap * sizeof(*tmp)));
            if(!tmp) FUSE_RETURN(-ENOMEM, "");
            cache->entries  = tmp;
            cache->capacity = new_cap;
        }
        cache->entries[cache->count++] = sme;
    }

    /* Update inode sector count and virtual file size.
     * The virtual size of an ioctl-written CD image is always
     * sector_count * CD_RAW_SECTOR_SIZE (2352), regardless of the
     * per-mode data size used for dedup storage. */
    if((uint64_t)(sector_lba + 1) > ffctx->inode.sector_count)
        ffctx->inode.sector_count = (uint64_t)(sector_lba + 1);
    ffctx->inode.file_size = ffctx->inode.sector_count * CD_RAW_SECTOR_SIZE;
    ffctx->inode_dirty = 1;

    /* Keep the .sub sidecar file_size in sync with the parent's sector_count.
     * The sidecar has no data of its own — its read path fetches subchannel
     * data from the parent's cd_sector_map_entries via the subchannel B+Tree. */
    if(ffctx->sub_inode_id != 0)
    {
        ffctx->sub_inode.file_size = ffctx->inode.sector_count * CD_SUBCHANNEL_SIZE;
        ffctx->sub_inode_dirty = 1;
    }

    return 0;
}

/**
 * Read a single CD sector by LBA, reconstructing the full 2352-byte
 * raw sector.  For audio sectors the dedup data is the full 2352 bytes.
 * For data sectors: prefix is generated or fetched from the CD prefix
 * tree, data is read from dedup, suffix is generated or fetched from
 * the CD suffix tree, and the subheader is restored as applicable.
 */
static int obmafs3_cd_read_long(struct fuse_file_ctx *ffctx, struct obmafs3_ioctl_cd_read_arg *arg)
{
    if(!arg) FUSE_RETURN(-EINVAL, "");

    int64_t sector_lba = arg->sector;
    if(sector_lba < 0 || (uint64_t)sector_lba >= ffctx->inode.sector_count) FUSE_RETURN(-EINVAL, "");

    /* Read the cd_sector_map_entry for this sector from inode data */
    struct inode_record map_inode;
    memcpy(&map_inode, &ffctx->inode, sizeof(map_inode));
    map_inode.file_size = ffctx->inode.sector_map_size * sizeof(struct cd_sector_map_entry);

    struct cd_sector_map_entry sme;
    uint64_t                   sme_offset = (uint64_t)sector_lba * sizeof(struct cd_sector_map_entry);
    int                        rc         = obmafs3_read_file_data(g_ctx, &map_inode, sme_offset, &sme, sizeof(sme));
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    uint8_t *out = arg->buffer;
    memset(out, 0, CD_RAW_SECTOR_SIZE);

    /* --- Audio: dedup data IS the full 2352 bytes --- */
    if(sme.sector_mode == kCdSectorModeAudio)
    {
        /* Look up and read the 2352-byte sector from dedup */
        struct btree_header dedup_hdr;
        uint64_t            dedup_hdr_lba;
        rc = obmafs3_dedup_get_tree(g_ctx, CD_RAW_SECTOR_SIZE, &dedup_hdr, &dedup_hdr_lba);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

        rc = obmafs3_read_media_image_data(g_ctx, &ffctx->inode, (uint64_t)sector_lba * CD_RAW_SECTOR_SIZE, out,
                                           CD_RAW_SECTOR_SIZE, CD_RAW_SECTOR_SIZE);
        return (rc == OBMAFS3_OK) ? 0 : -EIO;
    }

    /* --- Data modes --- */

    /* Determine data portion size */
    uint16_t data_size;
    int      has_subheader = 0;
    switch((enum obmafs3_cd_sector_mode)sme.sector_mode)
    {
        case kCdSectorMode1:
            data_size = CD_DATA_SIZE; /* 2048 */
            break;
        case kCdSectorMode2:
            data_size = 2336;
            break;
        case kCdSectorMode2Form1:
            data_size     = CD_DATA_SIZE; /* 2048 */
            has_subheader = 1;
            break;
        case kCdSectorMode2Form2:
            data_size     = 2328;
            has_subheader = 1;
            break;
        default:
            FUSE_RETURN(-EINVAL, "");
    }

    /* 1. Reconstruct prefix (bytes 0-15) */
    if(sme.generated_prefix)
    {
        /* Generate sync + MSF + mode from LBA */
        ecc_cd_reconstruct_prefix(out, sme.sector_mode, sector_lba);
    }
    else
    {
        /* Fetch stored prefix from the CD prefix tree */
        uint8_t pfx[CD_PREFIX_DATA_SIZE];
        rc = obmafs3_cd_prefix_get(g_ctx, sme.prefix_hash, pfx);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
        memcpy(out, pfx, CD_PREFIX_SIZE);
    }

    /* 2. Restore subheader (bytes 16-23) for Mode 2 variants */
    if(has_subheader)
    {
        /* subheader[8] contains both the subheader and its copy */
        memcpy(out + CD_PREFIX_SIZE, sme.subheader, 4);
        memcpy(out + CD_PREFIX_SIZE + 4, sme.subheader + 4, 4);
    }

    /* 3. Read data portion from dedup */
    {
        int data_offset_in_sector;
        if(has_subheader)
            data_offset_in_sector = CD_PREFIX_SIZE + 8; /* after prefix + subheader */
        else if(sme.sector_mode == kCdSectorMode2)
            data_offset_in_sector = CD_PREFIX_SIZE; /* Mode 2 raw: data starts after prefix */
        else
            data_offset_in_sector = CD_PREFIX_SIZE; /* Mode 1: data starts after prefix */

        /* Read from media image data path using the data portion's
         * sector size for dedup tree lookup */
        struct inode_record data_inode;
        memcpy(&data_inode, &ffctx->inode, sizeof(data_inode));
        /* The data was stored with offset = sector_lba * data_size */
        rc = obmafs3_read_media_image_data(g_ctx, &data_inode, (uint64_t)sector_lba * data_size,
                                           out + data_offset_in_sector, data_size, data_size);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
    }

    /* 4. Reconstruct suffix (last 288 bytes, position 2064-2351) */
    if(sme.sector_mode == kCdSectorMode2) { /* Raw Mode 2 has no suffix — all 2336 bytes after prefix are data */ }
    else if(sme.generated_suffix)
    {
        /* Generate EDC/ECC from the data using ecc_cd facilities */
        if(!ffctx->ecc_ctx)
        {
            ffctx->ecc_ctx = ecc_cd_init();
            if(!ffctx->ecc_ctx) FUSE_RETURN(-ENOMEM, "");
        }
        ecc_cd_reconstruct(ffctx->ecc_ctx, out, sme.sector_mode);
    }
    else
    {
        /* Fetch stored suffix from the CD suffix tree */
        uint8_t sfx[CD_SUFFIX_DATA_SIZE];
        rc = obmafs3_cd_suffix_get(g_ctx, sme.suffix_hash, sfx);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
        memcpy(out + CD_RAW_SECTOR_SIZE - CD_SUFFIX_SIZE, sfx, CD_SUFFIX_SIZE);
    }

    return 0;
}

/**
 * Read a single CD sector by LBA, returning 2352 raw bytes + 96 subchannel.
 * If subchannel is not available, the last 96 bytes are filled with zeros.
 */
static int obmafs3_cd_read_long_sub(struct fuse_file_ctx *ffctx, struct obmafs3_ioctl_cd_read_full_arg *arg)
{
    if(!arg) FUSE_RETURN(-EINVAL, "");

    /* Reuse the read-long handler for the first 2352 bytes */
    struct obmafs3_ioctl_cd_read_arg rd;
    rd.sector = arg->sector;
    int rc    = obmafs3_cd_read_long(ffctx, &rd);
    if(rc != 0) return rc;

    memcpy(arg->buffer, rd.buffer, CD_RAW_SECTOR_SIZE);

    /* Read the cd_sector_map_entry to get subchannel_hash */
    struct inode_record map_inode;
    memcpy(&map_inode, &ffctx->inode, sizeof(map_inode));
    map_inode.file_size = ffctx->inode.sector_map_size * sizeof(struct cd_sector_map_entry);

    struct cd_sector_map_entry sme;
    uint64_t                   sme_offset = (uint64_t)arg->sector * sizeof(struct cd_sector_map_entry);
    rc                                    = obmafs3_read_file_data(g_ctx, &map_inode, sme_offset, &sme, sizeof(sme));
    if(rc != OBMAFS3_OK)
    {
        memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
        return 0;
    }

    if(sme.subchannel_hash != 0)
    {
        uint8_t sub[CD_SUBCHANNEL_DATA_SIZE];
        rc = obmafs3_cd_subchannel_get(g_ctx, sme.subchannel_hash, sub);
        if(rc == OBMAFS3_OK) { memcpy(arg->buffer + CD_RAW_SECTOR_SIZE, sub, CD_SUBCHANNEL_SIZE); }
        else
        {
            memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
        }
    }
    else
    {
        memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main ioctl dispatcher                                              */
/* ------------------------------------------------------------------ */

/**
 * FUSE callback: handle ioctl commands.
 *
 * Dispatches ioctl requests for media tag get/set, CD image operations
 * (set CD image, write long, read long, read long with subchannel),
 * and metadata operations (get, set, delete, list, query).
 *
 * @param path   File path (unused).
 * @param cmd    Ioctl command number.
 * @param arg    Ioctl argument (unused — data is used instead).
 * @param fi     FUSE file info with the per-file context.
 * @param flags  Ioctl flags (unused).
 * @param data   Pointer to the ioctl data structure.
 * @return 0 on success, negative errno on failure.
 */
static int obmafs3_fuse_ioctl_impl(const char *path, unsigned int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags,
                       void *data)
{
    (void)arg;
    (void)flags;

    struct fuse_file_ctx *ffctx = fi ? (struct fuse_file_ctx *)(uintptr_t)fi->fh : NULL;
    if(!ffctx) FUSE_RETURN(-EBADF, "");

    switch(cmd)
    {

            /* ---- media tag ioctls (media image files only) ---- */

        case OBMAFS3_IOC_SET_MEDIA_TAG:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage)
                FUSE_RETURN(-ENOTTY, "");
            struct obmafs3_ioctl_tag_arg *tag_arg = (struct obmafs3_ioctl_tag_arg *)data;
            if(!tag_arg || tag_arg->data_length > OBMAFS3_IOC_MAX_TAG_DATA) FUSE_RETURN(-EINVAL, "");
            int rc =
                obmafs3_media_tag_put(g_ctx, ffctx->inode_id, tag_arg->tag_type, tag_arg->data, tag_arg->data_length);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_GET_MEDIA_TAG:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage) FUSE_RETURN(-ENOTTY, "");
            struct obmafs3_ioctl_tag_arg *tag_arg = (struct obmafs3_ioctl_tag_arg *)data;
            if(!tag_arg) FUSE_RETURN(-EINVAL, "");
            void    *buf;
            uint32_t length;
            int      rc = obmafs3_media_tag_get(g_ctx, ffctx->inode_id, tag_arg->tag_type, &buf, &length);
            if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENODATA, "");
            if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
            if(length > OBMAFS3_IOC_MAX_TAG_DATA)
            {
                obmafs3_media_tag_data_free(buf);
                FUSE_RETURN(-ERANGE, "");
            }
            tag_arg->data_length = length;
            memcpy(tag_arg->data, buf, length);
            obmafs3_media_tag_data_free(buf);
            return 0;
        }

            /* ---- compact disc image ioctl ---- */

        case OBMAFS3_IOC_SET_CD_IMAGE:
        {
            /* Only allow conversion of regular empty files */
            if(ffctx->inode.file_type != kFileTypeRegular) FUSE_RETURN(-ENOTTY, "");
            if(ffctx->inode.file_size != 0) FUSE_RETURN(-ENOTEMPTY, "");

            ffctx->inode.file_type       = kFileTypeCompactDiscImage;
            ffctx->inode.sector_count    = 0;
            ffctx->inode.sector_map_size = 0;

            int rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_CD_WRITE_LONG:
        {
            if(ffctx->inode.file_type != kFileTypeCompactDiscImage) FUSE_RETURN(-ENOTTY, "");
            return obmafs3_cd_write_long(ffctx, path, (const struct obmafs3_ioctl_cd_write_arg *)data);
        }

        case OBMAFS3_IOC_CD_READ_LONG:
        {
            if(ffctx->inode.file_type != kFileTypeCompactDiscImage) FUSE_RETURN(-ENOTTY, "");
            return obmafs3_cd_read_long(ffctx, (struct obmafs3_ioctl_cd_read_arg *)data);
        }

        case OBMAFS3_IOC_CD_READ_LONG_SUB:
        {
            if(ffctx->inode.file_type != kFileTypeCompactDiscImage) FUSE_RETURN(-ENOTTY, "");
            return obmafs3_cd_read_long_sub(ffctx, (struct obmafs3_ioctl_cd_read_full_arg *)data);
        }

            /* ---- Image metadata ioctls ---- */

        case OBMAFS3_IOC_SET_METADATA:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage)
                FUSE_RETURN(-ENOTTY, "");
            const struct obmafs3_ioctl_metadata_set_arg *sa = (const struct obmafs3_ioctl_metadata_set_arg *)data;
            int rc = obmafs3_metadata_put(g_ctx, ffctx->inode.inode_id, sa->key, sa->value);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_GET_METADATA:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage)
                FUSE_RETURN(-ENOTTY, "");
            struct obmafs3_ioctl_metadata_get_arg *ga = (struct obmafs3_ioctl_metadata_get_arg *)data;
            int rc = obmafs3_metadata_get(g_ctx, ffctx->inode.inode_id, ga->key, ga->value, METADATA_VALUE_MAX);
            if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENODATA, "");
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_DELETE_METADATA:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage)
                FUSE_RETURN(-ENOTTY, "");
            const struct obmafs3_ioctl_metadata_delete_arg *da = (const struct obmafs3_ioctl_metadata_delete_arg *)data;
            int rc = obmafs3_metadata_delete(g_ctx, ffctx->inode.inode_id, da->key);
            if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENODATA, "");
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_LIST_METADATA:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage)
                FUSE_RETURN(-ENOTTY, "");
            struct obmafs3_ioctl_metadata_list_arg *la = (struct obmafs3_ioctl_metadata_list_arg *)data;
            char                                  **keys;
            uint32_t                                total;
            int rc = obmafs3_metadata_list(g_ctx, ffctx->inode.inode_id, &keys, &total);
            if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
            uint32_t start = la->offset;
            uint32_t n     = 0;
            memset(la->keys, 0, sizeof(la->keys));
            for(uint32_t i = start; i < total && n < 16; i++, n++) strncpy(la->keys[n], keys[i], METADATA_KEY_MAX - 1);
            la->count = n;
            obmafs3_metadata_list_free(keys, total);
            return 0;
        }

        case OBMAFS3_IOC_QUERY_METADATA:
        {
            /* This query is filesystem-level; works on any open image file */
            struct obmafs3_ioctl_metadata_query_arg *qa = (struct obmafs3_ioctl_metadata_query_arg *)data;
            char                                   **paths;
            uint32_t                                 total;
            int rc = obmafs3_metadata_query(g_ctx, qa->key, qa->value, &paths, &total);
            if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
            uint32_t start = qa->offset;
            uint32_t n     = 0;
            memset(qa->paths, 0, sizeof(qa->paths));
            for(uint32_t i = start; i < total && n < METADATA_QUERY_MAX_RESULTS; i++, n++)
                strncpy(qa->paths[n], paths[i], METADATA_QUERY_PATH_MAX - 1);
            qa->count = n;
            obmafs3_metadata_query_free(paths, total);
            return 0;
        }

            /* ---- media image type conversion ioctl ---- */

        case OBMAFS3_IOC_SET_MEDIA_IMAGE:
        {
            /* Only allow conversion of empty regular files */
            if(ffctx->inode.file_type != kFileTypeRegular) FUSE_RETURN(-ENOTTY, "");
            if(ffctx->inode.file_size != 0) FUSE_RETURN(-ENOTEMPTY, "");
            const struct obmafs3_ioctl_set_media_image_arg *mia =
                (const struct obmafs3_ioctl_set_media_image_arg *)data;
            if(!mia || mia->sector_size == 0) FUSE_RETURN(-EINVAL, "");

            ffctx->inode.file_type    = kFileTypeMediaImage;
            ffctx->inode.sector_count = 0;
            ffctx->sector_size        = mia->sector_size;

            int rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        default:
            FUSE_RETURN(-ENOTTY, "");
    }
}

/* ------------------------------------------------------------------ */
/*  Thread-safe wrapper — serialise ioctl callback                     */
/* ------------------------------------------------------------------ */

int obmafs3_fuse_ioctl(const char *path, unsigned int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags,
                       void *data)
{
    /* CD_WRITE_LONG manages its own fine-grained locking inside
     * obmafs3_cd_write_long — skip the blanket serialisation so
     * writes to different CD images can overlap their hashing and
     * ECC computations. */
    if(cmd == OBMAFS3_IOC_CD_WRITE_LONG)
        return obmafs3_fuse_ioctl_impl(path, cmd, arg, fi, flags, data);

    pthread_rwlock_wrlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_ioctl_impl(path, cmd, arg, fi, flags, data);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}
