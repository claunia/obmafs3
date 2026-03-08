// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : nintendo.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — Nintendo GameCube/Wii disc support
//
// --[ Description ] ----------------------------------------------------------
//
//     Implementation of Nintendo GameCube/Wii disc parsing, encryption,
//     and the lagged Fibonacci junk PRNG.
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

#include "nintendo.h"

#include "aes128.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

/* ---- Wii common keys (well-known, published by the homebrew community) ---- */
const uint8_t NGC_WII_COMMON_KEY[16] = {
    0xEB, 0xE4, 0x2A, 0x22, 0x5E, 0x85, 0x93, 0xE4,
    0x48, 0xD9, 0xC5, 0x45, 0x73, 0x81, 0xAA, 0xF7
};

const uint8_t NGC_WII_KOREAN_KEY[16] = {
    0x63, 0xB8, 0x2B, 0xB4, 0xF4, 0x61, 0x4E, 0x2E,
    0x13, 0xF2, 0xFE, 0xFB, 0xBA, 0x4C, 0x9B, 0x7E
};

/* ================================================================== */
/*  Disc type detection                                                */
/* ================================================================== */

int ngc_detect_disc_type(const uint8_t header[0x440])
{
    uint32_t wii_magic = ngc_be32(header + 0x18);
    uint32_t gc_magic  = ngc_be32(header + 0x1C);
    if(wii_magic == NGC_WII_MAGIC) return 1;
    if(gc_magic == NGC_GC_MAGIC) return 0;
    return -1;
}

uint32_t ngc_get_disc_id(const uint8_t header[0x440])
{
    return ngc_be32(header);
}

/* ================================================================== */
/*  Title key decryption                                               */
/* ================================================================== */

void ngc_decrypt_title_key(const struct ngc_wii_ticket *ticket, uint8_t title_key[16])
{
    const uint8_t *common_key;
    if(ticket->common_key_index == 1)
        common_key = NGC_WII_KOREAN_KEY;
    else
        common_key = NGC_WII_COMMON_KEY;

    /* The IV for title key decryption is the title ID padded with zeroes */
    uint8_t iv[16];
    memset(iv, 0, 16);
    memcpy(iv, ticket->title_id, 8);

    struct aes128_ctx aes;
    aes128_init(&aes, common_key);
    aes128_cbc_decrypt(&aes, iv, ticket->enc_title_key, title_key, 16);
}

/* ================================================================== */
/*  Lagged Fibonacci Generator (junk/padding PRNG)                     */
/* ================================================================== */

/*
 * Based on Dolphin emulator's LaggedFibonacciGenerator (CC0 licensed).
 *
 * The GC/Wii junk PRNG is an additive lagged Fibonacci generator using XOR,
 * with parameters K=521, J=32.  The seed is 17 uint32 words (big-endian)
 * that are expanded to fill a 521-word buffer, then 4 rounds of Forward()
 * are run to mix the state.
 */

static inline uint32_t swap32(uint32_t x)
{
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}

static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void lfg_forward(struct ngc_lfg_ctx *ctx)
{
    for(size_t i = 0; i < NGC_LFG_J; i++)
        ctx->buffer[i] ^= ctx->buffer[i + NGC_LFG_K - NGC_LFG_J];
    for(size_t i = NGC_LFG_J; i < NGC_LFG_K; i++)
        ctx->buffer[i] ^= ctx->buffer[i - NGC_LFG_J];
}

static void lfg_backward(struct ngc_lfg_ctx *ctx, size_t start_word, size_t end_word)
{
    size_t loop_end = NGC_LFG_J > start_word ? NGC_LFG_J : start_word;
    size_t upper = end_word < NGC_LFG_K ? end_word : NGC_LFG_K;
    for(size_t i = upper; i > loop_end; --i)
        ctx->buffer[i - 1] ^= ctx->buffer[i - 1 - NGC_LFG_J];

    size_t upper2 = end_word < NGC_LFG_J ? end_word : NGC_LFG_J;
    for(size_t i = upper2; i > start_word; --i)
        ctx->buffer[i - 1] ^= ctx->buffer[i - 1 + NGC_LFG_K - NGC_LFG_J];
}

