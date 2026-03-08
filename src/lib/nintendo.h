// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : nintendo.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3 library — Nintendo GameCube/Wii disc support
//
// --[ Description ] ----------------------------------------------------------
//
//     Structures and functions for parsing, importing, and reconstructing
//     Nintendo GameCube and Wii optical disc images.
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

#ifndef OBMAFS3_NINTENDO_H
#define OBMAFS3_NINTENDO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Disc magic values ---- */
#define NGC_GC_MAGIC  0xC2339F3D  ///< GameCube magic at offset 0x1C
#define NGC_WII_MAGIC 0x5D1C9EA3  ///< Wii magic at offset 0x18

/* ---- Disc sizes ---- */
#define NGC_GC_DISC_SIZE       0x57058000ULL   ///< GameCube mini-DVD (1.46 GB)
#define NGC_WII_SL_DISC_SIZE   0x118240000ULL  ///< Wii single layer (4.7 GB)
#define NGC_WII_DL_DISC_SIZE   0x1FB4E0000ULL  ///< Wii dual layer (8.54 GB)

/* ---- Wii common keys ---- */
extern const uint8_t NGC_WII_COMMON_KEY[16];
extern const uint8_t NGC_WII_KOREAN_KEY[16];

/* ---- Disc header (first 0x440 bytes) ---- */
struct __attribute__((packed)) ngc_disc_header
{
    uint8_t  disc_id;          ///< Game type
    uint8_t  game_code[2];     ///< Game code
    uint8_t  region_code;      ///< Region code
    uint8_t  maker_code[2];    ///< Maker code
    uint8_t  disc_number;      ///< Disc number
    uint8_t  disc_version;     ///< Disc version
    uint8_t  audio_streaming;  ///< Audio streaming flag
    uint8_t  stream_buf_size;  ///< Streaming buffer size
    uint8_t  unused1[14];
    uint32_t wii_magic;        ///< 0x5D1C9EA3 for Wii
    uint32_t gc_magic;         ///< 0xC2339F3D for GameCube
    char     game_title[64];   ///< Game title (NUL-terminated)
    uint8_t  disable_hash_verification; ///< Disable hash verification (Wii)
    uint8_t  disable_disc_encryption;   ///< Disable disc encryption (Wii)
    uint8_t  padding[0x39A];   ///< Padding to 0x440
};

/* ---- Wii partition table ---- */
struct __attribute__((packed)) ngc_wii_part_info
{
    uint32_t count;    ///< Number of partitions (big-endian)
    uint32_t offset;   ///< Offset to table entries >> 2 (big-endian)
};

struct __attribute__((packed)) ngc_wii_part_entry
{
    uint32_t offset;   ///< Partition offset >> 2 (big-endian)
    uint32_t type;     ///< 0 = game, 1 = update, 2 = channel (big-endian)
};

/* ---- Wii ticket ---- */
struct __attribute__((packed)) ngc_wii_ticket
{
    uint8_t  sig_type[4];      ///< Signature type
    uint8_t  sig[256];         ///< RSA-2048 signature
    uint8_t  sig_padding[60];  ///< Padding
    uint8_t  sig_issuer[64];   ///< Signature issuer
    uint8_t  ecdh[60];         ///< ECDH data
    uint8_t  ecdh_padding[3];
    uint8_t  enc_title_key[16]; ///< Encrypted title key (AES-128-CBC)
    uint8_t  unknown1;
    uint8_t  ticket_id[8];
    uint8_t  console_id[4];
    uint8_t  title_id[8];      ///< Title ID (bytes 0-3 = unused padding, 4 selects common key)
    uint8_t  unknown2[2];
    uint16_t ticket_title_version;
    uint32_t permitted_titles;
    uint32_t permit_mask;
    uint8_t  title_export;
    uint8_t  common_key_index; ///< 0 = standard, 1 = Korean
    uint8_t  unknown3[48];
    uint8_t  content_access_permissions[64];
    uint8_t  padding2[2];
    uint8_t  time_limits[256];
};

/* ---- Wii partition context (in-memory) ---- */
struct ngc_partition
{
    uint64_t offset;      ///< Partition byte offset on disc
    uint64_t data_offset; ///< Absolute byte offset where partition data starts
    uint64_t data_size;   ///< Size of partition data in bytes
    uint32_t type;        ///< Partition type (0=game, 1=update, 2=channel)
    uint8_t  title_key[16]; ///< Decrypted AES-128 title key
};

/* ---- FSTEntry (GameCube & Wii) ---- */
struct __attribute__((packed)) ngc_fst_entry
{
    uint8_t  type;            ///< 0 = file, 1 = directory
    uint8_t  name_offset[3];  ///< 24-bit offset into string table
    uint32_t file_offset;     ///< File: offset>>2. Dir: parent index
    uint32_t file_length;     ///< File: length. Dir: next entry index
};

