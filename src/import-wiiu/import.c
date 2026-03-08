// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-wiiu — Nintendo Wii U disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     TOC parsing and sector-by-sector import for Wii U disc images.
//     All sectors from offset 0x18000 onward are AES-128-CBC encrypted with
//     the disc key and IV=0.  We decrypt them on import and re-encrypt on
//     read for byte-identical reconstruction.  Sectors before 0x18000 and
//     each partition's header sector are plaintext and stored verbatim.
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

#include "../../src/lib/aes128.h"
#include "import_wiiu.h"

#include <defs.h>

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- Wii U common key ---- */
const uint8_t WIIU_COMMON_KEY[16] = {0xD7, 0xB0, 0x04, 0x02, 0x65, 0x9B, 0xA2, 0xAB,
                                     0xD2, 0xCB, 0x0D, 0xB2, 0x7F, 0xA2, 0xB6, 0x56};

/* ---- Helpers ---- */

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void print_progress(uint64_t current, uint64_t total, const char *label)
{
    if(total == 0) return;
    int pct       = (int)((current * 100) / total);
    int bar_width = 30;
    int filled    = (int)((current * bar_width) / total);
    if(filled > bar_width) filled = bar_width;

    printf("\r  \033[36m%s\033[0m \033[90m[\033[0m", label);
    for(int i = 0; i < bar_width; i++)
    {
        if(i < filled)       printf("\033[32m\xe2\x96\x88\033[0m");
        else if(i == filled) printf("\033[33m\xe2\x96\x93\033[0m");
        else                 printf("\033[90m\xe2\x96\x91\033[0m");
    }
    printf("\033[90m]\033[0m %3d%% ", pct);

    double cur_gb = (double)current / (1024.0 * 1024.0 * 1024.0);
    double tot_gb = (double)total / (1024.0 * 1024.0 * 1024.0);
    if(tot_gb >= 1.0)
        printf("\033[90m(%.1f/%.1f GB)\033[0m", cur_gb, tot_gb);
    else
        printf("\033[90m(%.1f/%.1f MB)\033[0m", cur_gb * 1024.0, tot_gb * 1024.0);

    fflush(stdout);
}

/* ================================================================== */
/*  TOC parsing                                                        */
/* ================================================================== */

int wiiu_parse_toc(struct wiiu_reader *reader, const uint8_t disc_key[16],
                   struct wiiu_partition *parts, int *part_count)
{
    /* Read the encrypted TOC sector (sector 3, disc offset 0x18000) */
    uint8_t enc_sector[WIIU_SECTOR_SIZE];
    ssize_t n = wiiu_reader_pread(reader, enc_sector, WIIU_SECTOR_SIZE, WIIU_ENCRYPTED_OFFSET);
    if(n < (ssize_t)WIIU_SECTOR_SIZE)
    {
        fprintf(stderr, "Error: cannot read TOC sector\n");
        return -1;
    }

    /* Decrypt with disc key, IV = all zeros */
    uint8_t           dec_sector[WIIU_SECTOR_SIZE];
    struct aes128_ctx  aes;
    uint8_t            iv[16];
    memset(iv, 0, sizeof(iv));
    aes128_init(&aes, disc_key);
    aes128_cbc_decrypt(&aes, iv, enc_sector, dec_sector, WIIU_SECTOR_SIZE);

    /* Verify decrypted TOC signature */
    if(be32(dec_sector) != WIIU_TOC_SIGNATURE)
    {
        fprintf(stderr, "Error: TOC decryption failed (bad signature 0x%08X, expected 0x%08X)\n",
                be32(dec_sector), WIIU_TOC_SIGNATURE);
        fprintf(stderr, "       Check that the disc key is correct\n");
        return -1;
    }

    /* Parse header: +0x1C = partition count */
    uint32_t toc_part_count = be32(dec_sector + 0x1C);
    if(toc_part_count > WIIU_MAX_PARTITIONS) toc_part_count = WIIU_MAX_PARTITIONS;

    *part_count = (int)toc_part_count;