static bool lfg_initialize(struct ngc_lfg_ctx *ctx, bool check_existing)
{
    for(size_t i = NGC_LFG_SEED_SIZE; i < NGC_LFG_K; i++)
    {
        uint32_t calculated = (ctx->buffer[i - 17] << 23) ^ (ctx->buffer[i - 16] >> 9) ^ ctx->buffer[i - 1];

        if(check_existing)
        {
            uint32_t actual = (ctx->buffer[i] & 0xFF00FFFF) | (ctx->buffer[i] << 2 & 0x00FC0000);
            if((calculated & 0xFFFCFFFF) != actual) return false;
        }

        ctx->buffer[i] = calculated;
    }

    /* Apply the shift-by-18-instead-of-16 quirk + byteswap */
    for(size_t i = 0; i < NGC_LFG_K; i++)
        ctx->buffer[i] = swap32((ctx->buffer[i] & 0xFF00FFFF) | ((ctx->buffer[i] >> 2) & 0x00FF0000));

    for(int i = 0; i < 4; i++)
        lfg_forward(ctx);

    return true;
}

void ngc_lfg_set_seed(struct ngc_lfg_ctx *ctx, const uint32_t seed[NGC_LFG_SEED_SIZE])
{
    ctx->position_bytes = 0;

    for(size_t i = 0; i < NGC_LFG_SEED_SIZE; i++)
        ctx->buffer[i] = swap32(seed[i]);

    lfg_initialize(ctx, false);
}

void ngc_lfg_get_bytes(struct ngc_lfg_ctx *ctx, uint8_t *out, size_t count)
{
    while(count > 0)
    {
        size_t avail = NGC_LFG_K * sizeof(uint32_t) - ctx->position_bytes;
        size_t chunk = count < avail ? count : avail;

        memcpy(out, (uint8_t *)ctx->buffer + ctx->position_bytes, chunk);

        ctx->position_bytes += chunk;
        count -= chunk;
        out += chunk;

        if(ctx->position_bytes == NGC_LFG_K * sizeof(uint32_t))
        {
            lfg_forward(ctx);
            ctx->position_bytes = 0;
        }
    }
}

static bool lfg_reinitialize(struct ngc_lfg_ctx *ctx, uint32_t seed_out[NGC_LFG_SEED_SIZE])
{
    for(int i = 0; i < 4; i++)
        lfg_backward(ctx, 0, NGC_LFG_K);

    for(size_t i = 0; i < NGC_LFG_K; i++)
        ctx->buffer[i] = swap32(ctx->buffer[i]);

    /* Reconstruct bits lost by the shift-by-18-instead-of-16 quirk */
    for(size_t i = 0; i < NGC_LFG_SEED_SIZE; i++)
    {
        ctx->buffer[i] = (ctx->buffer[i] & 0xFF00FFFF) | (ctx->buffer[i] << 2 & 0x00FC0000) |
                          ((ctx->buffer[i + 16] ^ ctx->buffer[i + 15]) << 9 & 0x00030000);
    }

    for(size_t i = 0; i < NGC_LFG_SEED_SIZE; i++)
        seed_out[i] = swap32(ctx->buffer[i]);

    return lfg_initialize(ctx, true);
}

