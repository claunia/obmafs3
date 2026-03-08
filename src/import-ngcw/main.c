// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ngcw — Nintendo GameCube/Wii disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Entry point and phase orchestration for NGC/Wii disc import.
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
#include <tags.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr, "\033[1mimport-ngcw\033[0m — Nintendo GameCube/Wii disc importer for OBMAFS3\n\n");
    fprintf(stderr, "  \033[33mUsage:\033[0m %s <input.iso> <output-path-on-obmafs>\n\n", prog);
    fprintf(stderr, "  Imports a plain GameCube or Wii ISO disc image into a\n");
    fprintf(stderr, "  mounted OBMAFS3 filesystem with deduplication and junk\n");
    fprintf(stderr, "  removal. Wii partitions are decrypted on import and\n");
    fprintf(stderr, "  re-encrypted on read for byte-identical reconstruction.\n");
}

static void phase(int n, const char *desc)
{
    printf("\n\033[1;35m▶ Phase %d:\033[0m \033[1m%s\033[0m\n", n, desc);
}

static void phase_ok(const char *msg)
{
    printf("  \033[32m✓\033[0m %s\n", msg);
}

static void phase_warn(const char *msg)
{
    fprintf(stderr, "  \033[33m⚠\033[0m %s\n", msg);
}

static void phase_err(const char *msg)
{
    fprintf(stderr, "  \033[31m✗\033[0m %s\n", msg);
}

