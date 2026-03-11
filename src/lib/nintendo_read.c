// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : nintendo_read.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — Nintendo disc image read path
//
// --[ Description ] ----------------------------------------------------------
//
//     Read path for Nintendo GameCube/Wii disc images.
//     GC: data + junk reconstruction from B+Tree seeds.
//     Wii: full reconstruction (decrypt stored data, recompute hashes,
//          re-encrypt, re-assemble groups).
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

#include "aes128.h"
#include "nintendo.h"
#include "btree.h"
#include "defs.h"
#include "obmafs.h"
#include "obmafs3_ioctl.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Partition info from metadata ---- */

struct ngc_rp { uint64_t data_offset, data_size; uint8_t title_key[16]; };
struct ngc_ri { uint8_t disc_type; uint64_t disc_size; uint16_t part_count; struct ngc_rp parts[OBMAFS3_NGC_MAX_PARTITIONS]; };

static int hex2(char c) { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; }

static int load_info(struct obmafs3_ctx *ctx, uint64_t iid, struct ngc_ri *ri)
{
    char v[256]; memset(ri,0,sizeof(*ri));
    if(obmafs3_metadata_get(ctx,iid,"__ngc_disc_type__",v,sizeof(v))!=OBMAFS3_OK) return OBMAFS3_ERR_NOTFOUND;
    ri->disc_type=(uint8_t)atoi(v);
    if(obmafs3_metadata_get(ctx,iid,"__ngc_disc_size__",v,sizeof(v))==OBMAFS3_OK) ri->disc_size=strtoull(v,NULL,10);
    if(obmafs3_metadata_get(ctx,iid,"__ngc_part_count__",v,sizeof(v))==OBMAFS3_OK) ri->part_count=(uint16_t)atoi(v);
    for(int i=0;i<ri->part_count&&i<OBMAFS3_NGC_MAX_PARTITIONS;i++){
        char k[64];
        snprintf(k,sizeof(k),"__ngc_part_%d_data_offset__",i);
        if(obmafs3_metadata_get(ctx,iid,k,v,sizeof(v))==OBMAFS3_OK) ri->parts[i].data_offset=strtoull(v,NULL,10);
        snprintf(k,sizeof(k),"__ngc_part_%d_data_size__",i);
        if(obmafs3_metadata_get(ctx,iid,k,v,sizeof(v))==OBMAFS3_OK) ri->parts[i].data_size=strtoull(v,NULL,10);
        snprintf(k,sizeof(k),"__ngc_part_%d_title_key__",i);
        if(obmafs3_metadata_get(ctx,iid,k,v,sizeof(v))==OBMAFS3_OK)
            for(int b=0;b<16&&v[b*2]&&v[b*2+1];b++){int h=hex2(v[b*2]),l=hex2(v[b*2+1]);if(h>=0&&l>=0)ri->parts[i].title_key[b]=(uint8_t)((h<<4)|l);}
    }
    return OBMAFS3_OK;
}

/* ---- Junk reconstruction helper ---- */

static void reconstruct_junk_sector(struct obmafs3_ctx *ctx, uint64_t iid, uint64_t sector_off, uint8_t *buf)
{
    struct junk_map_record jrec;
    if(obmafs3_junk_map_lookup(ctx, iid, sector_off, &jrec) == OBMAFS3_OK)
    {
        struct ngc_lfg_ctx lfg;
        uint32_t sc[NGC_LFG_SEED_SIZE];
        memcpy(sc, jrec.seed, sizeof(sc));
        ngc_lfg_set_seed(&lfg, sc);
        /* The seed's position 0 corresponds to the start of the 0x8000-aligned
         * block, not jrec.offset (which is the start of the junk region).
         * Compute advance from the block start. */
        uint64_t adv = sector_off - (jrec.offset & ~(uint64_t)0x7FFF);
        if(adv > 0) {
            uint8_t d[4096];
            while(adv > 0) { size_t s = adv > sizeof(d) ? sizeof(d) : (size_t)adv; ngc_lfg_get_bytes(&lfg, d, s); adv -= s; }
        }
        ngc_lfg_get_bytes(&lfg, buf, NGC_SECTOR_SIZE);
    }
}

/* ---- GC read ---- */

static int read_gc(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset, void *buf, size_t size)
{
    int rc = obmafs3_read_media_image_data(ctx, inode, offset, buf, size, NGC_SECTOR_SIZE, NULL, NULL);
    if(rc != OBMAFS3_OK) return rc;

    uint8_t *out = (uint8_t *)buf; uint64_t pos = offset; size_t rem = size;
    while(rem > 0) {
        uint64_t ss = (pos / NGC_SECTOR_SIZE) * NGC_SECTOR_SIZE;
        size_t   ois = (size_t)(pos - ss), ch = NGC_SECTOR_SIZE - ois;
        if(ch > rem) ch = rem;

        struct junk_map_record jrec;
        if(obmafs3_junk_map_lookup(ctx, inode->inode_id, ss, &jrec) == OBMAFS3_OK) {
            uint8_t js[NGC_SECTOR_SIZE];
            reconstruct_junk_sector(ctx, inode->inode_id, ss, js);
            memcpy(out, js + ois, ch);
        }
        out += ch; pos += ch; rem -= ch;
    }
    return OBMAFS3_OK;
}