    /*
     * Partition entries at offset 0x800, each 0x80 bytes.
     * Bytes 0x00..0x18: identifier (25 bytes, NUL-padded)
     * Bytes 0x00..0x7F: full name
     * Byte  0x1F:       flags
     * Offset 0x20:      BE32 sector number (partition offset = sector * 0x8000 - 0x10000
     *                   relative to WIIU_ENCRYPTED_OFFSET)
     */
    for(int i = 0; i < (int)toc_part_count; i++)
    {
        const uint8_t *entry = dec_sector + WIIU_TOC_ENTRIES_OFF + i * WIIU_TOC_ENTRY_SIZE;

        memcpy(parts[i].identifier, entry, 25);
        parts[i].identifier[25] = '\0';

        memcpy(parts[i].name, entry, WIIU_TOC_ENTRY_SIZE);
        parts[i].name[WIIU_TOC_ENTRY_SIZE - 1] = '\0';

        parts[i].start_sector  = be32(entry + 0x20);
        parts[i].has_title_key = 0;
        memset(parts[i].key, 0, 16);
    }

    return 0;
}

/* ================================================================== */
/*  Title key extraction from SI/GI partitions                         */
/* ================================================================== */

/*
 * Read and decrypt data from a volume partition (SI/GI/UP) at a given
 * offset within the partition's data area.  Each 0x8000-byte sector is
 * independently AES-128-CBC encrypted with IV=0.
 *
 * partition_offset = absolute disc byte offset of the partition's first
 *                    data sector (WIIU_ENCRYPTED_OFFSET + part.start_sector * 0x8000 - 0x10000
 *                    per wudecrypt, i.e. WIIU_ENCRYPTED_OFFSET + (start_sector*0x8000 - 0x10000))
 * file_offset     = byte offset within the partition
 */
static int read_volume_decrypted(struct wiiu_reader *reader, const uint8_t key[16],
                                 uint64_t partition_disc_offset,
                                 uint64_t file_offset, void *buf, size_t size)
{
    struct aes128_ctx aes;
    aes128_init(&aes, key);

    uint8_t *out  = (uint8_t *)buf;
    size_t   done = 0;

    while(done < size)
    {
        uint64_t cur     = file_offset + done;
        uint64_t sec_idx = cur / WIIU_SECTOR_SIZE;
        uint64_t sec_off = cur % WIIU_SECTOR_SIZE;

        uint64_t disc_off = partition_disc_offset + sec_idx * WIIU_SECTOR_SIZE;

        uint8_t enc[WIIU_SECTOR_SIZE];
        uint8_t dec[WIIU_SECTOR_SIZE];
        ssize_t n = wiiu_reader_pread(reader, enc, WIIU_SECTOR_SIZE, disc_off);
        if(n < (ssize_t)WIIU_SECTOR_SIZE)
        {
            if(n > 0) memset(enc + n, 0, WIIU_SECTOR_SIZE - (size_t)n);
            else      memset(enc, 0, WIIU_SECTOR_SIZE);
        }

        uint8_t iv[16];
        memset(iv, 0, sizeof(iv));
        aes128_cbc_decrypt(&aes, iv, enc, dec, WIIU_SECTOR_SIZE);

        size_t chunk = WIIU_SECTOR_SIZE - (size_t)sec_off;
        if(chunk > size - done) chunk = size - done;
        memcpy(out + done, dec + sec_off, chunk);
        done += chunk;
    }

    return 0;
}

/*
 * Case-insensitive comparison (strncmp replacement for ticket filename matching).
 */
static int strnicmp_local(const char *a, const char *b, size_t n)
{
    for(size_t i = 0; i < n; i++)
    {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if(ca != cb) return ca - cb;
        if(ca == 0) return 0;
    }
    return 0;
}

/*
 * Scan a partition's FST for TITLE.TIK files, extract and decrypt
 * per-title keys, and match them to GM partitions by name.
 *
 * Wii U FST layout (from wudecrypt):
 *   +0x00: signature "FST\0"
 *   +0x04: BE32 offset_factor (entries_offset = offset_factor * cluster_count + 0x20)
 *   +0x08: BE32 cluster_count
 *   Cluster descriptors at +0x20, each 0x20 bytes:
 *     +0x00: BE32 cluster_start_sector
 *     +0x04: BE32 cluster_size_in_sectors
 *   File entries at entries_offset, each 0x10 bytes:
 *     +0x00: BE32 (top byte = type, lower 3 bytes = name_offset)
 *     +0x04: BE32 offset_in_cluster (<<5)
 *     +0x08: BE32 size (files) or last_entry_idx (dirs)
 *     +0x0C: BE16 unknown + BE16 starting_cluster
 *   Name table follows immediately after all entries.
 */