size_t ngc_lfg_get_seed(const uint8_t *data, size_t size, size_t data_offset,
                        uint32_t seed_out[NGC_LFG_SEED_SIZE])
{
    /* Alignment: data - data_offset must be 4-byte aligned */
    if(((uintptr_t)data - data_offset) % sizeof(uint32_t) != 0) return 0;

    /* Work on whole u32 words */
    size_t bytes_to_skip = ((data_offset + 3) & ~(size_t)3) - data_offset;
    if(bytes_to_skip > size) return 0;
    const uint32_t *u32_data = (const uint32_t *)(data + bytes_to_skip);
    size_t u32_size = (size - bytes_to_skip) / sizeof(uint32_t);
    size_t u32_data_offset = (data_offset + bytes_to_skip) / sizeof(uint32_t);

    if(u32_size < NGC_LFG_K) return 0;

    /* Quick sanity check: the top bits have a specific pattern from the shift quirk */
    for(size_t i = 0; i < NGC_LFG_K; i++)
    {
        uint32_t x = swap32(u32_data[i]);
        if((x & 0x00C00000) != (x >> 2 & 0x00C00000)) return 0;
    }

    struct ngc_lfg_ctx lfg;
    size_t data_offset_mod_k = u32_data_offset % NGC_LFG_K;
    size_t data_offset_div_k = u32_data_offset / NGC_LFG_K;

    /* Place the data into the buffer at the correct position.
     * Copy raw native-endian u32 values — NO byte-swapping here.
     * The swap happens later inside Reinitialize/Initialize. */
    size_t first_part = NGC_LFG_K - data_offset_mod_k;
    if(first_part > NGC_LFG_K) first_part = NGC_LFG_K;
    for(size_t i = 0; i < first_part && i < NGC_LFG_K; i++)
        lfg.buffer[data_offset_mod_k + i] = u32_data[i];
    for(size_t i = 0; i < data_offset_mod_k; i++)
        lfg.buffer[i] = u32_data[first_part + i];

    lfg_backward(&lfg, 0, data_offset_mod_k);

    for(size_t i = 0; i < data_offset_div_k; i++)
        lfg_backward(&lfg, 0, NGC_LFG_K);

    if(!lfg_reinitialize(&lfg, seed_out)) return 0;

    lfg.position_bytes = data_offset % (NGC_LFG_K * sizeof(uint32_t));

    /*
     * Advance the LFG forward to match the data_offset position.
     * After Reinitialize, the LFG is at stream position 0.
     * We need to advance to u32_data_offset words = data_offset bytes
     * (after alignment adjustment).
     *
     * position_bytes handles the sub-buffer offset.
     * We also need (u32_data_offset / LFG_K) Forward() calls for
     * full buffer cycles, but Reinitialize already did those
     * (via the Backward+Reinitialize+implicit forward path).
     * However, the internal Forward calls in Initialize handle
     * the initial 4 rounds, and the Backward calls reversed
     * data_offset_div_k + 4 forward rounds. After Reinitialize,
     * we're back at position 0 with 4 forward rounds done.
     * We need data_offset_div_k more Forward() calls.
     */
    for(size_t i = 0; i < data_offset_div_k; i++)
        lfg_forward(&lfg);

    /* Count how many bytes from data match the LFG output */
    size_t result = 0;
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    while(p < end)
    {
        uint8_t expected = ((uint8_t *)lfg.buffer)[lfg.position_bytes];
        if(*p != expected) break;
        result++;
        p++;
        lfg.position_bytes++;
        if(lfg.position_bytes == NGC_LFG_K * sizeof(uint32_t))
        {
            lfg_forward(&lfg);
            lfg.position_bytes = 0;
        }
    }

    return result;
}

/* ================================================================== */
/*  FST / data region map                                              */
/* ================================================================== */

static int data_map_add(struct ngc_data_map *map, uint64_t offset, uint64_t length)
{
    if(length == 0) return 0;

    if(map->count >= map->capacity)
    {
        uint32_t new_cap = map->capacity ? map->capacity * 2 : 256;
        struct ngc_data_region *nr = realloc(map->regions, new_cap * sizeof(*nr));
        if(!nr) return -1;
        map->regions  = nr;
        map->capacity = new_cap;
    }
    map->regions[map->count].offset = offset;
    map->regions[map->count].length = length;
    map->count++;
    return 0;
}

static int region_cmp(const void *a, const void *b)
{
    const struct ngc_data_region *ra = (const struct ngc_data_region *)a;
    const struct ngc_data_region *rb = (const struct ngc_data_region *)b;
    if(ra->offset < rb->offset) return -1;
    if(ra->offset > rb->offset) return 1;
    return 0;
}