int main(int argc, char **argv)
{
    if(argc < 3)
    {
        usage(argv[0]);
        return 1;
    }

    const char *iso_path = argv[1];
    const char *out_path = argv[2];

    printf("\n\033[1;36m╔═════════════════════════════════════════════════════╗\033[0m\n");
    printf("\033[1;36m║\033[0m  \033[1mimport-ngcw\033[0m — Nintendo Disc Importer for OBMAFS3   \033[1;36m║\033[0m\n");
    printf("\033[1;36m╚═════════════════════════════════════════════════════╝\033[0m\n");

    /* ---- Phase 1: Open and identify the disc ---- */
    phase(1, "Opening source image");

    int      iso_fd;
    uint8_t  header[0x440];
    uint64_t disc_size;

    if(ngcw_open_iso(iso_path, &iso_fd, header, &disc_size) != 0) return 1;

    int disc_type = ngc_detect_disc_type(header);
    if(disc_type < 0)
    {
        phase_err("Not a valid GameCube or Wii disc image");
        close(iso_fd);
        return 1;
    }

    ngcw_print_disc_info(header, disc_type, disc_size);

    /* ---- Phase 2: Parse partitions (Wii only) ---- */
    uint16_t             part_count = 0;
    struct ngc_partition *parts     = NULL;

    if(disc_type == 1) /* Wii */
    {
        phase(2, "Reading Wii partitions");
        if(ngcw_read_partitions(iso_fd, &part_count, &parts) != 0)
        {
            close(iso_fd);
            return 1;
        }
        phase_ok("Found partitions:");
        for(int i = 0; i < part_count; i++)
        {
            const char *ptype = "\033[90munknown\033[0m";
            if(parts[i].type == 0) ptype = "\033[32mgame\033[0m";
            else if(parts[i].type == 1) ptype = "\033[33mupdate\033[0m";
            else if(parts[i].type == 2) ptype = "\033[34mchannel\033[0m";
            printf("    \033[90m%s\033[0m Partition %d: %s  \033[90moffset=0x%lX  size=%.1f MB\033[0m\n",
                   i == part_count - 1 ? "└─" : "├─", i, ptype,
                   (unsigned long)parts[i].offset,
                   (double)parts[i].data_size / (1024.0 * 1024.0));
        }
    }
    else
    {
        phase(2, "Disc structure");
        phase_ok("GameCube disc — no partitions");
    }

    /* ---- Phase 3: Create output file ---- */
    phase(3, "Creating output file");

    int out_fd = open(out_path, O_CREAT | O_RDWR | O_EXCL, 0644);
    if(out_fd < 0)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Cannot create %s: %s", out_path, strerror(errno));
        phase_err(msg);
        free(parts);
        close(iso_fd);
        return 1;
    }

    /* Set as Nintendo image */
    struct obmafs3_ioctl_set_nintendo_image_arg nia;
    memset(&nia, 0, sizeof(nia));
    nia.disc_type       = (uint8_t)disc_type;
    nia.disc_size       = disc_size;
    nia.partition_count = part_count;
    for(int i = 0; i < part_count && i < OBMAFS3_NGC_MAX_PARTITIONS; i++)
    {
        nia.partitions[i].data_offset = parts[i].data_offset;
        nia.partitions[i].data_size   = parts[i].data_size;
        memcpy(nia.partitions[i].title_key, parts[i].title_key, 16);
    }

    if(ioctl(out_fd, OBMAFS3_IOC_SET_NINTENDO_IMAGE, &nia) < 0)
    {
        phase_err("SET_NINTENDO_IMAGE ioctl failed");
        close(out_fd);
        unlink(out_path);
        free(parts);
        close(iso_fd);
        return 1;
    }

    phase_ok("File created and typed as Nintendo image");

    /* ---- Phase 4: Import sector data ---- */
    phase(4, "Importing sector data");
    struct ngcw_junk_collector junk_collector;
    ngcw_junk_collector_init(&junk_collector);

    int rc;
    if(disc_type == 0)
        rc = ngcw_import_gc(iso_fd, out_fd, header, disc_size, &junk_collector);
    else
        rc = ngcw_import_wii(iso_fd, out_fd, header, disc_size, part_count, parts, &junk_collector);

    if(rc != 0)
    {
        phase_err("Sector import failed");
        ngcw_junk_collector_free(&junk_collector);
        close(out_fd);
        free(parts);
        close(iso_fd);
        return 1;
    }

    /* ---- Phase 5b: Store junk seed map ---- */
    if(junk_collector.count > 0)
    {
        phase(5, "Storing junk seed map");
        ngcw_junk_collector_store(&junk_collector, out_fd);
    }
    ngcw_junk_collector_free(&junk_collector);

    /* ---- Phase 4c: Check for BCA sidecar file ---- */
    {
        /* Build .bca path from the ISO path */
        size_t iso_len = strlen(iso_path);
        char  *bca_path = malloc(iso_len + 5);
        if(bca_path)
        {
            memcpy(bca_path, iso_path, iso_len + 1);
            /* Find the last dot and replace the extension */
            char *dot = strrchr(bca_path, '.');
            if(dot)
                strcpy(dot, ".bca");
            else
                strcat(bca_path, ".bca");

            struct stat bca_st;
            if(stat(bca_path, &bca_st) == 0 && bca_st.st_size == 64)
            {
                printf("\n");
                phase(6, "Importing BCA sidecar");
                int bca_fd = open(bca_path, O_RDONLY);
                if(bca_fd >= 0)
                {
                    uint8_t bca_data[64];
                    if(read(bca_fd, bca_data, 64) == 64)
                    {
                        struct obmafs3_ioctl_tag_arg tag_arg;
                        memset(&tag_arg, 0, sizeof(tag_arg));
                        tag_arg.tag_type    = kDvdBCA;
                        tag_arg.data_length = 64;
                        memcpy(tag_arg.data, bca_data, 64);
                        if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag_arg) < 0)
                            phase_warn("Failed to store BCA media tag");
                        else
                            phase_ok("Imported 64-byte BCA");
                    }
                    close(bca_fd);
                }
            }
            free(bca_path);
        }
    }

    /* ---- Phase 7: Import metadata ---- */
    phase(7, "Importing metadata");
    ngcw_import_metadata(out_fd, header, disc_type);

    /* ---- Done ---- */
    close(out_fd);
    free(parts);
    close(iso_fd);

    printf("\n\033[1;32m✓ Import complete!\033[0m\n\n");
    return 0;
}
