// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : import_aif.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-aif
//
// --[ Description ] ----------------------------------------------------------
//
//     Shared header for all translation units.
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

#ifndef IMPORT_AIF_H
#define IMPORT_AIF_H

#include "enums.h"
#include "obmafs3_ioctl.h"
#include "tags.h"

/* Prevent type collision: obmafs3 and libaaruformat both define MediaTagType, CdEccContext, and compression enums */
#define MediaTagType     AarufMediaTagType
#define CdEccContext     AarufCdEccContext
#define kCompressionNone AarufCompressionNone
#define kCompressionLzma AarufCompressionLzma
#define kCompressionZstd AarufCompressionZstd
#include <aaruformat.h>
#undef MediaTagType
#undef CdEccContext
#undef kCompressionNone
#undef kCompressionLzma
#undef kCompressionZstd

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- convert.c ---- */
int      aaruf_tag_to_obmafs(int aaruf_tag);
int      aaruf_track_type_to_cd_mode(int tt);
uint16_t cd_mode_sector_size(int mode);
int      is_compact_disc_media(uint32_t media_type);

/* ---- cuesheet.c ---- */
int write_cue_file(void *aaruf_ctx, const ImageInfo *info, const char *output_path, size_t base_len);

/* ---- metadata.c ---- */
int  import_media_tags(void *aaruf_ctx, int fd);
void import_metadata(void *aaruf_ctx, const ImageInfo *info, int fd);

/* ---- import.c ---- */
int import_flat_image(void *aaruf_ctx, int fd, const ImageInfo *info);
int import_cd_image(void *aaruf_ctx, int fd, const ImageInfo *info);
int import_sector_tags(void *aaruf_ctx, int fd, const ImageInfo *info);

/* ---- sidecar.c ---- */
void export_sidecar_files(void *aaruf_ctx, const char *output_path, size_t base_len);

#endif /* IMPORT_AIF_H */
