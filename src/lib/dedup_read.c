// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : dedup_read.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Media image read path, CD image read path, and CD sector map cache flush/free.
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

#include "dedup_internal.h"

/* ------------------------------------------------------------------ */
/*  Media image read path                                              */
/* ------------------------------------------------------------------ */

/**
 * Read data from a media image file.
 *
 * The inode's data blocks store sector_map_entry records that map
 * each logical sector to a hash.  The hash is used to look up the
 * actual sector data in the dedup tree.
 *
 * @param ctx         Filesystem context.
 * @param inode       Inode of the media image file.
 * @param offset      Byte offset in the original file.
 * @param buf         Output buffer.
 * @param size        Number of bytes to read.
 * @param sector_size Size of each sector in the image.
 * @return OBMAFS3_OK on success, error code otherwise.
 */
int obmafs3_read_media_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                                  size_t size, uint16_t sector_size)
{
    if(offset >= inode->file_size) return OBMAFS3_OK;

    if(offset + size > inode->file_size) size = (size_t)(inode->file_size - offset);

    if(size == 0) return OBMAFS3_OK;

    /* Get the dedup tree for this sector size */
    struct btree_header dedup_hdr;
    uint64_t            dedup_hdr_lba;
    int                 rc = obmafs3_dedup_get_tree(ctx, sector_size, &dedup_hdr, &dedup_hdr_lba);
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr, "[read_media_image] dedup_get_tree FAILED rc=%d ss=%u\n", rc, sector_size);
        return rc;
    }

    /* ---- Batch-read all needed sector_map_entries in one call ----
     *
     * A single 4 KiB block holds 4096/18 = 227 entries.  The old code
     * re-read the same block for every sector — thousands of pread
     * syscalls for a typical 1 MiB FUSE read.  Pre-reading the whole
     * range collapses that to a handful of block reads. */
    int64_t  first_sector = (int64_t)(offset / sector_size);
    int64_t  last_sector  = (int64_t)((offset + size - 1) / sector_size);
    uint64_t sme_count    = (uint64_t)(last_sector - first_sector + 1);

    struct sector_map_entry *sme_batch = malloc((size_t)(sme_count * sizeof(struct sector_map_entry)));
    if(!sme_batch) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

    /* Temporary inode copy for reading sector map (need to adjust file_size) */
    struct inode_record map_inode;
    memcpy(&map_inode, inode, sizeof(map_inode));
    map_inode.file_size = inode->sector_map_size * sizeof(struct sector_map_entry);

    uint64_t sme_offset = (uint64_t)first_sector * sizeof(struct sector_map_entry);
    rc                  = obmafs3_read_file_data(ctx, &map_inode, sme_offset, sme_batch,
                                                 (size_t)(sme_count * sizeof(struct sector_map_entry)));
    if(rc != OBMAFS3_OK)
    {
        fprintf(stderr,
                "[read_media_image] batch read_file_data(sme) FAILED rc=%d "
                "first_sector=%" PRId64 " count=%" PRIu64 " sme_offset=%" PRIu64 " map_file_size=%" PRIu64
                " sector_map_size=%" PRIu64 " inode=%" PRIu64 "\n",
                rc, first_sector, sme_count, sme_offset, map_inode.file_size, inode->sector_map_size, inode->inode_id);
        free(sme_batch);
        return rc;
    }

    /* Buffer for reading the dedup data block (dedup_block_size bytes) */
    uint8_t *dedup_buf = malloc((size_t)ctx->sb.dedup_block_size);
    if(!dedup_buf)
    {
        free(sme_batch);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }

    /* Decompressed payload buffer (allocated on first compressed block) */
    uint8_t *decomp_buf = NULL;

    /* Cache the last read dedup block LBA to avoid re-reading */
    uint64_t cached_dedup_lba  = 0;
    int      cached_compressed = 0;

    /* Leaf-level lookup cache: avoids full tree traversal when
     * consecutive sector hashes land in the same B+Tree leaf. */
    struct dedup_leaf_cache leaf_cache = DEDUP_LEAF_CACHE_INIT;

    uint8_t *out        = (uint8_t *)buf;
    size_t   bytes_read = 0;

    while(bytes_read < size)
    {
        uint64_t read_pos         = offset + bytes_read;
        int64_t  sector_num       = (int64_t)(read_pos / sector_size);
        size_t   offset_in_sector = (size_t)(read_pos % sector_size);

        /* How many bytes remain in this sector */
        size_t remaining_in_sector = (size_t)sector_size - offset_in_sector;
        size_t remaining_in_read   = size - bytes_read;
        size_t chunk               = remaining_in_read < remaining_in_sector ? remaining_in_read : remaining_in_sector;

        /* Index into the pre-fetched batch */
        uint64_t                 sme_idx = (uint64_t)(sector_num - first_sector);
        struct sector_map_entry *sme     = &sme_batch[sme_idx];

        /* Look up the hash in the dedup tree (using leaf cache) */
        struct dedup_entry de;
        rc = dedup_lookup_cached(ctx, &dedup_hdr, sme->hash, &de, &leaf_cache);
        if(rc != OBMAFS3_OK)
        {
            fprintf(stderr,
                    "[read_media_image] dedup_lookup FAILED rc=%d hash=%" PRIu64 " sector=%" PRId64 " inode=%" PRIu64
                    "\n",
                    rc, sme->hash, sector_num, inode->inode_id);
            free(leaf_cache.leaf_buf);
            free(sme_batch);
            free(decomp_buf);
            free(dedup_buf);
            return rc;
        }

        /* Read the dedup data block if not already cached */
        if(de.block_lba != cached_dedup_lba)
        {
            /* Read first standard block to get the header */
            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK)
            {
                fprintf(stderr,
                        "[read_media_image] block_read(dedup hdr) FAILED rc=%d lba=%" PRIu64 " hash=%" PRIu64
                        " sector=%" PRId64 "\n",
                        rc, de.block_lba, sme->hash, sector_num);
                free(leaf_cache.leaf_buf);
                free(sme_batch);
                free(decomp_buf);
                free(dedup_buf);
                return rc;
            }

            /* Check if the block is compressed */
            struct block_header bhdr;
            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

            /* Determine actual on-disk payload size and read remaining */
            uint64_t payload_size;
            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
                payload_size = bhdr.compressed_size;
            else
                payload_size = bhdr.original_size;

            uint64_t total_on_disk = sizeof(bhdr) + payload_size;
            uint64_t bs            = ctx->sb.block_size;
            uint64_t needed_std    = (total_on_disk + bs - 1) / bs;

            /* Read remaining standard blocks beyond the first */
            if(needed_std > 1)
            {
                rc = obmafs3_block_read(ctx, de.block_lba + 1, dedup_buf + bs, (size_t)((needed_std - 1) * bs));
                if(rc != OBMAFS3_OK)
                {
                    free(leaf_cache.leaf_buf);
                    free(sme_batch);
                    free(decomp_buf);
                    free(dedup_buf);
                    return rc;
                }
            }

            cached_dedup_lba = de.block_lba;

            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                if(!decomp_buf)
                {
                    decomp_buf = malloc((size_t)ctx->sb.dedup_block_size);
                    if(!decomp_buf)
                    {
                        free(leaf_cache.leaf_buf);
                        free(sme_batch);
                        free(dedup_buf);
                        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
                    }
                }
                rc = obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, dedup_buf + sizeof(bhdr),
                                        (size_t)bhdr.compressed_size, decomp_buf, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK)
                {
                    free(leaf_cache.leaf_buf);
                    free(sme_batch);
                    free(decomp_buf);
                    free(dedup_buf);
                    return rc;
                }
                cached_compressed = 1;
            }
            else
            {
                cached_compressed = 0;
            }

            /* Speculatively prefetch the next sector's dedup block.
             * Uses the leaf cache so the lookup is typically free. */
            if(sme_idx + 1 < sme_count)
                dedup_readahead_next(ctx, &dedup_hdr, sme_batch[sme_idx + 1].hash, cached_dedup_lba, &leaf_cache);
        }

        /* Copy sector data from the dedup block at the stored offset.
         * block_offset includes the header prefix; for compressed blocks
         * decomp_buf holds only the payload so subtract the header. */
        if(cached_compressed)
        {
            size_t decomp_off = de.block_offset - sizeof(struct block_header);
            memcpy(out + bytes_read, decomp_buf + decomp_off + offset_in_sector, chunk);
        }
        else
        {
            memcpy(out + bytes_read, dedup_buf + de.block_offset + offset_in_sector, chunk);
        }
        bytes_read += chunk;
    }

    free(leaf_cache.leaf_buf);
    free(sme_batch);
    free(decomp_buf);
    free(dedup_buf);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  CD compact disc image read path                                    */
