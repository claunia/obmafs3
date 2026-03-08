// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : metadata.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ngcw — Nintendo GameCube/Wii disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Image metadata import for GameCube and Wii disc images.
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

void ngcw_import_metadata(int out_fd, const uint8_t *header, int disc_type)
{
    const struct ngc_disc_header *dh = (const struct ngc_disc_header *)header;
    char buf[256];

    /* Platform */
    set_metadata(out_fd, "Platform", disc_type == 0 ? "Nintendo GameCube" : "Nintendo Wii");

    /* Game ID */
    snprintf(buf, sizeof(buf), "%c%c%c%c%c%c", header[0], header[1], header[2], header[3], header[4], header[5]);
    set_metadata(out_fd, "GameID", buf);

    /* Game title */
    char title[65];
    memcpy(title, dh->game_title, 64);
    title[64] = '\0';
    /* Trim trailing spaces */
    for(int i = 63; i >= 0 && (title[i] == ' ' || title[i] == '\0'); i--) title[i] = '\0';
    if(title[0]) set_metadata(out_fd, "Title", title);

    /* Disc number */
    snprintf(buf, sizeof(buf), "%u", dh->disc_number);
    set_metadata(out_fd, "DiscNumber", buf);

    /* Disc version */
    snprintf(buf, sizeof(buf), "%u", dh->disc_version);
    set_metadata(out_fd, "DiscVersion", buf);

    /* Maker code */
    snprintf(buf, sizeof(buf), "%c%c", dh->maker_code[0], dh->maker_code[1]);
    set_metadata(out_fd, "MakerCode", buf);

    /* Region */
    const char *region;
    switch(dh->region_code)
    {
        case 'J': region = "Japan"; break;
        case 'E': region = "USA"; break;
        case 'P':
        case 'D':
        case 'F':
        case 'I':
        case 'S':
        case 'H':
        case 'U': region = "Europe"; break;
        case 'K': region = "Korea"; break;
        case 'W': region = "Taiwan"; break;
        default: region = "Unknown"; break;
    }
    set_metadata(out_fd, "Region", region);

    /* Media type */
    set_metadata(out_fd, "MediaType", disc_type == 0 ? "Nintendo GameCube Optical Disc" : "Nintendo Wii Optical Disc");

    /* Aaru Media Type */
    set_metadata(out_fd, "AaruMediaType", disc_type == 0 ? "453" : "463");

    /* Import application */
    set_metadata(out_fd, "ImportApplication", "import-ngcw (OBMAFS3)");
}
