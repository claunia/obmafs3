/*
 * import-aif - Import an Aaru Image Format (.aif) file into a mounted
 *              OBMAFS3 filesystem via standard POSIX I/O and ioctls.
 *
 * Shared header for all translation units.
 */

#ifndef IMPORT_AIF_H
#define IMPORT_AIF_H

#include "enums.h"
#include "obmafs3_ioctl.h"
#include "tags.h"

/* Prevent type collision: obmafs3 and libaaruformat both define MediaTagType and CdEccContext */
#define MediaTagType AarufMediaTagType
#define CdEccContext AarufCdEccContext
#include <aaruformat.h>
#undef MediaTagType
#undef CdEccContext

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
int         aaruf_tag_to_obmafs(int aaruf_tag);
int         aaruf_track_type_to_cd_mode(int tt);
uint16_t    cd_mode_sector_size(int mode);
int         is_compact_disc_media(uint32_t media_type);

/* ---- cuesheet.c ---- */
int         write_cue_file(void *aaruf_ctx, const ImageInfo *info,
                           const char *output_path, size_t base_len);

/* ---- metadata.c ---- */
int         import_media_tags(void *aaruf_ctx, int fd);
void        import_metadata(void *aaruf_ctx, const ImageInfo *info, int fd);

/* ---- import.c ---- */
int         import_flat_image(void *aaruf_ctx, int fd, const ImageInfo *info);
int         import_cd_image(void *aaruf_ctx, int fd, const ImageInfo *info);

/* ---- sidecar.c ---- */
void        export_sidecar_files(void *aaruf_ctx, const char *output_path, size_t base_len);

#endif /* IMPORT_AIF_H */
