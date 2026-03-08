// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ngcw — Nintendo GameCube/Wii disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Sector-by-sector import for GameCube and Wii disc images.
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

#include "../../src/lib/nintendo.h"
#include "import_ngcw.h"

#include <defs.h>
#include <obmafs3_ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- Progress reporting ---- */

static void print_progress(uint64_t current, uint64_t total, const char *label)
{
    if(total == 0) return;
    int pct = (int)((current * 100) / total);
    int bar_width = 30;
    int filled = (int)((current * bar_width) / total);
    if(filled > bar_width) filled = bar_width;

    printf("\r  \033[36m%s\033[0m \033[90m[\033[0m", label);
    for(int i = 0; i < bar_width; i++)
    {
        if(i < filled)
            printf("\033[32m\xe2\x96\x88\033[0m"); /* █ green */
        else if(i == filled)
            printf("\033[33m\xe2\x96\x93\033[0m"); /* ▓ yellow */
        else
            printf("\033[90m\xe2\x96\x91\033[0m"); /* ░ dim */
    }
    printf("\033[90m]\033[0m %3d%% ", pct);

    /* Human-readable sizes */
    double cur_gb = (double)current / (1024.0 * 1024.0 * 1024.0);
    double tot_gb = (double)total / (1024.0 * 1024.0 * 1024.0);
    if(tot_gb >= 1.0)
        printf("\033[90m(%.1f/%.1f GB)\033[0m", cur_gb, tot_gb);
    else
        printf("\033[90m(%.1f/%.1f MB)\033[0m", cur_gb * 1024.0, tot_gb * 1024.0);

    fflush(stdout);
}

/* ================================================================== */
/*  GameCube import                                                    */
/* ================================================================== */

int ngcw_import_gc(int iso_fd, int out_fd, const uint8_t *header, uint64_t disc_size,
                   struct ngcw_junk_collector *jc)
{
    /*
     * GameCube layout:
     * 0x000000 - 0x000440: Disc header
     * 0x000440 - variable: Boot block (bb.bin), apploader, DOL, FST
     * FST offset and size come from the disc header
     *
     * FST offset: big-endian uint32 at header + 0x424
     * FST size:   big-endian uint32 at header + 0x428
     */
    uint32_t fst_offset  = ngc_be32(header + 0x424);
    uint32_t fst_size    = ngc_be32(header + 0x428);

    /* Build data region map from FST */
    uint8_t *fst = malloc(fst_size);
    if(!fst) { fprintf(stderr, "Error: out of memory for FST\n"); return -1; }

    if(pread(iso_fd, fst, fst_size, (off_t)fst_offset) < (ssize_t)fst_size)
    {
        fprintf(stderr, "Error: cannot read FST at 0x%X\n", fst_offset);
        free(fst);
        return -1;
    }

    struct ngc_data_map data_map;
    if(ngc_build_data_map(fst, fst_size, 0, 0, &data_map) != 0)  /* GC: address_shift = 0 */
    {
        fprintf(stderr, "Error: cannot parse FST\n");
        free(fst);
        return -1;
    }
    free(fst);

    /* Also add the system area as data: header + boot block + apploader + DOL + FST.
     * Everything from 0 to just past the FST is system data. */
    uint64_t sys_end = fst_offset + fst_size;

    /* Walk the disc in 0x8000-byte blocks for junk detection.
     * LFG seed extraction needs at least 521 u32 words (2084 bytes).
     * Testing at 0x8000-aligned boundaries with data_offset=0 gives
     * 0x8000/4 = 8192 words, always enough.  Sectors within a confirmed
     * junk block are all junk; sectors within a non-junk block are tested
     * against the FST data map individually.
     *
     * The FST is authoritative: if a sector is not in the system area
     * and not in any FST file region, it IS unused regardless of whether
     * the LFG check succeeds.  Gaps may contain PRNG junk or plain zeroes
     * (padding between system area and first file).  We attempt LFG seed
     * extraction to obtain a seed for reconstruction, but if it fails
     * the sector is still treated as unused (not stored). */

    #define GC_BLOCK_SIZE  0x8000
    #define GC_SECTORS_PER_BLOCK (GC_BLOCK_SIZE / NGC_SECTOR_SIZE)  /* 16 */

    uint64_t total_sectors = disc_size / NGC_SECTOR_SIZE;
    uint64_t data_sectors = 0, junk_sectors = 0, zero_sectors = 0;

    uint8_t  block_buf[GC_BLOCK_SIZE];