int ngc_build_data_map(const uint8_t *fst, uint32_t fst_size, uint64_t data_start_offset,
                       int address_shift, struct ngc_data_map *map)
{
    if(!fst || fst_size < sizeof(struct ngc_fst_entry)) return -1;

    memset(map, 0, sizeof(*map));

    /* First entry is the root directory */
    const struct ngc_fst_entry *entries = (const struct ngc_fst_entry *)fst;
    uint32_t total_entries = ngc_be32((const uint8_t *)&entries[0].file_length);

    if(total_entries * sizeof(struct ngc_fst_entry) > fst_size) return -1;

    for(uint32_t i = 1; i < total_entries; i++)
    {
        if(entries[i].type == 0) /* file */
        {
            uint64_t off = (uint64_t)ngc_be32((const uint8_t *)&entries[i].file_offset) << address_shift;
            uint64_t len = ngc_be32((const uint8_t *)&entries[i].file_length);
            if(data_map_add(map, data_start_offset + off, len) < 0)
            {
                ngc_data_map_free(map);
                return -1;
            }
        }
    }

    /* Sort by offset for binary search */
    if(map->count > 1) qsort(map->regions, map->count, sizeof(map->regions[0]), region_cmp);

    return 0;
}

void ngc_data_map_free(struct ngc_data_map *map)
{
    free(map->regions);
    memset(map, 0, sizeof(*map));
}

bool ngc_is_data_region(const struct ngc_data_map *map, uint64_t offset, uint64_t length)
{
    /* Binary search for first region that could overlap */
    int lo = 0, hi = (int)map->count - 1;
    while(lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        uint64_t end = map->regions[mid].offset + map->regions[mid].length;
        if(end <= offset)
            lo = mid + 1;
        else if(map->regions[mid].offset >= offset + length)
            hi = mid - 1;
        else
            return true; /* overlap */
    }
    return false;
}

/* ================================================================== */
/*  Wii group encryption / decryption                                  */
/* ================================================================== */

void ngc_wii_decrypt_group(const uint8_t title_key[16], uint64_t group_offset, const uint8_t *in,
                           uint8_t *hash_block, uint8_t *data_out)
{
    (void)group_offset;

    struct aes128_ctx aes;
    aes128_init(&aes, title_key);

    /* Hash block: first 0x400 bytes, IV = all zeroes */
    uint8_t iv[16];
    memset(iv, 0, 16);
    aes128_cbc_decrypt(&aes, iv, in, hash_block, 0x400);

    /* Data block: next 0x7C00 bytes.
     * IV = bytes 0x3D0..0x3DF of the ENCRYPTED input (not the decrypted hash block).
     * This matches Dolphin's VolumeWii::DecryptBlockData. */
    uint8_t data_iv[16];
    memcpy(data_iv, in + 0x3D0, 16);
    aes128_cbc_decrypt(&aes, data_iv, in + 0x400, data_out, 0x7C00);
}

void ngc_wii_encrypt_group(const uint8_t title_key[16], uint64_t group_offset, const uint8_t *hash_block,
                           const uint8_t *data_in, uint8_t *out)
{
    (void)group_offset;

    struct aes128_ctx aes;
    aes128_init(&aes, title_key);

    /* Hash block: first 0x400 bytes, IV = all zeroes */
    uint8_t iv[16];
    memset(iv, 0, 16);
    aes128_cbc_encrypt(&aes, iv, hash_block, out, 0x400);

    /* Data block: next 0x7C00 bytes.
     * IV = bytes 0x3D0..0x3DF of the ENCRYPTED hash output (just written to out).
     * This matches Dolphin's VolumeWii::EncryptBlock. */
    uint8_t data_iv[16];
    memcpy(data_iv, out + 0x3D0, 16);
    aes128_cbc_encrypt(&aes, data_iv, data_in, out + 0x400, 0x7C00);
}
