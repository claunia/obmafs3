// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-aif
//
// --[ Description ] ----------------------------------------------------------
//
//     Import an Aaru Image Format (.aif) file into a mounted
//     OBMAFS3 filesystem via standard POSIX I/O and ioctls.
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

#include "import_aif.h"

#include <getopt.h>
#include <libgen.h>

/**
 * Print usage information.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <aif-file> <output-path>\n"
            "\n"
            "Import an Aaru Image Format (.aif) file into a mounted OBMAFS3 filesystem.\n"
            "\n"
            "Arguments:\n"
            "  <aif-file>     Path to the source .aif file\n"
            "  <output-path>  Full path for the image within the mounted filesystem\n"
            "                 (e.g. /mnt/obmafs/images/myimage)\n"
            "\n"
            "Options:\n"
            "  -h, --help     Show this help\n",
            prog);
}

/**
 * Create all intermediate directories in a path.
 * Similar to 'mkdir -p'.  Only the directory portion of the path is
 * created; the final component is assumed to be the filename.
 *
 * @param path  Full path including the filename.
 * @return 0 on success, -1 on error (errno is set).
 */
static int mkdirs(const char *path)
{
    char *buf = strdup(path);
    if(!buf) return -1;

    /* Walk the path creating each directory component */
    for(char *p = buf + 1; *p; p++)
    {
        if(*p == '/')
        {
            *p = '\0';
            if(mkdir(buf, 0755) != 0 && errno != EEXIST)
            {
                int saved = errno;
                free(buf);
                errno = saved;
                return -1;
            }
            *p = '/';
        }
    }

    free(buf);
    return 0;
}

/**
 * Entry point for import-aif.
 *
 * Opens an Aaru Image Format file, creates a new file on the mounted
 * OBMAFS3 filesystem, and imports all sector data, media tags, and
 * metadata through standard POSIX I/O and ioctls.
 */
int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {  NULL,           0, NULL,   0}
    };

    int opt;

    while((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1)
    {
        switch(opt)
        {
            case 'h':
            default:
                usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    if(optind + 2 > argc)
    {
        fprintf(stderr, "Error: expected <aif-file> and <output-path>\n");
        usage(argv[0]);
        return 1;
    }

    const char *aif_path    = argv[optind];
    const char *output_path = argv[optind + 1];

    /* ---- Open the AIF file ---- */
    fprintf(stderr, "Opening AIF: %s\n", aif_path);
    void *aaruf_ctx = aaruf_open(aif_path, false, NULL);
    if(!aaruf_ctx)
    {
        fprintf(stderr, "Error: failed to open AIF file: %s\n", aif_path);
        return 1;
    }

    /* Get image info */
    ImageInfo info;
    memset(&info, 0, sizeof(info));
    aaruf_get_image_info(aaruf_ctx, &info);

    fprintf(stderr, "  Sectors:    %" PRIu64 "\n", info.Sectors);
    fprintf(stderr, "  SectorSize: %u\n", info.SectorSize);
    fprintf(stderr, "  MediaType:  %d\n", info.MediaType);

    int is_cd = is_compact_disc_media(info.MediaType);
    fprintf(stderr, "  Image type: %s\n", is_cd ? "Compact Disc" : "Flat media image");

    /* ---- Create parent directories ---- */
    if(mkdirs(output_path) != 0)
    {
        fprintf(stderr, "Error: failed to create parent directories for '%s' (errno=%d)\n", output_path, errno);
        aaruf_close(aaruf_ctx);
        return 1;
    }

    /* ---- Create the output file ---- */
    fprintf(stderr, "Creating: %s\n", output_path);
    int fd = open(output_path, O_CREAT | O_RDWR | O_EXCL, 0644);
    if(fd < 0)
    {
        fprintf(stderr, "Error: failed to create '%s' (errno=%d: %s)\n", output_path, errno, strerror(errno));
        aaruf_close(aaruf_ctx);
        return 1;
    }

    /* ---- Convert to appropriate file type ---- */
    if(is_cd)
    {
        if(ioctl(fd, OBMAFS3_IOC_SET_CD_IMAGE) != 0)
        {
            fprintf(stderr, "Error: SET_CD_IMAGE ioctl failed (errno=%d: %s)\n", errno, strerror(errno));
            close(fd);
            unlink(output_path);
            aaruf_close(aaruf_ctx);
            return 1;
        }
        fprintf(stderr, "  File type: CompactDiscImage\n");
    }
    else
    {
        struct obmafs3_ioctl_set_media_image_arg mia;
        memset(&mia, 0, sizeof(mia));
        mia.sector_size = (uint16_t)info.SectorSize;

        if(ioctl(fd, OBMAFS3_IOC_SET_MEDIA_IMAGE, &mia) != 0)
        {
            fprintf(stderr, "Error: SET_MEDIA_IMAGE ioctl failed (errno=%d: %s)\n", errno, strerror(errno));
            close(fd);
            unlink(output_path);
            aaruf_close(aaruf_ctx);
            return 1;
        }
        fprintf(stderr, "  File type: MediaImage (sector size %u)\n", info.SectorSize);
    }

    /* ---- Import sector data ---- */
    int rc;
    if(is_cd)
        rc = import_cd_image(aaruf_ctx, fd, &info);
    else
        rc = import_flat_image(aaruf_ctx, fd, &info);

    if(rc != 0)
    {
        fprintf(stderr, "Error: import failed\n");
        close(fd);
        aaruf_close(aaruf_ctx);
        return 1;
    }

    /* ---- Import media tags ---- */
    fprintf(stderr, "Importing media tags...\n");
    import_media_tags(aaruf_ctx, fd);

    /* ---- Import metadata ---- */
    fprintf(stderr, "Importing metadata...\n");
    import_metadata(aaruf_ctx, &info, fd);

    /* Compute output base path (without extension) for sidecar files */
    size_t      base_len = strlen(output_path);
    const char *dot      = strrchr(output_path, '.');
    const char *slash    = strrchr(output_path, '/');
    if(dot && (!slash || dot > slash)) base_len = (size_t)(dot - output_path);

    /* ---- Write cue sheet for compact disc images ---- */
    if(is_cd)
    {
        fprintf(stderr, "Writing cue sheet...\n");
        write_cue_file(aaruf_ctx, &info, output_path, base_len);
    }

    /* ---- Export sidecar files ---- */
    export_sidecar_files(aaruf_ctx, output_path, base_len);

    /* ---- Cleanup ---- */
    close(fd);
    aaruf_close(aaruf_ctx);

    fprintf(stderr, "Import complete: %s -> %s\n", aif_path, output_path);
    return 0;
}
