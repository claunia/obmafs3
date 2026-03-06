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
#include "ui.h"

#include <getopt.h>
#include <libgen.h>
#include <time.h>

/**
 * Print usage information.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
            "%sUsage:%s %s [options] <aif-file> <output-path>\n"
            "\n"
            "Import an Aaru Image Format (.aif) file into a mounted OBMAFS3 filesystem.\n"
            "\n"
            "%sArguments:%s\n"
            "  %s<aif-file>%s     Path to the source .aif file\n"
            "  %s<output-path>%s  Full path for the image within the mounted filesystem\n"
            "\n"
            "%sOptions:%s\n"
            "  %s-h, --help%s     Show this help\n",
            C_BOLD, C_RESET, prog,
            C_BOLD, C_RESET,
            C_CYAN, C_RESET,
            C_CYAN, C_RESET,
            C_BOLD, C_RESET,
            C_GREEN, C_RESET);
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
    ui_init();

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
        ui_error("Expected <aif-file> and <output-path>");
        usage(argv[0]);
        return 1;
    }

    const char *aif_path    = argv[optind];
    const char *output_path = argv[optind + 1];

    ui_banner();

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* ---- Phase 1: Open source image ---- */
    ui_phase(1, "Open source image");
    ui_info("Source:", "%s", aif_path);

    void *aaruf_ctx = aaruf_open(aif_path, false, NULL);
    if(!aaruf_ctx)
    {
        ui_error("Failed to open AIF file: %s", aif_path);
        return 1;
    }

    ImageInfo info;
    memset(&info, 0, sizeof(info));
    aaruf_get_image_info(aaruf_ctx, &info);

    int is_cd = is_compact_disc_media(info.MediaType);

    ui_info("Sectors:", "%" PRIu64, info.Sectors);
    ui_info("Sector size:", "%u bytes", info.SectorSize);
    ui_info("Media type:", "%s%d%s", C_CYAN, info.MediaType, C_RESET);
    ui_info("Image type:", "%s%s%s", C_BOLD, is_cd ? "Compact Disc" : "Flat media image", C_RESET);
    ui_ok("Image opened successfully");

    /* ---- Phase 2: Create output file ---- */
    ui_phase(2, "Create output file");
    ui_info("Destination:", "%s", output_path);

    if(mkdirs(output_path) != 0)
    {
        ui_error("Failed to create parent directories (errno=%d)", errno);
        aaruf_close(aaruf_ctx);
        return 1;
    }

    int fd = open(output_path, O_CREAT | O_RDWR | O_EXCL, 0644);
    if(fd < 0)
    {
        ui_error("Failed to create '%s' (%s)", output_path, strerror(errno));
        aaruf_close(aaruf_ctx);
        return 1;
    }

    if(is_cd)
    {
        if(ioctl(fd, OBMAFS3_IOC_SET_CD_IMAGE) != 0)
        {
            ui_error("SET_CD_IMAGE ioctl failed (%s)", strerror(errno));
            close(fd);
            unlink(output_path);
            aaruf_close(aaruf_ctx);
            return 1;
        }
        ui_info("File type:", "%sCompactDiscImage%s", C_MAGENTA, C_RESET);
    }
    else
    {
        struct obmafs3_ioctl_set_media_image_arg mia;
        memset(&mia, 0, sizeof(mia));
        mia.sector_size = (uint16_t)info.SectorSize;

        if(ioctl(fd, OBMAFS3_IOC_SET_MEDIA_IMAGE, &mia) != 0)
        {
            ui_error("SET_MEDIA_IMAGE ioctl failed (%s)", strerror(errno));
            close(fd);
            unlink(output_path);
            aaruf_close(aaruf_ctx);
            return 1;
        }
        ui_info("File type:", "%sMediaImage%s (sector size %u)", C_BLUE, C_RESET, info.SectorSize);
    }
    ui_ok("Output file created");

    /* ---- Phase 3: Import sector data ---- */
    ui_phase(3, "Import sector data");

    int rc;
    if(is_cd)
        rc = import_cd_image(aaruf_ctx, fd, &info);
    else
        rc = import_flat_image(aaruf_ctx, fd, &info);

    if(rc != 0)
    {
        ui_error("Sector data import failed");
        close(fd);
        aaruf_close(aaruf_ctx);
        return 1;
    }

    /* ---- Phase 4: Import media tags ---- */
    ui_phase(4, "Import media tags");
    import_media_tags(aaruf_ctx, fd);
    ui_ok("Media tags imported");

    /* ---- Phase 5: Import metadata ---- */
    ui_phase(5, "Import metadata");
    import_metadata(aaruf_ctx, &info, fd);
    ui_ok("Metadata imported");

    /* Compute output base path (without extension) for sidecar files */
    size_t      base_len = strlen(output_path);
    const char *dot      = strrchr(output_path, '.');
    const char *slash    = strrchr(output_path, '/');
    if(dot && (!slash || dot > slash)) base_len = (size_t)(dot - output_path);

    /* ---- Phase 6: Write sidecar files ---- */
    ui_phase(6, "Export sidecar files");

    if(is_cd)
    {
        write_cue_file(aaruf_ctx, &info, output_path, base_len);
    }

    export_sidecar_files(aaruf_ctx, output_path, base_len);
    ui_ok("Sidecar export complete");

    /* ---- Cleanup & summary ---- */
    close(fd);
    aaruf_close(aaruf_ctx);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (double)(t_end.tv_sec - t_start.tv_sec) + (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    ui_summary(aif_path, output_path, elapsed, info.Sectors, info.SectorSize);
    return 0;
}