/* ---- Wii: reconstruct one encrypted group ---- */

static int reconstruct_group(struct obmafs3_ctx *ctx, const struct inode_record *inode,
                             uint64_t partition_data_offset, uint64_t group_idx,
                             const uint8_t title_key[16], uint8_t enc_out[WII_GROUP_SIZE])
{
    uint64_t group_disc_off = partition_data_offset + group_idx * WII_GROUP_SIZE;

    /* Read the full decrypted group (hash_block + user_data) from dedup */
    uint8_t decrypted[WII_GROUP_SIZE];
    int rc = obmafs3_read_media_image_data(ctx, inode, group_disc_off, decrypted, WII_GROUP_SIZE,
                                           NGC_SECTOR_SIZE, NULL, NULL);
    if(rc != OBMAFS3_OK) return rc;

    /* Reconstruct junk in the user_data portion (bytes 0x400..0x7FFF).
     * The stored seed is a per-block seed (LFG state at the start of
     * the 0x8000-aligned chunk in the partition's user data stream).
     * We advance by (stream_pos % 0x8000) — same formula used during import. */
    for(uint64_t off = 0; off < WII_GROUP_DATA_SIZE; off += NGC_SECTOR_SIZE)
    {
        uint64_t junk_off = group_disc_off + WII_GROUP_HASH_SIZE + off;
        struct junk_map_record jrec;
        if(obmafs3_junk_map_lookup(ctx, inode->inode_id, junk_off, &jrec) == OBMAFS3_OK)
        {
            struct ngc_lfg_ctx lfg;
            uint32_t seed_copy[NGC_LFG_SEED_SIZE];
            memcpy(seed_copy, jrec.seed, sizeof(seed_copy));
            ngc_lfg_set_seed(&lfg, seed_copy);

            uint64_t stream_pos = group_idx * WII_GROUP_DATA_SIZE + off;
            size_t   advance    = (size_t)(stream_pos % WII_GROUP_SIZE);
            if(advance > 0)
            {
                uint8_t  discard[4096];
                size_t   adv = advance;
                while(adv > 0)
                {
                    size_t step = adv > sizeof(discard) ? sizeof(discard) : adv;
                    ngc_lfg_get_bytes(&lfg, discard, step);
                    adv -= step;
                }
            }

            size_t chunk = WII_GROUP_DATA_SIZE - (size_t)off;
            if(chunk > NGC_SECTOR_SIZE) chunk = NGC_SECTOR_SIZE;
            ngc_lfg_get_bytes(&lfg, decrypted + WII_GROUP_HASH_SIZE + off, chunk);
        }
    }

    /* Re-encrypt */
    ngc_wii_encrypt_group(title_key, group_idx * WII_GROUP_SIZE,
                          decrypted,                        /* hash_block at offset 0 */
                          decrypted + WII_GROUP_HASH_SIZE,  /* user_data at offset 0x400 */
                          enc_out);
    return OBMAFS3_OK;
}

/* ---- Wii read ---- */

static int read_wii(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset,
                    void *buf, size_t size, const struct ngc_ri *ri)
{
    uint8_t *out = (uint8_t *)buf; uint64_t pos = offset; size_t rem = size;

    while(rem > 0)
    {
        int in_part = -1;
        for(int p = 0; p < ri->part_count; p++)
            if(pos >= ri->parts[p].data_offset && pos < ri->parts[p].data_offset + ri->parts[p].data_size)
                { in_part = p; break; }

        if(in_part >= 0)
        {
            uint64_t rel = pos - ri->parts[in_part].data_offset;
            uint64_t gidx = rel / WII_GROUP_SIZE;
            uint64_t goff = rel % WII_GROUP_SIZE;
            uint64_t gdoff = ri->parts[in_part].data_offset + gidx * WII_GROUP_SIZE;

            uint8_t enc[WII_GROUP_SIZE];
            int rc = reconstruct_group(ctx, inode, ri->parts[in_part].data_offset, gidx,
                                       ri->parts[in_part].title_key, enc);
            if(rc != OBMAFS3_OK) return rc;

            size_t avail = WII_GROUP_SIZE - (size_t)goff;
            size_t ch = rem < avail ? rem : avail;
            memcpy(out, enc + goff, ch);
            out += ch; pos += ch; rem -= ch;
        }
        else
        {
            /* Outside partitions — straight read + junk reconstruction */
            uint64_t ss = (pos / NGC_SECTOR_SIZE) * NGC_SECTOR_SIZE;
            size_t ois = (size_t)(pos - ss), ch = NGC_SECTOR_SIZE - ois;
            if(ch > rem) ch = rem;

            uint8_t sb[NGC_SECTOR_SIZE];
            int rc = obmafs3_read_media_image_data(ctx, inode, ss, sb, NGC_SECTOR_SIZE, NGC_SECTOR_SIZE, NULL, NULL);
            if(rc != OBMAFS3_OK) return rc;
            reconstruct_junk_sector(ctx, inode->inode_id, ss, sb);

            memcpy(out, sb + ois, ch);
            out += ch; pos += ch; rem -= ch;
        }
    }
    return OBMAFS3_OK;
}