int wiiu_extract_title_keys(struct wiiu_reader *reader, const uint8_t disc_key[16],
                            struct wiiu_partition *parts, int part_count)
{
    struct aes128_ctx common_aes;
    aes128_init(&common_aes, WIIU_COMMON_KEY);

    int keys_found = 0;

    for(int p = 0; p < part_count; p++)
    {
        /* Only scan SI and GI partitions for tickets */
        if(strncmp(parts[p].identifier, "SI", 2) != 0 && strncmp(parts[p].identifier, "GI", 2) != 0)
            continue;

        /* Compute partition's disc data offset.
         * Per wudecrypt: data_offset = WIIU_ENCRYPTED_OFFSET + part_offset
         *                part_offset = start_sector * 0x8000 - 0x10000
         * So: disc_data_offset = WIIU_ENCRYPTED_OFFSET + start_sector * 0x8000 - 0x10000 */
        uint64_t part_disc_off =
            WIIU_ENCRYPTED_OFFSET + (uint64_t)parts[p].start_sector * WIIU_SECTOR_SIZE - 0x10000;

        /* Read FST header (first sector of partition data) */
        uint8_t fst_hdr[WIIU_SECTOR_SIZE];
        if(read_volume_decrypted(reader, disc_key, part_disc_off, 0, fst_hdr, WIIU_SECTOR_SIZE) != 0)
            continue;

        if(memcmp(fst_hdr, "FST\0", 4) != 0) continue;

        uint32_t offset_factor = be32(fst_hdr + 4);
        uint32_t cluster_count = be32(fst_hdr + 8);

        /* Parse cluster descriptors at +0x20 */
        uint64_t *cluster_offsets = calloc(cluster_count, sizeof(uint64_t));
        if(!cluster_offsets) continue;

        for(uint32_t c = 0; c < cluster_count; c++)
        {
            uint32_t raw = be32(fst_hdr + 0x20 + c * 0x20);
            uint64_t start = (uint64_t)raw * WIIU_SECTOR_SIZE;
            cluster_offsets[c] = (start > WIIU_SECTOR_SIZE) ? start - WIIU_SECTOR_SIZE : 0;
        }

        /* Entries offset in the FST data */
        uint64_t entries_offset = (uint64_t)offset_factor * cluster_count + 0x20;

        /* Read root entry to get total entries */
        uint8_t root_entry[0x10];
        if(read_volume_decrypted(reader, disc_key, part_disc_off, entries_offset, root_entry, 0x10) != 0)
        {
            free(cluster_offsets);
            continue;
        }
        uint32_t total_entries = be32(root_entry + 8);
        if(total_entries > 100000) { free(cluster_offsets); continue; } /* sanity */

        /* Name table starts after all entries */
        uint64_t name_table_offset = entries_offset + (uint64_t)total_entries * 0x10;

        /* Read all entries + name table — allocate enough room */
        size_t fst_data_size = (size_t)((name_table_offset - entries_offset) + 0x10000); /* +64K for names */
        if(fst_data_size > 16 * 1024 * 1024) { free(cluster_offsets); continue; }
        uint8_t *fst_data = malloc(fst_data_size);
        if(!fst_data) { free(cluster_offsets); continue; }

        if(read_volume_decrypted(reader, disc_key, part_disc_off, entries_offset,
                                 fst_data, fst_data_size) != 0)
        {
            free(fst_data);
            free(cluster_offsets);
            continue;
        }

        /* Scan for TITLE.TIK files */
        for(uint32_t e = 0; e < total_entries; e++)
        {
            const uint8_t *ent = fst_data + e * 0x10;
            uint8_t  type       = ent[0];
            uint32_t name_off   = be32(ent) & 0x00FFFFFF;
            uint64_t file_off   = (uint64_t)be32(ent + 4) << 5;
            uint32_t file_size  = be32(ent + 8);
            uint16_t cluster_id = (uint16_t)((ent[0x0E] << 8) | ent[0x0F]);

            if(type == 1) continue; /* directory */
            if(file_size < 0x200) continue; /* too small for a ticket */

            /* Get filename from name table */
            uint64_t fname_off = (uint64_t)name_off;
            if(fname_off >= fst_data_size - (entries_offset - entries_offset))
            {
                /* Name outside our buffer — look it up relative to name_table */
                uint64_t abs_name = name_table_offset + fname_off;
                (void)abs_name;
                continue;
            }

            const char *fname = (const char *)(fst_data + (name_table_offset - entries_offset) + name_off);

            if(strnicmp_local(fname, "title.tik", 9) != 0) continue;

            /* Found a ticket file! Read the relevant fields:
             *   +0x1BF: encrypted title key (16 bytes)
             *   +0x1DC: title ID (8 bytes) */
            if(cluster_id >= cluster_count) continue;

            uint64_t tik_disc_off = part_disc_off + cluster_offsets[cluster_id] + file_off;
            (void)tik_disc_off; /* unused — we read via volume function */

            uint8_t tik_buf[0x200];
            uint64_t tik_volume_off = cluster_offsets[cluster_id] + file_off;
            if(read_volume_decrypted(reader, disc_key, part_disc_off,
                                     tik_volume_off + 0x1BF, tik_buf, 0x10 + 0x1D + 8) != 0)
                continue;

            uint8_t enc_title_key[16];
            uint8_t title_id[8];
            memcpy(enc_title_key, tik_buf, 16);
            /* title_id is at offset 0x1DC - 0x1BF = 0x1D relative to our read */
            memcpy(title_id, tik_buf + 0x1D, 8);

            /* Decrypt title key: AES-128-CBC with common key, IV = title_id + 8 zero bytes */
            uint8_t dec_title_key[16];
            uint8_t iv[16];
            memset(iv, 0, 16);
            memcpy(iv, title_id, 8);
            aes128_cbc_decrypt(&common_aes, iv, enc_title_key, dec_title_key, 16);

            /* Build expected GM partition name from title ID */
            char gm_name[19];
            snprintf(gm_name, sizeof(gm_name),
                     "GM%02X%02X%02X%02X%02X%02X%02X%02X",
                     title_id[0], title_id[1], title_id[2], title_id[3],
                     title_id[4], title_id[5], title_id[6], title_id[7]);

            /* Match to a GM partition */
            for(int g = 0; g < part_count; g++)
            {
                if(strncmp(parts[g].identifier, "GM", 2) != 0) continue;
                if(strncmp(parts[g].identifier, gm_name, 18) == 0)
                {
                    memcpy(parts[g].key, dec_title_key, 16);
                    parts[g].has_title_key = 1;
                    keys_found++;

                    printf("  \033[32m\xe2\x9c\x93\033[0m Title key for \033[1m%s\033[0m: ", gm_name);
                    for(int k = 0; k < 12; k++) printf("%02X", dec_title_key[k]);
                    printf("****\n");
                    break;
                }
            }
        }

        free(fst_data);
        free(cluster_offsets);
    }

