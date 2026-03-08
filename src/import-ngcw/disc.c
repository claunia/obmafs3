// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : disc.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ngcw — Nintendo GameCube/Wii disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Disc opening and identification.
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int ngcw_open_iso(const char *path, int *fd, uint8_t *header, uint64_t *disc_size)
{
    *fd = open(path, O_RDONLY);
    if(*fd < 0)
    {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if(fstat(*fd, &st) < 0)
    {
        fprintf(stderr, "Error: cannot stat %s: %s\n", path, strerror(errno));
        close(*fd);
        return -1;
    }
    *disc_size = (uint64_t)st.st_size;

    ssize_t n = pread(*fd, header, 0x440, 0);
    if(n < 0x440)
    {
        fprintf(stderr, "Error: cannot read disc header from %s\n", path);
        close(*fd);
        return -1;
    }

    return 0;
}

void ngcw_print_disc_info(const uint8_t *header, int disc_type, uint64_t disc_size)
{
    const struct ngc_disc_header *dh = (const struct ngc_disc_header *)header;

    const char *platform = disc_type == 0 ? "Nintendo GameCube" : "Nintendo Wii";
    const char *icon     = disc_type == 0 ? "\xF0\x9F\x8E\xAE" : "\xF0\x9F\x8E\xAE"; /* 🎮 */

    printf("\n  %s \033[1;36m%s\033[0m disc\n", icon, platform);
    printf("  \033[90m├─\033[0m Game ID:    \033[1;33m%c%c%c%c%c%c\033[0m\n",
           header[0], header[1], header[2], header[3], header[4], header[5]);

    char title[65];
    memcpy(title, dh->game_title, 64);
    title[64] = '\0';
    for(int i = 63; i >= 0 && (title[i] == ' ' || title[i] == '\0'); i--) title[i] = '\0';
    printf("  \033[90m├─\033[0m Title:      \033[1m%s\033[0m\n", title);

    printf("  \033[90m├─\033[0m Disc:       v%u #%u\n", dh->disc_version, dh->disc_number);
    printf("  \033[90m├─\033[0m Maker:      %c%c\n", dh->maker_code[0], dh->maker_code[1]);
    printf("  \033[90m└─\033[0m Size:       \033[32m%.2f GB\033[0m (%lu bytes)\n\n",
           disc_size / (1024.0 * 1024.0 * 1024.0), (unsigned long)disc_size);
}