/* ---- Wii U read ---- */

/*
 * Wii U sectors from offset 0x18000 onward are AES-128-CBC encrypted
 * with IV = zeros.  SI/UP/GI partitions use the disc key; GM partitions
 * use a per-title key.  The import path stores decrypted data, so we
 * re-encrypt with the correct partition key to reconstruct the original.
 *
 * Plaintext sectors (0-2 and each partition's header sector) are stored
 * verbatim and returned as-is.
 */

#define WIIU_SECTOR_SIZE      0x8000
#define WIIU_ENCRYPTED_OFFSET 0x18000

static int read_wiiu(struct obmafs3_ctx *ctx, const struct inode_record *inode, uint64_t offset,
                     void *buf, size_t size, const struct ngc_ri *ri)
{
    uint8_t *out = (uint8_t *)buf;
    uint64_t pos = offset;
    size_t   rem = size;

    while(rem > 0)
    {
        /* Align to Wii U sector boundaries */
        uint64_t sec_idx  = pos / WIIU_SECTOR_SIZE;
        uint64_t sec_off  = pos % WIIU_SECTOR_SIZE;
        uint64_t sec_base = sec_idx * WIIU_SECTOR_SIZE;

        size_t avail = WIIU_SECTOR_SIZE - (size_t)sec_off;
        size_t ch    = rem < avail ? rem : avail;

        /* Read the stored (decrypted/plaintext) sector from dedup */
        uint8_t sector[WIIU_SECTOR_SIZE];
        int rc = obmafs3_read_media_image_data(ctx, inode, sec_base, sector, WIIU_SECTOR_SIZE,
                                               NGC_SECTOR_SIZE, NULL, NULL);
        if(rc != OBMAFS3_OK) return rc;

        /*
         * Determine if this sector needs re-encryption.
         * Plaintext sectors:
         *   - Before the encrypted area (offset < 0x18000, i.e. sectors 0-2)
         *   - Partition header sectors (each partition's start_sector)
         */
        int is_plain = 0;

        if(sec_base < WIIU_ENCRYPTED_OFFSET)
        {
            is_plain = 1;
        }
        else
        {
            for(int p = 0; p < ri->part_count; p++)
            {
                if(sec_base == ri->parts[p].data_offset)
                {
                    is_plain = 1;
                    break;
                }
            }
        }

        if(!is_plain)
        {
            /* Find the partition this sector belongs to and re-encrypt with its key */
            const uint8_t *key = NULL;
            for(int p = 0; p < ri->part_count; p++)
            {
                uint64_t p_start = ri->parts[p].data_offset;
                uint64_t p_end   = p_start + ri->parts[p].data_size;
                if(sec_base >= p_start && sec_base < p_end)
                {
                    key = ri->parts[p].title_key;
                    break;
                }
            }

            /* If sector falls outside all partitions, use the first partition's key
             * (disc key stored with SI/UP partitions) */
            if(!key && ri->part_count > 0)
                key = ri->parts[0].title_key;

            if(key)
            {
                struct aes128_ctx aes;
                uint8_t           iv[16];
                uint8_t           enc[WIIU_SECTOR_SIZE];
                memset(iv, 0, sizeof(iv));
                aes128_init(&aes, key);
                aes128_cbc_encrypt(&aes, iv, sector, enc, WIIU_SECTOR_SIZE);
                memcpy(out, enc + sec_off, ch);
            }
            else
            {
                memcpy(out, sector + sec_off, ch);
            }
        }
        else
        {
            memcpy(out, sector + sec_off, ch);
        }

        out += ch;
        pos += ch;
        rem -= ch;
    }

    return OBMAFS3_OK;
}

/* ---- Public entry point ---- */

int obmafs3_read_nintendo_image_data(struct obmafs3_ctx *ctx, const struct inode_record *inode,
                                     uint64_t offset, void *buf, size_t size)
{
    struct ngc_ri ri;
    int rc = load_info(ctx, inode->inode_id, &ri);
    if(rc != OBMAFS3_OK || ri.disc_type == 0)
        return read_gc(ctx, inode, offset, buf, size);
    if(ri.disc_type == 2)
        return read_wiiu(ctx, inode, offset, buf, size, &ri);
    return read_wii(ctx, inode, offset, buf, size, &ri);
}