    for(uint64_t block_off = 0; block_off < disc_size; block_off += GC_BLOCK_SIZE)
    {
        if((block_off & 0x7FFFF) == 0) print_progress(block_off, disc_size, "Importing GC sectors");

        size_t block_bytes = GC_BLOCK_SIZE;
        if(block_off + block_bytes > disc_size) block_bytes = (size_t)(disc_size - block_off);

        ssize_t n = pread(iso_fd, block_buf, block_bytes, (off_t)block_off);
        if(n < (ssize_t)block_bytes)
        {
            if(n > 0) memset(block_buf + n, 0, block_bytes - (size_t)n);
            else memset(block_buf, 0, block_bytes);
        }

        /* Try LFG seed extraction on the full block (for junk-only blocks) */
        int      block_is_lfg = 0;
        uint32_t block_seed[NGC_LFG_SEED_SIZE];
        if(block_bytes >= NGC_LFG_K * sizeof(uint32_t))
        {
            size_t matched = ngc_lfg_get_seed(block_buf, block_bytes, 0, block_seed);
            if(matched >= block_bytes)
                block_is_lfg = 1;
        }

        /* Classify sectors and build the output block in-place:
         * zero junk sectors in block_buf, record seeds, keep data sectors. */
        for(size_t s = 0; s < block_bytes; s += NGC_SECTOR_SIZE)
        {
            uint64_t offset     = block_off + s;
            size_t   sector_len = NGC_SECTOR_SIZE;
            if(s + sector_len > block_bytes) sector_len = block_bytes - s;

            int is_data;
            if(offset < sys_end)
                is_data = 1;
            else
                is_data = ngc_is_data_region(&data_map, offset, sector_len);

            if(is_data)
            {
                data_sectors++;
            }
            else if(block_is_lfg)
            {
                memset(block_buf + s, 0, sector_len);
                ngcw_junk_collector_add(jc, offset, sector_len, 0xFFFF, block_seed);
                junk_sectors++;
            }
            else
            {
                int all_zero = 1;
                for(size_t b = 0; b < sector_len; b++)
                {
                    if(block_buf[s + b] != 0) { all_zero = 0; break; }
                }

                if(all_zero)
                {
                    zero_sectors++;
                    junk_sectors++;
                }
                else
                {
                    /* Unknown non-zero content outside FST — keep verbatim */
                    data_sectors++;
                }
            }
        }

        /* Write the entire block in one call */
        ssize_t w = write(out_fd, block_buf, block_bytes);
        if(w < (ssize_t)block_bytes)
        {
            fprintf(stderr, "\nError: write failed at offset 0x%lX\n", (unsigned long)block_off);
            ngc_data_map_free(&data_map);
            return -1;
        }
    }

    (void)total_sectors;
    printf("\n  \033[32m\xe2\x9c\x93\033[0m \033[1m%lu\033[0m data, \033[1m%lu\033[0m junk (\033[33m%lu\033[0m LFG, \033[90m%lu\033[0m zero-fill)",
           (unsigned long)data_sectors, (unsigned long)junk_sectors,
           (unsigned long)(junk_sectors - zero_sectors), (unsigned long)zero_sectors);
    printf("\n");

    ngc_data_map_free(&data_map);
    return 0;
}

/* ================================================================== */
/*  Wii import                                                         */
/* ================================================================== */

/*
 * Wii disc layout:
 * - Unencrypted area: disc header (0x440), region settings, partition table
 * - Each partition has: ticket, TMD, cert chain, H3 table, then encrypted groups
 * - Between partitions: junk (Fibonacci PRNG)
 * - After last partition to disc end: junk
 *
 * Import approach:
 * 1. Walk the disc linearly in 2048-byte sectors
 * 2. For sectors inside a partition's data area:
 *    - Decrypt the group, strip hash block, extract user data
 *    - Split into 2048-byte sectors and write to dedup
 * 3. For sectors outside partitions:
 *    - Check if it's a known structure (header, partition table, ticket areas)
 *    - Otherwise verify it's junk, write as data (dedup will handle it)
 *
 * Since the write path uses standard write() (sector_size=2048 dedup),
 * we write decrypted user data. The read path reconstructs the original
 * encrypted format using the stored media tags (title keys, H3, etc.)
 */

int ngcw_import_wii(int iso_fd, int out_fd, const uint8_t *header, uint64_t disc_size,
                    uint16_t part_count, struct ngc_partition *parts,
                    struct ngcw_junk_collector *jc)
{
    uint64_t total_sectors = disc_size / NGC_SECTOR_SIZE;
    uint64_t data_sectors = 0, junk_sectors = 0, partition_sectors = 0;
    int      verify_errors = 0;