    /* Set disc key for all partitions that didn't get a title key */
    for(int i = 0; i < part_count; i++)
    {
        if(!parts[i].has_title_key)
            memcpy(parts[i].key, disc_key, 16);
    }

    return keys_found;
}

/* ================================================================== */
/*  Sector import                                                      */
/* ================================================================== */

/*
 * Wii U disc encryption scheme (verified against wudecrypt):
 *
 * - Sectors 0-2 (offsets 0x00000-0x17FFF): plaintext disc header area
 * - Each partition's first sector: plaintext partition header (magic 0xCC93A4F5)
 * - SI/UP/GI partition sectors: AES-128-CBC with disc key, IV=0
 * - GM partition sectors: AES-128-CBC with per-title key, IV=0
 *
 * We decrypt each sector with the correct per-partition key and store the
 * decrypted data.  On read, re-encrypting with the same key+IV restores
 * the original ciphertext byte-for-byte.
 */

int wiiu_import(struct wiiu_reader *reader, int out_fd,
                const struct wiiu_partition *parts, int part_count)
{
    /* Pre-init an AES context per partition for fast sector decryption */
    struct aes128_ctx part_aes[WIIU_MAX_PARTITIONS];
    for(int i = 0; i < part_count; i++)
        aes128_init(&part_aes[i], parts[i].key);

    /* Build sorted ranges for partition lookup:
     * Each partition occupies [start_sector .. next_partition_start_sector) */
    uint64_t part_start[WIIU_MAX_PARTITIONS];
    uint64_t part_end[WIIU_MAX_PARTITIONS];

    for(int i = 0; i < part_count; i++)
    {
        part_start[i] = parts[i].start_sector;
        part_end[i]   = reader->disc_size / WIIU_SECTOR_SIZE; /* default: to disc end */
    }
    /* Find the nearest next partition as the end boundary */
    for(int i = 0; i < part_count; i++)
    {
        for(int j = 0; j < part_count; j++)
        {
            if(parts[j].start_sector > parts[i].start_sector &&
               parts[j].start_sector < part_end[i])
                part_end[i] = parts[j].start_sector;
        }
    }

    uint64_t disc_size     = reader->disc_size;
    uint64_t total_sectors = disc_size / WIIU_SECTOR_SIZE;
    uint64_t plain_count   = 0;
    uint64_t enc_count     = 0;
    uint64_t zero_count    = 0;

    uint8_t sector_buf[WIIU_SECTOR_SIZE];
    uint8_t dec_buf[WIIU_SECTOR_SIZE];

    for(uint64_t sec = 0; sec < total_sectors; sec++)
    {
        uint64_t offset = sec * WIIU_SECTOR_SIZE;

        if((sec & 0x3FF) == 0) print_progress(offset, disc_size, "Importing Wii U disc");

        ssize_t n = wiiu_reader_pread(reader, sector_buf, WIIU_SECTOR_SIZE, offset);
        if(n < (ssize_t)WIIU_SECTOR_SIZE)
        {
            if(n > 0) memset(sector_buf + n, 0, WIIU_SECTOR_SIZE - (size_t)n);
            else      memset(sector_buf, 0, WIIU_SECTOR_SIZE);
        }

        /* Determine if this sector is plaintext */
        int is_plain = 0;

        if(sec < WIIU_HEADER_SECTORS)
        {
            is_plain = 1;
        }
        else
        {
            for(int p = 0; p < part_count; p++)
            {
                if(sec == parts[p].start_sector)
                {
                    is_plain = 1;
                    break;
                }
            }
        }

        const uint8_t *data_to_write;

        if(is_plain)
        {
            data_to_write = sector_buf;
            plain_count++;
        }
        else
        {
            /* Find which partition this sector belongs to and use its key */
            int part_idx = -1;
            for(int p = 0; p < part_count; p++)
            {
                if(sec >= part_start[p] && sec < part_end[p])
                {
                    part_idx = p;
                    break;
                }
            }

            uint8_t iv[16];
            memset(iv, 0, sizeof(iv));

            if(part_idx >= 0)
                aes128_cbc_decrypt(&part_aes[part_idx], iv, sector_buf, dec_buf, WIIU_SECTOR_SIZE);
            else
                aes128_cbc_decrypt(&part_aes[0], iv, sector_buf, dec_buf, WIIU_SECTOR_SIZE);

            data_to_write = dec_buf;
            enc_count++;
        }

        /* Check for all-zero sector */
        int all_zero = 1;
        for(int i = 0; i < WIIU_SECTOR_SIZE; i++)
        {
            if(data_to_write[i] != 0) { all_zero = 0; break; }
        }
        if(all_zero) zero_count++;

        /* Write the full 0x8000-byte sector in one call.
         * The FUSE write path splits it into 2048-byte dedup sectors internally. */
        ssize_t w = write(out_fd, data_to_write, WIIU_SECTOR_SIZE);
        if(w < (ssize_t)WIIU_SECTOR_SIZE)
        {
            fprintf(stderr, "\nError: write failed at disc offset 0x%" PRIX64 "\n", offset);
            return -1;
        }
    }

    printf("\n  \033[32m\xe2\x9c\x93\033[0m \033[1m%" PRIu64 "\033[0m plaintext, \033[1m%" PRIu64
           "\033[0m encrypted, \033[1m%" PRIu64 "\033[0m zero sectors imported\n",
           plain_count, enc_count, zero_count);
    return 0;
}
