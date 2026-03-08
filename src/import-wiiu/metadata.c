// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : metadata.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-wiiu — Nintendo Wii U disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Image metadata import for Wii U disc images.
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

#include "import_wiiu.h"

#include <obmafs3_ioctl.h>

#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>

static void set_metadata(int fd, const char *key, const char *value)
{
    char xattr_name[256 + 16];
    snprintf(xattr_name, sizeof(xattr_name), "user.metadata.%s", key);
    if(fsetxattr(fd, xattr_name, value, strlen(value), 0) < 0)
        fprintf(stderr, "  Warning: failed to set metadata '%s'\n", key);
}

void wiiu_import_metadata(int out_fd, const uint8_t *header)
{
    char buf[256];

    /* Platform */
    set_metadata(out_fd, "Platform", "Nintendo Wii U");

    /*
     * Wii U disc header layout (plaintext at offset 0):
     *   0x00..0x09: Product code (e.g. "WUP-P-ABJP")
     *   0x0A:       '-'
     *   0x0B..0x0C: Revision ("00")
     *   0x0D:       '-'
     *   0x0E..0x10: Some identifier
     *   0x11..0x14: Region ("EUR-")
     *   0x15:       Disc number ('0')
     */

    /* Product code (first 10 bytes) */
    char product_code[11];
    memcpy(product_code, header, 10);
    product_code[10] = '\0';
    set_metadata(out_fd, "ProductCode", product_code);

    /* Game ID (bytes 4-9, e.g. "P-ABJP" → "ABJP" for the title ID part) */
    if(header[3] == '-')
    {
        char game_id[7];
        memcpy(game_id, header + 4, 6);
        game_id[6] = '\0';
        set_metadata(out_fd, "GameID", game_id);
    }

    /* Full product string */
    char product_string[23];
    memcpy(product_string, header, 22);
    product_string[22] = '\0';
    for(int i = 21; i >= 0 && (product_string[i] == '\0' || product_string[i] == ' '); i--)
        product_string[i] = '\0';
    if(product_string[0]) set_metadata(out_fd, "Title", product_string);

    /* Disc number (byte at offset 0x15, ASCII digit) */
    if(header[0x15] >= '0' && header[0x15] <= '9')
    {
        snprintf(buf, sizeof(buf), "%c", header[0x15]);
        set_metadata(out_fd, "DiscNumber", buf);
    }

    /* Revision (bytes 0x0B-0x0C) */
    char revision[3];
    revision[0] = (char)header[0x0B];
    revision[1] = (char)header[0x0C];
    revision[2] = '\0';
    set_metadata(out_fd, "DiscVersion", revision);

    /* Region (bytes 0x11-0x13, e.g. "EUR", "USA", "JPN") */
    char region_code[4];
    memcpy(region_code, header + 0x11, 3);
    region_code[3] = '\0';
    const char *region;
    if(strncmp(region_code, "EUR", 3) == 0)      region = "Europe";
    else if(strncmp(region_code, "USA", 3) == 0)  region = "USA";
    else if(strncmp(region_code, "JPN", 3) == 0)  region = "Japan";
    else if(strncmp(region_code, "KOR", 3) == 0)  region = "Korea";
    else if(strncmp(region_code, "TWN", 3) == 0)  region = "Taiwan";
    else if(strncmp(region_code, "CHN", 3) == 0)  region = "China";
    else                                           region = region_code;
    set_metadata(out_fd, "Region", region);

    /* Media type */
    set_metadata(out_fd, "MediaType", "Nintendo Wii U Optical Disc");

    /* Aaru Media Type — use value for Wii U disc */
    set_metadata(out_fd, "AaruMediaType", "464");

    /* Import application */
    set_metadata(out_fd, "ImportApplication", "import-wiiu (OBMAFS3)");
}
