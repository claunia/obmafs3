/*
 * This file is part of the Aaru Data Preservation Suite.
 * Copyright (c) 2019-2026 Natalia Portillo.
 * ECC algorithm from ECM(c) 2002-2011 Neill Corlett
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "defs.h"
#include "enums.h"

uint32_t edc_cd_compute(void *context, uint32_t edc, const uint8_t *src, int size, int pos);
bool     ecc_cd_check(void *context, const uint8_t *address, const uint8_t *data, const uint32_t major_count,
                      const uint32_t minor_count, const uint32_t major_mult, const uint32_t minor_inc, const uint8_t *ecc,
                      const int32_t address_offset, const int32_t data_offset, const int32_t ecc_offset);

/**
 * @brief Initializes a Compact Disc ECC context.
 *
 * Allocates and initializes a context for Compact Disc ECC calculations.
 *
 * @return Pointer to the initialized CdEccContext structure, or NULL on failure.
 */
void *ecc_cd_init()
{
    CdEccContext *context = NULL;
    uint32_t      edc = 0, i = 0, j = 0;

    context = (CdEccContext *)malloc(sizeof(CdEccContext));

    if(context == NULL) return NULL;

    context->ecc_f_table = (uint8_t *)malloc(sizeof(uint8_t) * 256);

    if(context->ecc_f_table == NULL)
    {
        free(context);
        return NULL;
    }

    context->ecc_b_table = (uint8_t *)malloc(sizeof(uint8_t) * 256);

    if(context->ecc_b_table == NULL)
    {
        free(context->ecc_f_table);
        free(context);
        return NULL;
    }

    context->edc_table = (uint32_t *)malloc(sizeof(uint32_t) * 256);

    if(context->edc_table == NULL)
    {
        free(context->ecc_f_table);
        free(context->ecc_b_table);
        free(context);
        return NULL;
    }

    for(i = 0; i < 256; i++)
    {
        edc                         = i;
        j                           = i << 1 ^ ((i & 0x80) == 0x80 ? 0x11D : 0);
        context->ecc_f_table[i]     = (uint8_t)j;
        context->ecc_b_table[i ^ j] = (uint8_t)i;
        for(j = 0; j < 8; j++) edc = edc >> 1 ^ ((edc & 1) > 0 ? 0xD8018001 : 0);
        context->edc_table[i] = edc;
    }

    context->inited_edc = true;

    return context;
}

/**
 * @brief Frees a Compact Disc ECC context and its internal tables.
 *
 * @param ctx Pointer to the CdEccContext to free.
 */
void ecc_cd_free(void *ctx)
{
    if(!ctx) return;

    CdEccContext *context = (CdEccContext *)ctx;

    free(context->ecc_f_table);
    free(context->ecc_b_table);
    free(context->edc_table);
    free(context);
}

/**
 * @brief Checks if the suffix (EDC/ECC) of a CD sector is correct (Mode 1).
 *
 * @param context Pointer to the ECC context.
 * @param sector Pointer to the sector data.
 * @return true if the suffix is correct, false otherwise.
 */
bool ecc_cd_is_suffix_correct(void *context, const uint8_t *sector)
{
    uint32_t edc;
    int      size, pos;

    if(context == NULL || sector == NULL) { return false; }

    const CdEccContext *ctx = context;
    if(!ctx->inited_edc) { return false; }

    if(sector[0x814] != 0x00 ||
       // reserved (8 bytes)
       sector[0x815] != 0x00 || sector[0x816] != 0x00 || sector[0x817] != 0x00 || sector[0x818] != 0x00 ||
       sector[0x819] != 0x00 || sector[0x81A] != 0x00 || sector[0x81B] != 0x00)
    {
        return false;
    }

    const bool correct_ecc_p = ecc_cd_check(context, sector, sector, 86, 24, 2, 86, sector, 0xC, 0x10, 0x81C);
    if(!correct_ecc_p) { return false; }

    const bool correct_ecc_q = ecc_cd_check(context, sector, sector, 52, 43, 86, 88, sector, 0xC, 0x10, 0x81C + 0xAC);
    if(!correct_ecc_q) { return false; }

    uint32_t stored_edc;
    memcpy(&stored_edc, sector + 0x810, 4);
    uint32_t calculated_edc = edc_cd_compute(context, 0, sector, 0x810, 0);

    if(stored_edc != calculated_edc) { return false; }

    return calculated_edc == stored_edc;
}

