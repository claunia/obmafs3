// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : sha1.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — embedded SHA-1
//
// --[ Description ] ----------------------------------------------------------
//
//     Minimal SHA-1 implementation for Nintendo Wii disc hash verification.
//     Based on FIPS 180-4 specification.
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

#include "sha1.h"

#include <string.h>

static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for(int i = 0; i < 16; i++) w[i] = be32(block + i * 4);
    for(int i = 16; i < 80; i++) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

    for(int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if(i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        }
        else if(i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if(i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e          = d;
        d          = c;
        c          = rotl32(b, 30);
        b          = a;
        a          = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_init(struct sha1_ctx *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count    = 0;
}

void sha1_update(struct sha1_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p   = (const uint8_t *)data;
    size_t         idx = (size_t)(ctx->count & 63);
    ctx->count += len;

    if(idx)
    {
        size_t fill = 64 - idx;
        if(len < fill)
        {
            memcpy(ctx->buffer + idx, p, len);
            return;
        }
        memcpy(ctx->buffer + idx, p, fill);
        sha1_transform(ctx->state, ctx->buffer);
        p += fill;
        len -= fill;
    }
    while(len >= 64)
    {
        sha1_transform(ctx->state, p);
        p += 64;
        len -= 64;
    }
    if(len) memcpy(ctx->buffer, p, len);
}

void sha1_final(struct sha1_ctx *ctx, uint8_t digest[SHA1_DIGEST_SIZE])
{
    uint64_t bits = ctx->count * 8;
    uint8_t  pad  = 0x80;
    sha1_update(ctx, &pad, 1);

    pad = 0;
    while((ctx->count & 63) != 56) sha1_update(ctx, &pad, 1);

    uint8_t bits_be[8];
    put_be64(bits_be, bits);
    sha1_update(ctx, bits_be, 8);

    for(int i = 0; i < 5; i++) put_be32(digest + i * 4, ctx->state[i]);
}

void sha1_hash(const void *data, size_t len, uint8_t digest[SHA1_DIGEST_SIZE])
{
    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}
