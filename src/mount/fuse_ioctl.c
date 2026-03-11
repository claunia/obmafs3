// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fuse_ioctl.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : FUSE interface for OBMAFS3
//
// --[ Description ] ----------------------------------------------------------
//
//     FUSE ioctl operations for OBMAFS3.
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

/*
 * Implements: media tag, CD image, media image, and metadata ioctls
 */

#include "debug.h"
#include "fuse_ops_internal.h"
#include "obmafs3_ioctl.h"

#include "defs.h"
#include "obmafs.h"

#include <inttypes.h>
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
        uint64_t sub_leaf_lba = 0, sub_rec_offset = 0;
        uint8_t  existing[CD_SUBCHANNEL_DATA_SIZE];
        int      rc = obmafs3_cd_subchannel_get_location(g_ctx, sub_hash, existing, &sub_leaf_lba, &sub_rec_offset);
        if(rc == OBMAFS3_ERR_NOTFOUND)
        {
            rc = obmafs3_cd_subchannel_put(g_ctx, sub_hash, sub);
            if(rc != OBMAFS3_OK)
            {
                pthread_rwlock_unlock(&g_ctx->tree_lock);
                FUSE_RETURN(-EIO, "");
            }
            /* Retrieve the leaf location of the newly stored record */
            int lrc = obmafs3_cd_subchannel_get_location(g_ctx, sub_hash, NULL, &sub_leaf_lba, &sub_rec_offset);
            if(lrc != OBMAFS3_OK)
            {
                sub_leaf_lba   = 0;
                sub_rec_offset = 0;
            }
        }
        else if(rc != OBMAFS3_OK)
        {
            pthread_rwlock_unlock(&g_ctx->tree_lock);
            FUSE_RETURN(-EIO, "");
        }
        pthread_rwlock_unlock(&g_ctx->tree_lock);
        sme.dedup_subchannel_lba    = sub_leaf_lba;
        sme.dedup_subchannel_offset = sub_rec_offset;

        /* --- Create .sub sidecar file (kFileTypeSubchannelFile) on first subchannel --- */
        if(ffctx->sub_inode_id == 0 && path)
        {
            /* Build the .sub sidecar path: replace the extension with .sub */
            char sub_path[PATH_MAX];
            strncpy(sub_path, path, sizeof(sub_path) - 1);
            sub_path[sizeof(sub_path) - 1] = '\0';
            char *dot                      = strrchr(sub_path, '.');
            char *slash                    = strrchr(sub_path, '/');
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
                    struct fuse_context *fusectx = fuse_get_context();

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
                        sub_cat.directory_flag = 0;
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

        /* Bootstrap dedup block cache for 2352-byte sectors.
         * If a previous track used a different sector size, flush
         * and reinitialise so we switch to the correct dedup tree. */
        if(ffctx->db_cache.initialized && ffctx->sector_size != CD_RAW_SECTOR_SIZE)
        {
            obmafs3_flush_dedup_block_cache(g_ctx, ffctx->sector_size, &ffctx->db_cache);
            obmafs3_free_dedup_block_cache(g_ctx, &ffctx->db_cache);
        }
        if(!ffctx->db_cache.initialized) ffctx->sector_size = CD_RAW_SECTOR_SIZE;

        struct dedup_block_cache *dbc = &ffctx->db_cache;

        /* Write via the media image data path — dedup_upsert_find handles
         * both hit (skip data) and miss (store + insert) in one traversal,
         * avoiding the overhead of a separate dedup_get_tree + dedup_lookup.
         * Pass NULL cache: CD images use cd_sector_map_entries instead. */
        uint64_t offset = (uint64_t)sector_lba * CD_RAW_SECTOR_SIZE;
        int      rc     = obmafs3_write_media_image_data(g_ctx, &ffctx->inode, offset, raw, CD_RAW_SECTOR_SIZE,
                                                         CD_RAW_SECTOR_SIZE, NULL, dbc);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

        sme.dedup_sector_lba    = dbc->last_dedup_lba;
        sme.dedup_sector_offset = dbc->last_dedup_offset;

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
            if(rc != OBMAFS3_OK)
            {
                pthread_rwlock_unlock(&g_ctx->tree_lock);
                FUSE_RETURN(-EIO, "");
            }
        }
        else if(rc != OBMAFS3_OK)
        {
            pthread_rwlock_unlock(&g_ctx->tree_lock);
            FUSE_RETURN(-EIO, "");
        }
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
            if(rc != OBMAFS3_OK)
            {
                pthread_rwlock_unlock(&g_ctx->tree_lock);
                FUSE_RETURN(-EIO, "");
            }
        }
        else if(rc != OBMAFS3_OK)
        {
            pthread_rwlock_unlock(&g_ctx->tree_lock);
            FUSE_RETURN(-EIO, "");
        }
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
        /* Bootstrap dedup block cache.
         * If a previous track used a different sector size, flush
         * and reinitialise so we switch to the correct dedup tree. */
        if(ffctx->db_cache.initialized && ffctx->sector_size != data_size)
        {
            obmafs3_flush_dedup_block_cache(g_ctx, ffctx->sector_size, &ffctx->db_cache);
            obmafs3_free_dedup_block_cache(g_ctx, &ffctx->db_cache);
        }
        if(!ffctx->db_cache.initialized) ffctx->sector_size = data_size;

        struct dedup_block_cache *dbc = &ffctx->db_cache;

        /* Write via the media image data path — dedup_upsert_find handles
         * both hit (skip data) and miss (store + insert) in one traversal,
         * avoiding the overhead of a separate dedup_get_tree + dedup_lookup.
         * Pass NULL cache: CD images use cd_sector_map_entries instead. */
        uint64_t offset = (uint64_t)sector_lba * data_size;
        int      rc =
            obmafs3_write_media_image_data(g_ctx, &ffctx->inode, offset, data_ptr, data_size, data_size, NULL, dbc);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

        sme.dedup_sector_lba    = dbc->last_dedup_lba;
        sme.dedup_sector_offset = dbc->last_dedup_offset;
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
    if((uint64_t)(sector_lba + 1) > ffctx->inode.sector_count) ffctx->inode.sector_count = (uint64_t)(sector_lba + 1);
    ffctx->inode.file_size = ffctx->inode.sector_count * CD_RAW_SECTOR_SIZE;
    ffctx->inode_dirty     = 1;

    /* Keep the .sub sidecar file_size in sync with the parent's sector_count.
     * The sidecar has no data of its own — its read path fetches subchannel
     * data from the parent's cd_sector_map_entries via the subchannel B+Tree. */
    if(ffctx->sub_inode_id != 0)
    {
        ffctx->sub_inode.file_size = ffctx->inode.sector_count * CD_SUBCHANNEL_SIZE;
        ffctx->sub_inode_dirty     = 1;
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
    map_inode.file_size =
        sizeof(struct sector_map_header) + ffctx->inode.sector_map_size * sizeof(struct cd_sector_map_entry);

    struct cd_sector_map_entry sme;
    uint64_t sme_offset = sizeof(struct sector_map_header) + (uint64_t)sector_lba * sizeof(struct cd_sector_map_entry);
    int      rc         = obmafs3_read_file_data(g_ctx, &map_inode, sme_offset, &sme, sizeof(sme));
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
                                           CD_RAW_SECTOR_SIZE, CD_RAW_SECTOR_SIZE, NULL, NULL);
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
                                           out + data_offset_in_sector, data_size, data_size, NULL, NULL);
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
    map_inode.file_size =
        sizeof(struct sector_map_header) + ffctx->inode.sector_map_size * sizeof(struct cd_sector_map_entry);

    struct cd_sector_map_entry sme;
    uint64_t sme_offset = sizeof(struct sector_map_header) + (uint64_t)arg->sector * sizeof(struct cd_sector_map_entry);
    rc                  = obmafs3_read_file_data(g_ctx, &map_inode, sme_offset, &sme, sizeof(sme));
    if(rc != OBMAFS3_OK)
    {
        memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
        return 0;
    }

    if(sme.subchannel_hash != 0)
    {
        if(sme.dedup_subchannel_lba != 0)
        {
            /* Use cached leaf location — skip B+Tree traversal */
            uint8_t *leaf_buf = malloc((size_t)g_ctx->sb.block_size);
            if(leaf_buf)
            {
                rc = obmafs3_block_read(g_ctx, sme.dedup_subchannel_lba, leaf_buf, (size_t)g_ctx->sb.block_size);
                if(rc == OBMAFS3_OK)
                {
                    struct cd_subchannel_record rec;
                    memcpy(&rec, leaf_buf + sme.dedup_subchannel_offset, sizeof(rec));
                    if(rec.hash == sme.subchannel_hash)
                        memcpy(arg->buffer + CD_RAW_SECTOR_SIZE, rec.data, CD_SUBCHANNEL_SIZE);
                    else
                    {
                        /* Cached location stale — fall back to tree lookup */
                        uint8_t sub[CD_SUBCHANNEL_DATA_SIZE];
                        rc = obmafs3_cd_subchannel_get(g_ctx, sme.subchannel_hash, sub);
                        if(rc == OBMAFS3_OK)
                            memcpy(arg->buffer + CD_RAW_SECTOR_SIZE, sub, CD_SUBCHANNEL_SIZE);
                        else
                            memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
                    }
                }
                else
                {
                    memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
                }
                free(leaf_buf);
            }
            else
            {
                /* Allocation failed — fall back to tree lookup */
                uint8_t sub[CD_SUBCHANNEL_DATA_SIZE];
                rc = obmafs3_cd_subchannel_get(g_ctx, sme.subchannel_hash, sub);
                if(rc == OBMAFS3_OK)
                    memcpy(arg->buffer + CD_RAW_SECTOR_SIZE, sub, CD_SUBCHANNEL_SIZE);
                else
                    memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
            }
        }
        else
        {
            uint8_t sub[CD_SUBCHANNEL_DATA_SIZE];
            rc = obmafs3_cd_subchannel_get(g_ctx, sme.subchannel_hash, sub);
            if(rc == OBMAFS3_OK) { memcpy(arg->buffer + CD_RAW_SECTOR_SIZE, sub, CD_SUBCHANNEL_SIZE); }
            else
            {
                memset(arg->buffer + CD_RAW_SECTOR_SIZE, 0, CD_SUBCHANNEL_SIZE);
            }
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
static int obmafs3_fuse_ioctl_impl(const char *path, unsigned int cmd, void *arg, struct fuse_file_info *fi,
                                   unsigned int flags, void *data)
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
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage &&
               ffctx->inode.file_type != kFileTypeNintendo &&
               ffctx->inode.file_type != kFileTypePS3Image)
                FUSE_RETURN(-ENOTTY, "");
            struct obmafs3_ioctl_tag_arg *tag_arg = (struct obmafs3_ioctl_tag_arg *)data;
            if(!tag_arg || tag_arg->data_length > OBMAFS3_IOC_MAX_TAG_DATA) FUSE_RETURN(-EINVAL, "");
            int rc =
                obmafs3_media_tag_put(g_ctx, ffctx->inode_id, tag_arg->tag_type, tag_arg->data, tag_arg->data_length);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_GET_MEDIA_TAG:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage &&
               ffctx->inode.file_type != kFileTypeNintendo &&
               ffctx->inode.file_type != kFileTypePS3Image)
                FUSE_RETURN(-ENOTTY, "");
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
            /* Only allow conversion of empty files */
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
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage &&
               ffctx->inode.file_type != kFileTypeNintendo &&
               ffctx->inode.file_type != kFileTypePS3Image)
                FUSE_RETURN(-ENOTTY, "");
            const struct obmafs3_ioctl_metadata_set_arg *sa = (const struct obmafs3_ioctl_metadata_set_arg *)data;
            int rc = obmafs3_metadata_put(g_ctx, ffctx->inode.inode_id, sa->key, sa->value);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_GET_METADATA:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage &&
               ffctx->inode.file_type != kFileTypeNintendo &&
               ffctx->inode.file_type != kFileTypePS3Image)
                FUSE_RETURN(-ENOTTY, "");
            struct obmafs3_ioctl_metadata_get_arg *ga = (struct obmafs3_ioctl_metadata_get_arg *)data;
            int rc = obmafs3_metadata_get(g_ctx, ffctx->inode.inode_id, ga->key, ga->value, METADATA_VALUE_MAX);
            if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENODATA, "");
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_DELETE_METADATA:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage &&
               ffctx->inode.file_type != kFileTypeNintendo &&
               ffctx->inode.file_type != kFileTypePS3Image)
                FUSE_RETURN(-ENOTTY, "");
            const struct obmafs3_ioctl_metadata_delete_arg *da = (const struct obmafs3_ioctl_metadata_delete_arg *)data;
            int rc = obmafs3_metadata_delete(g_ctx, ffctx->inode.inode_id, da->key);
            if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENODATA, "");
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_LIST_METADATA:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage && ffctx->inode.file_type != kFileTypeCompactDiscImage &&
               ffctx->inode.file_type != kFileTypeNintendo &&
               ffctx->inode.file_type != kFileTypePS3Image)
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
            /* Filesystem-level multi-filter query; works on any open image file */
            struct obmafs3_ioctl_metadata_query_arg *qa = (struct obmafs3_ioctl_metadata_query_arg *)data;

            if(qa->filter_count == 0 || qa->filter_count > OBMAFS3_QUERY_MAX_FILTERS) FUSE_RETURN(-EINVAL, "");
            if(qa->combine != kQueryCombineAnd && qa->combine != kQueryCombineOr) FUSE_RETURN(-EINVAL, "");

            /* Convert ioctl filters to library filters (strip padding) */
            struct obmafs3_query_filter lib_filters[OBMAFS3_QUERY_MAX_FILTERS];
            for(uint8_t f = 0; f < qa->filter_count; f++)
            {
                memcpy(lib_filters[f].key, qa->filters[f].key, METADATA_KEY_MAX);
                memcpy(lib_filters[f].value, qa->filters[f].value, METADATA_VALUE_MAX);
                lib_filters[f].op     = qa->filters[f].op;
                lib_filters[f].negate = qa->filters[f].negate ? 1 : 0;
                lib_filters[f].group  = qa->filters[f].group  ? 1 : 0;
            }

            char   **paths;
            uint32_t page_count;
            uint32_t total_count;
            /* offset == UINT32_MAX is count-only mode: skip path resolution */
            uint32_t req_limit = (qa->offset == UINT32_MAX) ? 0 : METADATA_QUERY_MAX_RESULTS;
            int      rc = obmafs3_metadata_query_filtered(g_ctx, lib_filters, qa->filter_count,
                                                          qa->combine, qa->combine1, qa->group_combine,
                                                          qa->offset, req_limit, &paths, &page_count,
                                                          &total_count);
            if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

            memset(qa->paths, 0, sizeof(qa->paths));
            for(uint32_t i = 0; i < page_count && i < METADATA_QUERY_MAX_RESULTS; i++)
                strncpy(qa->paths[i], paths[i], METADATA_QUERY_PATH_MAX - 1);
            qa->count = page_count;
            qa->total = total_count;
            obmafs3_metadata_query_free(paths, page_count);
            return 0;
        }

            /* ---- media image type conversion ioctl ---- */

        case OBMAFS3_IOC_SET_MEDIA_IMAGE:
        {
            /* Only allow conversion of empty files */
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

        case OBMAFS3_IOC_SET_SECTOR_TAG:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage) FUSE_RETURN(-ENOTTY, "");
            const struct obmafs3_ioctl_sector_tag_write_arg *sta =
                (const struct obmafs3_ioctl_sector_tag_write_arg *)data;
            if(!sta || sta->data_length > SECTOR_TAG_DATA_MAX) FUSE_RETURN(-EINVAL, "");
            int rc = obmafs3_sector_tag_put(g_ctx, ffctx->inode_id, sta->sector,
                                            sta->tag_type, sta->data, sta->data_length);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_GET_SECTOR_TAG:
        {
            if(ffctx->inode.file_type != kFileTypeMediaImage) FUSE_RETURN(-ENOTTY, "");
            struct obmafs3_ioctl_sector_tag_read_arg *sta =
                (struct obmafs3_ioctl_sector_tag_read_arg *)data;
            if(!sta) FUSE_RETURN(-EINVAL, "");
            int rc = obmafs3_sector_tag_get(g_ctx, ffctx->inode_id, sta->sector,
                                            sta->tag_type, sta->data, &sta->data_length);
            if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENODATA, "");
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

            /* ---- Nintendo disc image ioctl ---- */

        case OBMAFS3_IOC_SET_NINTENDO_IMAGE:
        {
            /* Only allow conversion of empty files */
            if(ffctx->inode.file_size != 0) FUSE_RETURN(-ENOTEMPTY, "");
            const struct obmafs3_ioctl_set_nintendo_image_arg *nia =
                (const struct obmafs3_ioctl_set_nintendo_image_arg *)data;
            if(!nia) FUSE_RETURN(-EINVAL, "");

            ffctx->inode.file_type       = kFileTypeNintendo;
            ffctx->inode.sector_count    = 0;
            ffctx->inode.sector_map_size = 0;
            ffctx->sector_size           = NGC_SECTOR_SIZE;

            /* Set the rocompat flag for Nintendo support */
            if(!(g_ctx->sb.rocompat_flags & OBMAFS3_ROCOMPAT_NINTENDO))
            {
                g_ctx->sb.rocompat_flags |= OBMAFS3_ROCOMPAT_NINTENDO;
                obmafs3_sb_write(g_ctx->fd, &g_ctx->sb);
            }

            /* Persist disc type, disc size, and partition descriptors as metadata */
            {
                char val[256];
                snprintf(val, sizeof(val), "%u", nia->disc_type);
                obmafs3_metadata_put(g_ctx, ffctx->inode_id, "__ngc_disc_type__", val);
                snprintf(val, sizeof(val), "%" PRIu64, nia->disc_size);
                obmafs3_metadata_put(g_ctx, ffctx->inode_id, "__ngc_disc_size__", val);
                snprintf(val, sizeof(val), "%u", nia->partition_count);
                obmafs3_metadata_put(g_ctx, ffctx->inode_id, "__ngc_part_count__", val);

                for(int i = 0; i < nia->partition_count && i < OBMAFS3_NGC_MAX_PARTITIONS; i++)
                {
                    char key[64];
                    snprintf(key, sizeof(key), "__ngc_part_%d_data_offset__", i);
                    snprintf(val, sizeof(val), "%" PRIu64, nia->partitions[i].data_offset);
                    obmafs3_metadata_put(g_ctx, ffctx->inode_id, key, val);

                    snprintf(key, sizeof(key), "__ngc_part_%d_data_size__", i);
                    snprintf(val, sizeof(val), "%" PRIu64, nia->partitions[i].data_size);
                    obmafs3_metadata_put(g_ctx, ffctx->inode_id, key, val);

                    snprintf(key, sizeof(key), "__ngc_part_%d_title_key__", i);
                    /* Store title key as hex string */
                    char *p = val;
                    for(int b = 0; b < 16; b++) p += sprintf(p, "%02x", nia->partitions[i].title_key[b]);
                    obmafs3_metadata_put(g_ctx, ffctx->inode_id, key, val);
                }
            }

            int rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_ADD_JUNK_ENTRY:
        {
            if(ffctx->inode.file_type != kFileTypeNintendo) FUSE_RETURN(-ENOTTY, "");
            const struct obmafs3_ioctl_add_junk_entry_arg *jea =
                (const struct obmafs3_ioctl_add_junk_entry_arg *)data;
            if(!jea || jea->length == 0) FUSE_RETURN(-EINVAL, "");

            int rc = obmafs3_junk_map_put(g_ctx, ffctx->inode_id, jea->offset, jea->length,
                                          jea->partition_index, jea->seed);
            if(rc != OBMAFS3_OK)
                fprintf(stderr, "[ioctl] junk_map_put failed: rc=%d inode=%" PRIu64 " offset=0x%" PRIx64 "\n",
                        rc, ffctx->inode_id, jea->offset);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_SET_PS3_IMAGE:
        {
            if(ffctx->inode.file_size != 0) FUSE_RETURN(-ENOTEMPTY, "");
            const struct obmafs3_ioctl_set_ps3_image_arg *pia =
                (const struct obmafs3_ioctl_set_ps3_image_arg *)data;
            if(!pia) FUSE_RETURN(-EINVAL, "");

            ffctx->inode.file_type       = kFileTypePS3Image;
            ffctx->inode.sector_count    = 0;
            ffctx->inode.sector_map_size = 0;
            ffctx->sector_size           = 2048;

            /* Set the rocompat flag for PS3 support */
            if(!(g_ctx->sb.rocompat_flags & OBMAFS3_ROCOMPAT_PS3))
            {
                g_ctx->sb.rocompat_flags |= OBMAFS3_ROCOMPAT_PS3;
                obmafs3_sb_write(g_ctx->fd, &g_ctx->sb);
            }

            /* Persist disc key, disc size, and region map as metadata */
            {
                char val[256], key[64];
                /* Disc key as hex */
                char *p = val;
                for(int b = 0; b < 16; b++) p += sprintf(p, "%02x", pia->disc_key[b]);
                obmafs3_metadata_put(g_ctx, ffctx->inode_id, "__ps3_disc_key__", val);

                snprintf(val, sizeof(val), "%" PRIu64, pia->disc_size);
                obmafs3_metadata_put(g_ctx, ffctx->inode_id, "__ps3_disc_size__", val);

                snprintf(val, sizeof(val), "%u", pia->region_count);
                obmafs3_metadata_put(g_ctx, ffctx->inode_id, "__ps3_region_count__", val);

                for(int i = 0; i < pia->region_count && i < OBMAFS3_PS3_MAX_REGIONS; i++)
                {
                    snprintf(key, sizeof(key), "__ps3_region_%d_start__", i);
                    snprintf(val, sizeof(val), "%u", pia->regions[i].start_sector);
                    obmafs3_metadata_put(g_ctx, ffctx->inode_id, key, val);

                    snprintf(key, sizeof(key), "__ps3_region_%d_end__", i);
                    snprintf(val, sizeof(val), "%u", pia->regions[i].end_sector);
                    obmafs3_metadata_put(g_ctx, ffctx->inode_id, key, val);
                }
            }

            int rc = obmafs3_inode_put(g_ctx, &ffctx->inode);
            return rc == OBMAFS3_OK ? 0 : -EIO;
        }

        case OBMAFS3_IOC_DISTINCT_METADATA:
        {
            /* Filesystem-level distinct values query */
            struct obmafs3_ioctl_metadata_distinct_arg *da =
                (struct obmafs3_ioctl_metadata_distinct_arg *)data;

            char   **vals;
            uint32_t total;
            int rc = obmafs3_metadata_distinct(g_ctx, da->key, &vals, &total);
            if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

            uint32_t start = da->offset;
            uint32_t n     = 0;
            memset(da->values, 0, sizeof(da->values));
            for(uint32_t i = start; i < total && n < METADATA_DISTINCT_MAX_RESULTS; i++, n++)
                strncpy(da->values[n], vals[i], METADATA_VALUE_MAX - 1);
            da->count = n;
            da->total = total;
            obmafs3_metadata_distinct_free(vals, total);
            return 0;
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
    if(cmd == OBMAFS3_IOC_CD_WRITE_LONG) return obmafs3_fuse_ioctl_impl(path, cmd, arg, fi, flags, data);

    pthread_rwlock_wrlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_ioctl_impl(path, cmd, arg, fi, flags, data);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}
