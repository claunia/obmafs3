// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : aes128.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — embedded AES-128
//
// --[ Description ] ----------------------------------------------------------
//
//     Minimal AES-128-CBC encrypt/decrypt for Nintendo Wii disc support.
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

#ifndef OBMAFS3_AES128_H
#define OBMAFS3_AES128_H

#include <stddef.h>
#include <stdint.h>

#define AES128_KEY_SIZE   16
#define AES128_BLOCK_SIZE 16
#define AES128_ROUND_KEYS 176  /* 11 round keys × 16 bytes */

struct aes128_ctx
{
    uint8_t round_keys[AES128_ROUND_KEYS];
    uint8_t dec_round_keys[AES128_ROUND_KEYS];
};

void aes128_init(struct aes128_ctx *ctx, const uint8_t key[AES128_KEY_SIZE]);
void aes128_cbc_decrypt(const struct aes128_ctx *ctx, const uint8_t iv[AES128_BLOCK_SIZE], const uint8_t *in,
                        uint8_t *out, size_t len);
void aes128_cbc_encrypt(const struct aes128_ctx *ctx, const uint8_t iv[AES128_BLOCK_SIZE], const uint8_t *in,
                        uint8_t *out, size_t len);

#endif /* OBMAFS3_AES128_H */