/**
 * @brief Checks if the suffix (EDC/ECC) of a CD sector is correct (Mode 2).
 *
 * @param context Pointer to the ECC context.
 * @param sector Pointer to the sector data.
 * @return true if the suffix is correct, false otherwise.
 */
bool ecc_cd_is_suffix_correct_mode2(void *context, const uint8_t *sector)
{
    uint32_t edc;
    int      size, pos;
    uint8_t  zeroaddress[4] = {0};

    if(context == NULL || sector == NULL) { return false; }

    CdEccContext *ctx = context;

    if(!ctx->inited_edc) { return false; }

    const int form2 = sector[0x12] & 0x20;

    const bool correct_ecc_p = ecc_cd_check(context, zeroaddress, sector, 86, 24, 2, 86, sector, 0, 0x10, 0x81C);
    if(!correct_ecc_p) { return false; }

    const bool correct_ecc_q =
        ecc_cd_check(context, zeroaddress, sector, 52, 43, 86, 88, sector, 0, 0x10, 0x81C + 0xAC);
    if(!correct_ecc_q) { return false; }

    uint32_t stored_edc;
    memcpy(&stored_edc, form2 ? sector + 0x92C : sector + 0x818, 4);
    const uint32_t calculated_edc = edc_cd_compute(context, 0, sector, form2 ? 0x91C : 0x808, 0x10);

    return calculated_edc == stored_edc;
}

/**
 * @brief Checks the ECC of a CD sector.
 *
 * @param context Pointer to the ECC context.
 * @param address Pointer to the address field.
 * @param data Pointer to the data field.
 * @param major_count Number of major iterations.
 * @param minor_count Number of minor iterations.
 * @param major_mult Major multiplier.
 * @param minor_inc Minor increment.
 * @param ecc Pointer to the ECC field.
 * @param address_offset Offset for the address field.
 * @param data_offset Offset for the data field.
 * @param ecc_offset Offset for the ECC field.
 * @return true if ECC is correct, false otherwise.
 */