    /* Build data maps and track system area end for each partition */
    struct ngc_data_map *part_maps    = calloc(part_count, sizeof(struct ngc_data_map));
    uint64_t            *part_sys_end = calloc(part_count, sizeof(uint64_t));
    if((!part_maps || !part_sys_end) && part_count > 0) { free(part_maps); free(part_sys_end); free(part_sys_end); return -1; }

    for(int p = 0; p < part_count; p++)
    {
        /* Read the partition's FST.
         * Boot block offset (within partition data): big-endian uint32 at decrypted offset 0x420
         * FST offset: big-endian uint32 at decrypted offset 0x424
         * FST size: big-endian uint32 at decrypted offset 0x428
         *
         * We need to decrypt group 0 of the partition to get the boot block first. */

        /* Read and decrypt first group to get partition boot info */
        uint8_t enc_group[WII_GROUP_SIZE];
        if(pread(iso_fd, enc_group, WII_GROUP_SIZE, (off_t)parts[p].data_offset) < WII_GROUP_SIZE)
        {
            fprintf(stderr, "Error: cannot read first group of partition %d\n", p);
            for(int q = 0; q < p; q++) ngc_data_map_free(&part_maps[q]);
            free(part_maps); free(part_sys_end);
            return -1;
        }

        uint8_t hash_block[WII_GROUP_HASH_SIZE];
        uint8_t group_data[WII_GROUP_DATA_SIZE];
        ngc_wii_decrypt_group(parts[p].title_key, 0, enc_group, hash_block, group_data);

        /* FST offset and size from decrypted boot block.
         * Both are word-shifted (<<2) on Wii. */
        uint32_t fst_offset = ngc_be32(group_data + 0x424) << 2;
        uint32_t fst_size   = ngc_be32(group_data + 0x428) << 2;

        /* System area = everything up to and including the FST */
        part_sys_end[p] = (uint64_t)fst_offset + fst_size;

        if(fst_size > 0 && fst_size < 64 * 1024 * 1024) /* sanity check: < 64 MiB */
        {
            uint8_t *fst = malloc(fst_size);
            if(!fst)
            {
                for(int q = 0; q < p; q++) ngc_data_map_free(&part_maps[q]);
                free(part_maps); free(part_sys_end);
                return -1;
            }

            /* Read the FST from the decrypted partition data.
             * This requires reading and decrypting the groups that contain the FST. */
            uint64_t fst_read = 0;
            while(fst_read < fst_size)
            {
                uint64_t logical_offset = fst_offset + fst_read;
                uint64_t group_idx      = logical_offset / WII_GROUP_DATA_SIZE;
                uint64_t group_off      = logical_offset % WII_GROUP_DATA_SIZE;
                uint64_t disc_off       = parts[p].data_offset + group_idx * WII_GROUP_SIZE;

                uint8_t enc_grp[WII_GROUP_SIZE];
                if(pread(iso_fd, enc_grp, WII_GROUP_SIZE, (off_t)disc_off) < WII_GROUP_SIZE)
                {
                    fprintf(stderr, "Error: cannot read FST group for partition %d\n", p);
                    free(fst);
                    for(int q = 0; q <= p; q++) ngc_data_map_free(&part_maps[q]);
                    free(part_maps); free(part_sys_end);
                    return -1;
                }

                uint8_t hb[WII_GROUP_HASH_SIZE];
                uint8_t gd[WII_GROUP_DATA_SIZE];
                ngc_wii_decrypt_group(parts[p].title_key, disc_off - parts[p].data_offset, enc_grp, hb, gd);

                uint64_t avail = WII_GROUP_DATA_SIZE - group_off;
                uint64_t chunk = (fst_size - fst_read < avail) ? fst_size - fst_read : avail;
                memcpy(fst + fst_read, gd + group_off, chunk);
                fst_read += chunk;
            }

            ngc_build_data_map(fst, fst_size, 0, 2, &part_maps[p]);  /* Wii: address_shift = 2 */
            free(fst);
        }
    }

    /* Walk the disc linearly */
    /* For Wii: we process in groups (0x8000 bytes) when inside partition data,
     * and in 2048-byte sectors otherwise */
    uint8_t sector_buf[NGC_SECTOR_SIZE];

