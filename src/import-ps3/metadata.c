// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : metadata.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Image metadata import for PS3 disc images.
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

#include "import_ps3.h"

#include <obmafs3_ioctl.h>

#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>

static void set_metadata(int fd, const char *key, const char *value)
{
    if(!value || !value[0]) return;
    char xattr_name[256 + 16];
    snprintf(xattr_name, sizeof(xattr_name), "user.metadata.%s", key);
    if(fsetxattr(fd, xattr_name, value, strlen(value), 0) < 0)
        fprintf(stderr, "  Warning: failed to set metadata '%s'\n", key);
}

void ps3_import_metadata(int out_fd, const char *disc_id, const struct ps3_sfo_data *sfo,
                         const struct ps3_ird_data *ird)
{
    set_metadata(out_fd, "Platform", "Sony PlayStation 3");

    /* Disc ID from sector 1 */
    if(disc_id && disc_id[0]) set_metadata(out_fd, "DiscID", disc_id);

    /* From PARAM.SFO */
    if(sfo)
    {
        const char *v;
        if((v = ps3_sfo_get(sfo, "TITLE")))          set_metadata(out_fd, "Title", v);
        if((v = ps3_sfo_get(sfo, "TITLE_ID")))        set_metadata(out_fd, "GameID", v);
        if((v = ps3_sfo_get(sfo, "PS3_SYSTEM_VER")))  set_metadata(out_fd, "FirmwareVersion", v);
        if((v = ps3_sfo_get(sfo, "VERSION")))          set_metadata(out_fd, "GameVersion", v);
        if((v = ps3_sfo_get(sfo, "APP_VER")))          set_metadata(out_fd, "AppVersion", v);
        if((v = ps3_sfo_get(sfo, "CATEGORY")))         set_metadata(out_fd, "Category", v);
        if((v = ps3_sfo_get(sfo, "CONTENT_ID")))       set_metadata(out_fd, "ContentID", v);
    }

    /* Fallback from IRD if SFO didn't have it */
    if(ird && ird->valid)
    {
        if(ird->game_id[0])   set_metadata(out_fd, "GameID", ird->game_id);
        if(ird->game_name[0]) set_metadata(out_fd, "Title", ird->game_name);
        if(ird->update_ver[0]) set_metadata(out_fd, "DiscUpdateVersion", ird->update_ver);
    }

    set_metadata(out_fd, "MediaType", "Sony PlayStation 3 Blu-ray Disc");
    set_metadata(out_fd, "AaruMediaType", "116");
    set_metadata(out_fd, "ImportApplication", "import-ps3 (OBMAFS3)");
}