bool ecc_cd_check(void *context, const uint8_t *address, const uint8_t *data, const uint32_t major_count,
                  const uint32_t minor_count, const uint32_t major_mult, const uint32_t minor_inc, const uint8_t *ecc,
                  const int32_t address_offset, const int32_t data_offset, const int32_t ecc_offset)
{
    if(context == NULL || address == NULL || data == NULL || ecc == NULL) { return false; }

    CdEccContext *ctx = context;

    if(!ctx->inited_edc) { return false; }

    uint32_t size = major_count * minor_count;
    for(uint32_t major = 0; major < major_count; major++)
    {
        uint32_t idx   = (major >> 1) * major_mult + (major & 1);
        uint8_t  ecc_a = 0;
        uint8_t  ecc_b = 0;
        for(uint32_t minor = 0; minor < minor_count; minor++)
        {
            uint8_t temp = idx < 4 ? address[idx + address_offset] : data[idx + data_offset - 4];
            idx += minor_inc;
            if(idx >= size) idx -= size;
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ctx->ecc_f_table[ecc_a];
        }

        ecc_a = ctx->ecc_b_table[ctx->ecc_f_table[ecc_a] ^ ecc_b];
        if(ecc[major + ecc_offset] != ecc_a || ecc[major + major_count + ecc_offset] != (ecc_a ^ ecc_b))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Writes ECC for a CD sector.
 *
 * @param context Pointer to the ECC context.
 * @param address Pointer to the address field.
 * @param data Pointer to the data field.
 * @param major_count Number of major iterations.
 * @param minor_count Number of minor iterations.
 * @param major_mult Major multiplier.
 * @param minor_inc Minor increment.
 * @param ecc Pointer to the ECC field to write.
 * @param address_offset Offset for the address field.
 * @param data_offset Offset for the data field.
 * @param ecc_offset Offset for the ECC field.
 */
void ecc_cd_write(void *context, const uint8_t *address, const uint8_t *data, const uint32_t major_count,
                  const uint32_t minor_count, const uint32_t major_mult, const uint32_t minor_inc, uint8_t *ecc,
                  const int32_t address_offset, const int32_t data_offset, const int32_t ecc_offset)
{

    if(context == NULL || address == NULL || data == NULL || ecc == NULL) { return; }

    CdEccContext *ctx = context;

    if(!ctx->inited_edc) { return; }

    uint32_t size = major_count * minor_count;
    for(uint32_t major = 0; major < major_count; major++)
    {
        uint32_t idx   = (major >> 1) * major_mult + (major & 1);
        uint8_t  ecc_a = 0;
        uint8_t  ecc_b = 0;

        for(uint32_t minor = 0; minor < minor_count; minor++)
        {
            uint8_t temp = idx < 4 ? address[idx + address_offset] : data[idx + data_offset - 4];
            idx += minor_inc;
            if(idx >= size) idx -= size;
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ctx->ecc_f_table[ecc_a];
        }

        ecc_a                                 = ctx->ecc_b_table[ctx->ecc_f_table[ecc_a] ^ ecc_b];
        ecc[major + ecc_offset]               = ecc_a;
        ecc[major + major_count + ecc_offset] = ecc_a ^ ecc_b;
    }
}

/**
 * @brief Writes ECC for a full CD sector (both P and Q ECC).
 *
 * @param context Pointer to the ECC context.
 * @param address Pointer to the address field.
 * @param data Pointer to the data field.
 * @param ecc Pointer to the ECC field to write.
 * @param address_offset Offset for the address field.
 * @param data_offset Offset for the data field.
 * @param ecc_offset Offset for the ECC field.
 */
void ecc_cd_write_sector(void *context, const uint8_t *address, const uint8_t *data, uint8_t *ecc,
                         const int32_t address_offset, const int32_t data_offset, const int32_t ecc_offset)
{

    ecc_cd_write(context, address, data, 86, 24, 2, 86, ecc, address_offset, data_offset, ecc_offset);  // P
    ecc_cd_write(context, address, data, 52, 43, 86, 88, ecc, address_offset, data_offset,
                 ecc_offset + 0xAC);  // Q
}

/**
 * @brief Converts a CD LBA (Logical Block Address) to MSF (Minute:Second:Frame) format.
 *
 * @param pos LBA position.
 * @param minute Pointer to store the minute value.
 * @param second Pointer to store the second value.
 * @param frame Pointer to store the frame value.
 */
void cd_lba_to_msf(const int64_t pos, uint8_t *minute, uint8_t *second, uint8_t *frame)
{

    *minute = (uint8_t)((pos + 150) / 75 / 60);
    *second = (uint8_t)((pos + 150) / 75 % 60);
    *frame  = (uint8_t)((pos + 150) % 75);
}

/**
 * @brief Reconstructs the prefix (sync, address, mode) of a CD sector.
 *
 * @param sector Pointer to the sector data (must be 2352 bytes).
 * @param type Track type (mode).
 * @param lba Logical Block Address.
 */
void ecc_cd_reconstruct_prefix(uint8_t *sector, const uint8_t type, const int64_t lba)
{
    uint8_t minute, second, frame;

    if(sector == NULL) return;

    //
    // Sync
    //
    sector[0x000] = 0x00;
    sector[0x001] = 0xFF;
    sector[0x002] = 0xFF;
    sector[0x003] = 0xFF;
    sector[0x004] = 0xFF;
    sector[0x005] = 0xFF;
    sector[0x006] = 0xFF;
    sector[0x007] = 0xFF;
    sector[0x008] = 0xFF;
    sector[0x009] = 0xFF;
    sector[0x00A] = 0xFF;
    sector[0x00B] = 0x00;

    cd_lba_to_msf(lba, &minute, &second, &frame);

    sector[0x00C] = (uint8_t)(((minute / 10) << 4) + minute % 10);
    sector[0x00D] = (uint8_t)(((second / 10) << 4) + second % 10);
    sector[0x00E] = (uint8_t)(((frame / 10) << 4) + frame % 10);

    switch((enum obmafs3_cd_sector_mode)type)
    {
        case kCdSectorMode1:
            //
            // Mode
            //
            sector[0x00F] = 0x01;
            break;
        case kCdSectorMode2Form1:
        case kCdSectorMode2Form2:
        case kCdSectorMode2:
            //
            // Mode
            //
            sector[0x00F] = 0x02;
            //
            // Flags
            //
            sector[0x010] = sector[0x014];
            sector[0x011] = sector[0x015];
            sector[0x012] = sector[0x016];
            sector[0x013] = sector[0x017];
            break;
        default:
            return;
    }
}

/**
 * @brief Reconstructs the EDC and ECC fields of a CD sector.
 *
 * @param context Pointer to the ECC context.
 * @param sector Pointer to the sector data (must be 2352 bytes).
 * @param type Track type (mode).
 */
void ecc_cd_reconstruct(void *context, uint8_t *sector, const uint8_t type)
{
    uint32_t computed_edc;
    uint8_t  zeroaddress[4];

    if(context == NULL || sector == NULL) { return; }

    CdEccContext *ctx = context;

    if(!ctx->inited_edc) { return; }

    switch(type)
    {
        //
        // Compute EDC
        //
        case kCdSectorMode1:
            computed_edc = edc_cd_compute(context, 0, sector, 0x810, 0);
            memcpy(sector + 0x810, &computed_edc, 4);
            break;
        case kCdSectorMode2Form1:
            computed_edc = edc_cd_compute(context, 0, sector, 0x808, 0x10);
            memcpy(sector + 0x818, &computed_edc, 4);
            break;
        case kCdSectorMode2Form2:
            computed_edc = edc_cd_compute(context, 0, sector, 0x91C, 0x10);
            memcpy(sector + 0x92C, &computed_edc, 4);
            break;
        default:
            return;
    }

    memset(&zeroaddress, 0, 4);

    switch(type)
    {
        //
        // Compute ECC
        //
        case kCdSectorMode1:
            //
            // Reserved
            //
            sector[0x814] = 0x00;
            sector[0x815] = 0x00;
            sector[0x816] = 0x00;
            sector[0x817] = 0x00;
            sector[0x818] = 0x00;
            sector[0x819] = 0x00;
            sector[0x81A] = 0x00;
            sector[0x81B] = 0x00;
            ecc_cd_write_sector(context, sector, sector, sector, 0xC, 0x10, 0x81C);
            break;
        case kCdSectorMode2Form1:
            ecc_cd_write_sector(context, zeroaddress, sector, sector, 0, 0x10, 0x81C);
            break;
        default:
            return;
    }

    //
    // Done
    //
}

/**
 * @brief Computes the EDC (Error Detection Code) for a CD sector.
 *
 * @param context Pointer to the ECC context.
 * @param edc Initial EDC value.
 * @param src Pointer to the data to compute EDC over.
 * @param size Number of bytes to process.
 * @param pos Starting position in the data.
 * @return Computed EDC value.
 */
uint32_t edc_cd_compute(void *context, uint32_t edc, const uint8_t *src, int size, int pos)
{

    if(context == NULL || src == NULL) { return 0; }

    CdEccContext *ctx = context;

    if(!ctx->inited_edc) { return 0; }

    for(; size > 0; size--) edc = edc >> 8 ^ ctx->edc_table[(edc ^ src[pos++]) & 0xFF];

    return edc;
}