    for(uint64_t offset = 0; offset < disc_size;)
    {
        if((offset & 0x7FFFF) == 0) print_progress(offset, disc_size, "Importing Wii disc");

        /* Check if this offset falls inside a partition's data area */
        int in_part = -1;
        for(int p = 0; p < part_count; p++)
        {
            if(offset >= parts[p].data_offset && offset < parts[p].data_offset + parts[p].data_size)
            {
                in_part = p;
                break;
            }
        }

        if(in_part >= 0)
        {
            /* Inside partition data — decrypt the group and write the full
             * decrypted group (hash_block + user_data = 0x8000 bytes).
             * The read path re-encrypts it to reconstruct the original. */
            uint64_t group_disc_off = offset;
            uint64_t rel = offset - parts[in_part].data_offset;
            group_disc_off = parts[in_part].data_offset + (rel / WII_GROUP_SIZE) * WII_GROUP_SIZE;

            uint8_t enc_grp[WII_GROUP_SIZE];
            ssize_t n = pread(iso_fd, enc_grp, WII_GROUP_SIZE, (off_t)group_disc_off);
            if(n < WII_GROUP_SIZE)
            {
                if(n > 0) memset(enc_grp + n, 0, WII_GROUP_SIZE - (size_t)n);
                else memset(enc_grp, 0, WII_GROUP_SIZE);
            }

            uint8_t hash_block[WII_GROUP_HASH_SIZE];
            uint8_t group_data[WII_GROUP_DATA_SIZE];
            ngc_wii_decrypt_group(parts[in_part].title_key, group_disc_off - parts[in_part].data_offset,
                                  enc_grp, hash_block, group_data);

            /* Detect junk in decrypted user_data.
             *
             * The LFG junk stream spans the partition's decrypted user data.
             * GetSeed with data_offset % 0x8000 extracts a PER-BLOCK seed.
             * A group (0x7C00 bytes) can span two 0x8000-aligned blocks.
             * We extract up to two seeds from free sectors in each block,
             * then verify each free sector against the matching seed. */
            uint64_t group_num      = rel / WII_GROUP_SIZE;
            uint64_t logical_offset = group_num * WII_GROUP_DATA_SIZE;

            /* The offset within the current 0x8000-aligned block */
            uint64_t block_phase = logical_offset % WII_GROUP_SIZE;
            /* Byte offset within the group where the second block starts */
            uint64_t block2_start = (block_phase > 0) ? (WII_GROUP_SIZE - block_phase) : WII_GROUP_DATA_SIZE;
            if(block2_start > WII_GROUP_DATA_SIZE) block2_start = WII_GROUP_DATA_SIZE;

            /* Classify each sector with the FST map */
            int sector_is_data[16];
            int num_sectors    = 0;
            for(uint64_t off = 0; off < WII_GROUP_DATA_SIZE; off += NGC_SECTOR_SIZE)
            {
                uint64_t chunk = WII_GROUP_DATA_SIZE - off;
                if(chunk > NGC_SECTOR_SIZE) chunk = NGC_SECTOR_SIZE;

                if(logical_offset + off < part_sys_end[in_part])
                    sector_is_data[num_sectors] = 1;
                else
                    sector_is_data[num_sectors] = ngc_is_data_region(
                        &part_maps[in_part], logical_offset + off, chunk);
                num_sectors++;
            }

            /* Extract seed for block 1 (offsets 0..block2_start-1) */
            int      have_seed1 = 0;
            uint32_t seed1[NGC_LFG_SEED_SIZE];
            /* Extract seed for block 2 (offsets block2_start..0x7C00-1) */
            int      have_seed2 = 0;
            uint32_t seed2[NGC_LFG_SEED_SIZE];

            for(int s = 0; s < num_sectors; s++)
            {
                if(sector_is_data[s]) continue;
                uint64_t soff      = (uint64_t)s * NGC_SECTOR_SIZE;
                int      in_block2 = (soff >= block2_start);
                if(in_block2 && have_seed2) continue;
                if(!in_block2 && have_seed1) continue;

                uint64_t stream_pos = logical_offset + soff;
                size_t   avail      = (size_t)(WII_GROUP_DATA_SIZE - soff);
                size_t   doff       = (size_t)(stream_pos % WII_GROUP_SIZE);

                if(avail < NGC_LFG_K * sizeof(uint32_t)) continue;

                uint32_t *dst = in_block2 ? seed2 : seed1;
                size_t m = ngc_lfg_get_seed(group_data + soff, avail, doff, dst);
                if(m > 0)
                {
                    if(in_block2) have_seed2 = 1;
                    else          have_seed1 = 1;
                }
                if(have_seed1 && have_seed2) break;
            }

            /* Build output: hash_block (always verbatim) + user_data */
            uint8_t decrypted_group[WII_GROUP_SIZE];
            memcpy(decrypted_group, hash_block, WII_GROUP_HASH_SIZE);

            for(int s = 0; s < num_sectors; s++)
            {
                uint64_t off     = (uint64_t)s * NGC_SECTOR_SIZE;
                uint64_t chunk   = WII_GROUP_DATA_SIZE - off;
                if(chunk > NGC_SECTOR_SIZE) chunk = NGC_SECTOR_SIZE;
                uint64_t out_off = WII_GROUP_HASH_SIZE + off;

                if(sector_is_data[s])
                {
                    memcpy(decrypted_group + out_off, group_data + off, chunk);
                    data_sectors++;
                    continue;
                }

                int      in_block2  = (off >= block2_start);
                int      have_seed  = in_block2 ? have_seed2 : have_seed1;
                uint32_t *the_seed  = in_block2 ? seed2 : seed1;

                if(!have_seed)
                {
                    memcpy(decrypted_group + out_off, group_data + off, chunk);
                    data_sectors++;
                    continue;
                }

                /* Regenerate LFG at stream_pos % 0x8000 and compare */
                struct ngc_lfg_ctx lfg;
                uint32_t sc[NGC_LFG_SEED_SIZE];
                memcpy(sc, the_seed, sizeof(sc));
                ngc_lfg_set_seed(&lfg, sc);

                size_t adv = (size_t)((logical_offset + off) % WII_GROUP_SIZE);
                if(adv > 0)
                {
                    uint8_t discard[4096];
                    size_t rem = adv;
                    while(rem > 0)
                    {
                        size_t step = rem > sizeof(discard) ? sizeof(discard) : rem;
                        ngc_lfg_get_bytes(&lfg, discard, step);
                        rem -= step;
                    }
                }

                uint8_t expected[NGC_SECTOR_SIZE];
                ngc_lfg_get_bytes(&lfg, expected, chunk);

                if(memcmp(group_data + off, expected, chunk) == 0)
                {
                    memset(decrypted_group + out_off, 0, chunk);
                    ngcw_junk_collector_add(jc, group_disc_off + WII_GROUP_HASH_SIZE + off,
                                           chunk, (uint16_t)in_part, the_seed);
                    junk_sectors++;
                }
                else
                {
                    memcpy(decrypted_group + out_off, group_data + off, chunk);
                    data_sectors++;
                }
            }

            partition_sectors += 16;

            ssize_t w = write(out_fd, decrypted_group, WII_GROUP_SIZE);
            if(w < WII_GROUP_SIZE)
            {
                fprintf(stderr, "\nError: write failed\n");
                for(int q = 0; q < part_count; q++) ngc_data_map_free(&part_maps[q]);
                free(part_maps); free(part_sys_end);
                return -1;
            }

            offset = group_disc_off + WII_GROUP_SIZE;
        }

        else
        {
            /* Outside any partition — write as 2048-byte sectors of
             * unencrypted disc data. These are typically the disc header,
             * partition table, or inter-partition junk. */
            ssize_t n = pread(iso_fd, sector_buf, NGC_SECTOR_SIZE, (off_t)offset);
            if(n < NGC_SECTOR_SIZE)
            {
                if(n > 0) memset(sector_buf + n, 0, NGC_SECTOR_SIZE - (size_t)n);
                else memset(sector_buf, 0, NGC_SECTOR_SIZE);
            }

            ssize_t w = write(out_fd, sector_buf, NGC_SECTOR_SIZE);
            if(w < NGC_SECTOR_SIZE)
            {
                fprintf(stderr, "\nError: write failed\n");
                for(int q = 0; q < part_count; q++) ngc_data_map_free(&part_maps[q]);
                free(part_maps); free(part_sys_end);
                return -1;
            }

            data_sectors++;
            offset += NGC_SECTOR_SIZE;
        }
    }

    printf("\n  \033[32m\xe2\x9c\x93\033[0m \033[1m%lu\033[0m data, \033[1m%lu\033[0m junk, \033[1m%lu\033[0m partition sectors",
           (unsigned long)data_sectors, (unsigned long)junk_sectors, (unsigned long)partition_sectors);
    if(verify_errors > 0) printf(", %d junk verification mismatches", verify_errors);
    printf("\n");

    for(int p = 0; p < part_count; p++) ngc_data_map_free(&part_maps[p]);
    free(part_maps); free(part_sys_end);
    return 0;
}