/* ------------------------------------------------------------------ */

/**
 * Read data from a compact disc image, reconstructing full 2352-byte
 * raw sectors from dedup, prefix/suffix B+Trees, and the CD sector map.
 *
 * This is the CD equivalent of @c obmafs3_read_media_image_data.
 * All needed @c cd_sector_map_entry records are batch-read in a single
 * call to @c obmafs3_read_file_data, eliminating per-sector I/O for
 * the map.  Each sector is then reconstructed from:
 *
 *  - Audio mode: full 2352 bytes from the 2352-byte dedup tree.
 *  - Data modes: prefix (generated or from prefix tree) + subheader
 *    (if applicable) + user data (from appropriately-sized dedup tree)
 *    + suffix (generated or from suffix tree).
 *
 * The virtual file size is @c sector_count * @c CD_RAW_SECTOR_SIZE.
 * Sectors that do not have a corresponding entry in the sector map
 * (gaps between tracks) are returned as zero-filled 2352-byte buffers.
 * Reading beyond @c sector_count is not allowed (caller must clamp).
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode record describing the CD image file.
 * @param offset Byte offset into the virtual 2352-byte-per-sector image.
 * @param buf    Output buffer.
 * @param size   Number of bytes to read.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_read_cd_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf,
                               size_t size)
{
    /* Virtual file size: sector_count * 2352 */
    uint64_t virtual_size = inode->sector_count * CD_RAW_SECTOR_SIZE;
    if(offset >= virtual_size) return OBMAFS3_OK;
    if(offset + size > virtual_size) size = (size_t)(virtual_size - offset);
    if(size == 0) return OBMAFS3_OK;

    /* ---- Read ALL cd_sector_map_entries (sparse — indexed by sector field) ---- */
    uint64_t total_entries = inode->sector_map_size;

    struct cd_sector_map_entry *sme_all = NULL;
    if(total_entries > 0)
    {
        sme_all = malloc((size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(!sme_all) DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");

        struct inode_record map_inode;
        memcpy(&map_inode, inode, sizeof(map_inode));
        map_inode.file_size = total_entries * sizeof(struct cd_sector_map_entry);

        int rc = obmafs3_read_file_data(ctx, &map_inode, 0, sme_all,
                                        (size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(rc != OBMAFS3_OK)
        {
            free(sme_all);
            return rc;
        }
    }

    /* ECC context for suffix reconstruction — lazy-allocated on first use */
    void *ecc_ctx = NULL;

    /* Dedup block cache — shared across all sectors in this read */
    uint8_t *dedup_buf = malloc((size_t)ctx->sb.dedup_block_size);
    if(!dedup_buf)
    {
        free(sme_all);
        DBG_RETURN(OBMAFS3_ERR_NOMEM, "out of memory");
    }
    uint8_t *decomp_buf        = NULL;
    uint64_t cached_dedup_lba  = 0;
    int      cached_compressed = 0;

    /* Leaf-level lookup cache — amortises tree traversals across sectors */
    struct dedup_leaf_cache leaf_cache = DEDUP_LEAF_CACHE_INIT;

    /* Cached dedup tree header (changes when the sector's data_size changes) */
    struct btree_header cached_dedup_hdr;
    uint16_t            cached_data_size = 0;

    int      rc         = OBMAFS3_OK;
    uint8_t *out        = (uint8_t *)buf;
    size_t   bytes_read = 0;

    while(bytes_read < size)
    {
        uint64_t read_pos         = offset + bytes_read;
        int64_t  sector_num       = (int64_t)(read_pos / CD_RAW_SECTOR_SIZE);
        size_t   offset_in_sector = (size_t)(read_pos % CD_RAW_SECTOR_SIZE);

        size_t remaining_in_sector = CD_RAW_SECTOR_SIZE - offset_in_sector;
        size_t remaining_in_read   = size - bytes_read;
        size_t chunk               = remaining_in_read < remaining_in_sector ? remaining_in_read : remaining_in_sector;

        /* Binary search the sector map for this LBA.  Entries are
         * sorted by sector number (written in track order). */
        struct cd_sector_map_entry *sme = NULL;
        if(sme_all && total_entries > 0)
        {
            int64_t lo = 0, hi = (int64_t)total_entries - 1;
            while(lo <= hi)
            {
                int64_t mid = lo + (hi - lo) / 2;
                if(sme_all[mid].sector == sector_num)
                {
                    sme = &sme_all[mid];
                    break;
                }
                else if(sme_all[mid].sector < sector_num)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
        }

        /* Sector not found in the map — gap between tracks.
         * Return a zero-filled 2352-byte buffer. */
        if(!sme)
        {
            uint8_t zero_sector[CD_RAW_SECTOR_SIZE];
            memset(zero_sector, 0, CD_RAW_SECTOR_SIZE);
            memcpy(out + bytes_read, zero_sector + offset_in_sector, chunk);
            bytes_read += chunk;
            continue;
        }

        /* Determine the data_size (= dedup sector size) for this sector */
        uint16_t data_size;
        switch((enum obmafs3_cd_sector_mode)sme->sector_mode)
        {
            case kCdSectorModeAudio:
                data_size = CD_RAW_SECTOR_SIZE;
                break;
            case kCdSectorMode1:
                data_size = CD_DATA_SIZE;
                break;
            case kCdSectorMode2:
                data_size = 2336;
                break;
            case kCdSectorMode2Form1:
                data_size = CD_DATA_SIZE;
                break;
            case kCdSectorMode2Form2:
                data_size = 2328;
                break;
            default:
                rc = OBMAFS3_ERR_INVAL;
                goto fail;
        }

        /* If the dedup tree changed, get the new header and invalidate
         * the leaf cache (different tree = different leaves). */
        if(data_size != cached_data_size)
        {
            uint64_t hdr_lba;
            rc = obmafs3_dedup_get_tree(ctx, data_size, &cached_dedup_hdr, &hdr_lba);
            if(rc != OBMAFS3_OK)
            {
                fprintf(stderr, "[read_cd_image] dedup_get_tree FAILED rc=%d ss=%u\n", rc, data_size);
                goto fail;
            }
            cached_data_size = data_size;
            /* Invalidate leaf cache — it belongs to the previous tree */
            free(leaf_cache.leaf_buf);
            leaf_cache = (struct dedup_leaf_cache)DEDUP_LEAF_CACHE_INIT;
        }

        /* ---- Look up the sector's hash in the dedup tree ---- */
        struct dedup_entry de;
        rc = dedup_lookup_cached(ctx, &cached_dedup_hdr, sme->hash, &de, &leaf_cache);
        if(rc != OBMAFS3_OK)
        {
            fprintf(stderr,
                    "[read_cd_image] dedup_lookup FAILED rc=%d hash=%" PRIu64 " sector=%" PRId64 " inode=%" PRIu64 "\n",
                    rc, sme->hash, sector_num, inode->inode_id);
            goto fail;
        }

        /* ---- Read the dedup data block if not already cached ---- */
        if(de.block_lba != cached_dedup_lba)
        {
            rc = obmafs3_block_read(ctx, de.block_lba, dedup_buf, (size_t)ctx->sb.block_size);
            if(rc != OBMAFS3_OK) goto fail;

            struct block_header bhdr;
            memcpy(&bhdr, dedup_buf, sizeof(bhdr));

            uint64_t payload_size =
                (bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED) ? bhdr.compressed_size : bhdr.original_size;
            uint64_t total_on_disk = sizeof(bhdr) + payload_size;
            uint64_t bs            = ctx->sb.block_size;
            uint64_t needed_std    = (total_on_disk + bs - 1) / bs;

            if(needed_std > 1)
            {
                rc = obmafs3_block_read(ctx, de.block_lba + 1, dedup_buf + bs, (size_t)((needed_std - 1) * bs));
                if(rc != OBMAFS3_OK) goto fail;
            }

            cached_dedup_lba = de.block_lba;

            if(bhdr.flags & OBMAFS3_BLOCK_FLAG_COMPRESSED)
            {
                if(!decomp_buf)
                {
                    decomp_buf = malloc((size_t)ctx->sb.dedup_block_size);
                    if(!decomp_buf)
                    {
                        rc = OBMAFS3_ERR_NOMEM;
                        goto fail;
                    }
                }
                rc = obmafs3_decompress(obmafs3_get_thread_bufs(ctx)->zstd_dctx, dedup_buf + sizeof(bhdr),
                                        (size_t)bhdr.compressed_size, decomp_buf, (size_t)bhdr.original_size);
                if(rc != OBMAFS3_OK) goto fail;
                cached_compressed = 1;
            }
            else
            {
                cached_compressed = 0;
            }
        }

        /* ---- Reconstruct the full 2352-byte raw sector ---- */
        uint8_t sector_buf[CD_RAW_SECTOR_SIZE];
        memset(sector_buf, 0, CD_RAW_SECTOR_SIZE);

        /* Copy the data portion from the dedup block */
        const uint8_t *src_data;
        if(cached_compressed)
        {
            size_t decomp_off = de.block_offset - sizeof(struct block_header);
            src_data          = decomp_buf + decomp_off;
        }
        else
        {
            src_data = dedup_buf + de.block_offset;
        }

        if(sme->sector_mode == kCdSectorModeAudio)
        {
            /* Audio: full 2352 bytes IS the dedup data */
            memcpy(sector_buf, src_data, CD_RAW_SECTOR_SIZE);
        }
        else
        {
            int has_subheader = (sme->sector_mode == kCdSectorMode2Form1 || sme->sector_mode == kCdSectorMode2Form2);

            /* 1. Prefix (bytes 0-15) */
            if(sme->generated_prefix) { ecc_cd_reconstruct_prefix(sector_buf, sme->sector_mode, sector_num); }
            else
            {
                uint8_t pfx[CD_PREFIX_DATA_SIZE];
                rc = obmafs3_cd_prefix_get(ctx, sme->prefix_hash, pfx);
                if(rc != OBMAFS3_OK) goto fail;
                memcpy(sector_buf, pfx, CD_PREFIX_SIZE);
            }

            /* 2. Subheader (bytes 16-23) for Mode 2 variants */
            if(has_subheader)
            {
                memcpy(sector_buf + CD_PREFIX_SIZE, sme->subheader, 4);
                memcpy(sector_buf + CD_PREFIX_SIZE + 4, sme->subheader + 4, 4);
            }

            /* 3. Data portion */
            {
                int data_offset_in_sector = has_subheader ? CD_PREFIX_SIZE + 8 : CD_PREFIX_SIZE;
                memcpy(sector_buf + data_offset_in_sector, src_data, data_size);
            }

            /* 4. Suffix */
            if(sme->sector_mode == kCdSectorMode2) { /* Raw Mode 2 has no suffix */ }
            else if(sme->generated_suffix)
            {
                if(!ecc_ctx)
                {
                    ecc_ctx = ecc_cd_init();
                    if(!ecc_ctx)
                    {
                        rc = OBMAFS3_ERR_NOMEM;
                        goto fail;
                    }
                }
                ecc_cd_reconstruct(ecc_ctx, sector_buf, sme->sector_mode);
            }
            else
            {
                uint8_t sfx[CD_SUFFIX_DATA_SIZE];
                rc = obmafs3_cd_suffix_get(ctx, sme->suffix_hash, sfx);
                if(rc != OBMAFS3_OK) goto fail;
                memcpy(sector_buf + CD_RAW_SECTOR_SIZE - CD_SUFFIX_SIZE, sfx, CD_SUFFIX_SIZE);
            }
        }

        memcpy(out + bytes_read, sector_buf + offset_in_sector, chunk);
        bytes_read += chunk;
    }

    ecc_cd_free(ecc_ctx);
    free(leaf_cache.leaf_buf);
    free(decomp_buf);
    free(dedup_buf);
    free(sme_all);
    return OBMAFS3_OK;

fail:
    ecc_cd_free(ecc_ctx);
    free(leaf_cache.leaf_buf);
    free(decomp_buf);
    free(dedup_buf);
    free(sme_all);
    return rc;
}

/**
 * Read subchannel data from a kFileTypeSubchannelFile.
 *
 * The subchannel file has no data of its own.  Its @c sector_count field
 * stores the parent CD image's inode_id.  This function loads the parent
 * inode, reads its CD sector map, and for each requested 96-byte sector
 * offset, looks up the subchannel hash via binary search and fetches the
 * 96-byte subchannel data from the CD subchannel B+Tree.  Sectors
 * without subchannel data are zero-filled.
 *
 * @param ctx        Filesystem context.
 * @param sub_inode  Inode of the subchannel sidecar file.
 * @param offset     Byte offset into the virtual subchannel stream.
 * @param buf        Output buffer.
 * @param size       Number of bytes to read.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_read_subchannel_data(struct obmafs3_ctx *ctx, const struct inode_record *sub_inode, uint64_t offset,
                                 void *buf, size_t size)
{
    /* The parent CD image inode_id is stored in sector_count */
    uint64_t parent_inode_id = sub_inode->sector_count;

    struct inode_record parent_inode;
    int                 rc = obmafs3_inode_get(ctx, parent_inode_id, &parent_inode);
    if(rc != OBMAFS3_OK) return rc;

    /* Virtual size: parent's sector_count * 96 */
    uint64_t virtual_size = parent_inode.sector_count * CD_SUBCHANNEL_SIZE;
    if(offset >= virtual_size) return OBMAFS3_OK;
    if(offset + size > virtual_size) size = (size_t)(virtual_size - offset);
    if(size == 0) return OBMAFS3_OK;

    /* Read all cd_sector_map_entries from the parent */
    uint64_t                    total_entries = parent_inode.sector_map_size;
    struct cd_sector_map_entry *sme_all       = NULL;
    if(total_entries > 0)
    {
        sme_all = malloc((size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(!sme_all) return OBMAFS3_ERR_NOMEM;

        struct inode_record map_inode;
        memcpy(&map_inode, &parent_inode, sizeof(map_inode));
        map_inode.file_size = total_entries * sizeof(struct cd_sector_map_entry);

        rc = obmafs3_read_file_data(ctx, &map_inode, 0, sme_all,
                                    (size_t)(total_entries * sizeof(struct cd_sector_map_entry)));
        if(rc != OBMAFS3_OK)
        {
            free(sme_all);
            return rc;
        }
    }

    uint8_t *out        = (uint8_t *)buf;
    size_t   bytes_read = 0;

    while(bytes_read < size)
    {
        uint64_t read_pos         = offset + bytes_read;
        int64_t  sector_num       = (int64_t)(read_pos / CD_SUBCHANNEL_SIZE);
        size_t   offset_in_sector = (size_t)(read_pos % CD_SUBCHANNEL_SIZE);

        size_t remaining_in_sector = CD_SUBCHANNEL_SIZE - offset_in_sector;
        size_t remaining_in_read   = size - bytes_read;
        size_t chunk               = remaining_in_read < remaining_in_sector ? remaining_in_read : remaining_in_sector;

        /* Binary search for this sector number */
        uint64_t subchannel_hash = 0;
        if(sme_all && total_entries > 0)
        {
            int64_t lo = 0, hi = (int64_t)total_entries - 1;
            while(lo <= hi)
            {
                int64_t mid = lo + (hi - lo) / 2;
                if(sme_all[mid].sector == sector_num)
                {
                    subchannel_hash = sme_all[mid].subchannel_hash;
                    break;
                }
                else if(sme_all[mid].sector < sector_num)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
        }

        /* Fetch subchannel data or zero-fill */
        uint8_t sub_sector[CD_SUBCHANNEL_DATA_SIZE];
        if(subchannel_hash != 0)
        {
            rc = obmafs3_cd_subchannel_get(ctx, subchannel_hash, sub_sector);
            if(rc != OBMAFS3_OK) memset(sub_sector, 0, CD_SUBCHANNEL_DATA_SIZE);
        }
        else
        {
            memset(sub_sector, 0, CD_SUBCHANNEL_DATA_SIZE);
        }

        memcpy(out + bytes_read, sub_sector + offset_in_sector, chunk);
        bytes_read += chunk;
    }

    free(sme_all);
    return OBMAFS3_OK;
}

/* ------------------------------------------------------------------ */
/*  CD sector map cache flush / free                                   */
/* ------------------------------------------------------------------ */

/**
 * Write a batch of CD sector map entries to the inode's data blocks.
 *
 * Appends @p count entries at the current @c sector_map_size offset
 * and advances the map size accordingly.
 *
 * @param ctx      Filesystem context.
 * @param inode    Inode record to update (modified in place).
 * @param entries  Array of CD sector map entries to write.
 * @param count    Number of entries.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
static int write_cd_sector_map_batch(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                     const struct cd_sector_map_entry *entries, uint64_t count)
{
    if(count == 0) return OBMAFS3_OK;

    size_t   entry_size  = sizeof(struct cd_sector_map_entry);
    uint64_t map_offset  = inode->sector_map_size * entry_size;
    size_t   total_bytes = (size_t)(count * entry_size);

    uint64_t saved_file_size = inode->file_size;
    inode->file_size         = map_offset;

    int rc = obmafs3_write_file_data(ctx, inode, map_offset, entries, total_bytes);

    inode->file_size = saved_file_size;

    if(rc == OBMAFS3_OK) inode->sector_map_size += count;

    return rc;
}

/**
 * Flush cached CD sector map entries to disk.
 *
 * Writes all accumulated @c cd_sector_map_entry records from @p cache
 * to disk and resets the cache count.
 *
 * @param ctx    Filesystem context.
 * @param inode  Inode record to update.
 * @param cache  CD sector map cache to flush.
 * @return @c OBMAFS3_OK on success, or an error code on failure.
 */
int obmafs3_flush_cd_sector_map_cache(struct obmafs3_ctx *ctx, struct inode_record *inode,
                                      struct cd_sector_map_cache *cache)
{
    if(!cache || cache->count == 0) return OBMAFS3_OK;

    int rc = write_cd_sector_map_batch(ctx, inode, cache->entries, cache->count);
    if(rc == OBMAFS3_OK) { cache->count = 0; }
    return rc;
}

/**
 * Free all resources held by a CD sector map cache.
 *
 * @param cache  CD sector map cache to free.
 */
void obmafs3_free_cd_sector_map_cache(struct cd_sector_map_cache *cache)
{
    if(!cache) return;
    free(cache->entries);
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}