/* ---- Data region map ---- */
struct ngc_data_region
{
    uint64_t offset;  ///< Start byte offset (within partition data for Wii, disc for GC)
    uint64_t length;  ///< Length in bytes
};

struct ngc_data_map
{
    struct ngc_data_region *regions;
    uint32_t                count;
    uint32_t                capacity;
};

/* ---- Junk/padding detection via Lagged Fibonacci Generator ---- */

/*
 * GC/Wii discs fill unused space with a deterministic PRNG output generated
 * by a Lagged Fibonacci Generator (LFG) with parameters K=521, J=32, XOR.
 *
 * The LFG state is a 521-entry uint32 buffer.  The seed is 17 uint32 words
 * stored big-endian.  After seeding, the buffer is extended to 521 words and
 * 4 rounds of forward advancement are performed.
 *
 * Based on Dolphin emulator's LaggedFibonacciGenerator (CC0 licensed).
 */

#define NGC_LFG_K         521
#define NGC_LFG_J         32
#define NGC_LFG_SEED_SIZE 17

struct ngc_lfg_ctx
{
    uint32_t buffer[NGC_LFG_K];
    size_t   position_bytes;
};

/** Initialise the LFG from a 17-word big-endian seed. */
void ngc_lfg_set_seed(struct ngc_lfg_ctx *ctx, const uint32_t seed[NGC_LFG_SEED_SIZE]);

/** Generate count bytes of junk data into out. */
void ngc_lfg_get_bytes(struct ngc_lfg_ctx *ctx, uint8_t *out, size_t count);

/**
 * Try to extract the LFG seed from a chunk of data.
 *
 * data:        pointer to the data to test
 * size:        number of bytes available
 * data_offset: the byte offset of data[0] within its LFG block (0x8000-aligned for Wii,
 *              arbitrary for GC — typically data_offset = disc_offset % (LFG_K * 4))
 * seed_out:    if successful, receives the 17-word seed (big-endian)
 *
 * Returns the number of consecutive bytes from data[0] that match the LFG.
 * Returns 0 if the data does not look like junk.
 */
size_t ngc_lfg_get_seed(const uint8_t *data, size_t size, size_t data_offset,
                        uint32_t seed_out[NGC_LFG_SEED_SIZE]);

/* ---- Disc parsing ---- */

/** Read big-endian uint32 from buffer. */
static inline uint32_t ngc_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/** Read big-endian uint16 from buffer. */
static inline uint16_t ngc_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/** Detect whether an ISO is GameCube (returns 0), Wii (returns 1), or unknown (returns -1). */
int ngc_detect_disc_type(const uint8_t header[0x440]);

/** Extract the 4-byte disc ID as a uint32 (first 4 bytes of header). */
uint32_t ngc_get_disc_id(const uint8_t header[0x440]);

/** Decrypt a Wii title key from a ticket using the appropriate common key. */
void ngc_decrypt_title_key(const struct ngc_wii_ticket *ticket, uint8_t title_key[16]);

/**
 * Build data region map from a GameCube/Wii FST.
 * For GC: offsets are direct byte offsets (address_shift = 0).
 * For Wii: offsets are shifted left by 2 (address_shift = 2).
 */
int ngc_build_data_map(const uint8_t *fst, uint32_t fst_size, uint64_t data_start_offset,
                       int address_shift, struct ngc_data_map *map);

/** Free data region map. */
void ngc_data_map_free(struct ngc_data_map *map);

/** Check whether a byte offset falls within a data region. */
bool ngc_is_data_region(const struct ngc_data_map *map, uint64_t offset, uint64_t length);

/* ---- Wii group encryption/decryption ---- */

/**
 * Decrypt a Wii group (0x8000 bytes) in-place.
 * Input:  0x8000 bytes of encrypted data from disc.
 * Output: 0x400 bytes hash block + 0x7C00 bytes user data.
 */
void ngc_wii_decrypt_group(const uint8_t title_key[16], uint64_t group_offset, const uint8_t *in,
                           uint8_t *hash_block, uint8_t *data_out);

/**
 * Encrypt a Wii group (produce 0x8000 bytes ready for disc).
 * Input:  0x400 bytes hash block + 0x7C00 bytes user data.
 * Output: 0x8000 bytes encrypted data.
 */
void ngc_wii_encrypt_group(const uint8_t title_key[16], uint64_t group_offset, const uint8_t *hash_block,
                           const uint8_t *data_in, uint8_t *out);

#endif /* OBMAFS3_NINTENDO_H */
