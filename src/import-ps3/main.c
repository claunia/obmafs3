// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-ps3 — PlayStation 3 disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Entry point and phase orchestration for PS3 disc import.
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
    fprintf(stderr, "\033[1mimport-ps3\033[0m \xe2\x80\x94 PlayStation 3 disc importer for OBMAFS3\n\n");
    fprintf(stderr, "  \033[33mUsage:\033[0m %s <input.iso> <output-path-on-obmafs>\n", prog);
    fprintf(stderr, "         %s <input.iso> <key-or-ird-file> <output-path-on-obmafs>\n\n", prog);
    fprintf(stderr, "  If <key-or-ird-file> is omitted, auto-searches for:\n");
    fprintf(stderr, "    <basename>.ird, <basename>.d1, <basename>.key, <basename>.dkey\n");
}

static void phase(int n, const char *desc)
{
    printf("\n\033[1;35m\xe2\x96\xb6 Phase %d:\033[0m \033[1m%s\033[0m\n", n, desc);
}

static void phase_ok(const char *msg) { printf("  \033[32m\xe2\x9c\x93\033[0m %s\n", msg); }
static void phase_err(const char *msg) { fprintf(stderr, "  \033[31m\xe2\x9c\x97\033[0m %s\n", msg); }

static int load_d1_file(const char *path, uint8_t d1[16])
{
    int fd = open(path, O_RDONLY);
    if(fd < 0) return -1;

    struct stat st;
    if(fstat(fd, &st) < 0 || st.st_size < 16) { close(fd); return -1; }

    /* Check if it's a hex text file (32 hex chars = dkey format) */
    if(st.st_size >= 32 && st.st_size <= 34)
    {
        char hex[33];
        if(read(fd, hex, 32) == 32)
        {
            hex[32] = '\0';
            int valid_hex = 1;
            for(int i = 0; i < 32 && valid_hex; i++)
            {
                char c = hex[i];
                if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                    valid_hex = 0;
            }
            if(valid_hex)
            {
                /* It's a hex-encoded disc key (dkey file) — this IS the disc key, not d1 */
                for(int i = 0; i < 16; i++)
                {
                    unsigned int byte;
                    sscanf(hex + i * 2, "%02x", &byte);
                    d1[i] = (uint8_t)byte;
                }
                close(fd);
                return 1; /* return 1 = already a disc key, skip derivation */
            }
        }
        lseek(fd, 0, SEEK_SET);
    }

    /* Raw 16-byte d1 */
    if(read(fd, d1, 16) != 16) { close(fd); return -1; }
    close(fd);
    return 0; /* return 0 = d1, needs derivation */
}

static char *derive_sidecar_path(const char *iso_path, const char *ext)
{
    size_t len  = strlen(iso_path);
    char  *path = malloc(len + 8);
    if(!path) return NULL;
    memcpy(path, iso_path, len + 1);
    char *dot = strrchr(path, '.');
    char *sep = strrchr(path, '/');
    if(dot && (!sep || dot > sep))
        strcpy(dot + 1, ext);
    else
    {
        strcat(path, ".");
        strcat(path, ext);
    }
    return path;
}

int main(int argc, char **argv)
{
    if(argc < 3) { usage(argv[0]); return 1; }

    const char *iso_path     = argv[1];
    const char *key_path     = NULL;
    const char *out_path     = NULL;
    char       *auto_key     = NULL;
    int         key_is_dkey  = 0;

    struct ps3_ird_data ird;
    memset(&ird, 0, sizeof(ird));
    int have_ird = 0;

    if(argc >= 4)
    {
        key_path = argv[2];
        out_path = argv[3];
    }
    else
    {
        out_path = argv[2];
        /* Auto-detect sidecar: .ird, .d1, .key, .dkey */
        static const char *exts[] = {"ird", "d1", "key", "dkey"};
        for(int i = 0; i < 4; i++)
        {
            auto_key = derive_sidecar_path(iso_path, exts[i]);
            if(auto_key)
            {
                struct stat st;
                if(stat(auto_key, &st) == 0 && st.st_size >= 16) { key_path = auto_key; break; }
                free(auto_key);
                auto_key = NULL;
            }
        }
        if(!key_path)
        {
            fprintf(stderr, "Error: no key file found. Searched for .ird, .d1, .key, .dkey sidecars\n");
            usage(argv[0]);
            return 1;
        }
    }

    printf("\n\033[1;36m\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x97\033[0m\n");
    printf("\033[1;36m\xe2\x95\x91\033[0m  \033[1mimport-ps3\033[0m"
           " \xe2\x80\x94 PS3 Disc Importer for OBMAFS3           \033[1;36m\xe2\x95\x91\033[0m\n");
    printf("\033[1;36m\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x9d\033[0m\n");

    /* ---- Phase 1: Open ISO and read disc structure ---- */
    phase(1, "Opening source image");

    int iso_fd = open(iso_path, O_RDONLY);
    if(iso_fd < 0) { phase_err("Cannot open ISO"); free(auto_key); return 1; }

    struct ps3_region_map region_map;
    if(ps3_read_region_map(iso_fd, &region_map) != 0) { close(iso_fd); free(auto_key); return 1; }

    char disc_id[64] = {0};
    ps3_read_disc_info(iso_fd, disc_id, sizeof(disc_id));

    printf("  \xF0\x9F\x8E\xAE \033[1;36mPlayStation 3\033[0m disc\n");
    printf("  \033[90m\xe2\x94\x9c\xe2\x94\x80\033[0m Disc ID:    \033[1;33m%s\033[0m\n", disc_id);
    printf("  \033[90m\xe2\x94\x9c\xe2\x94\x80\033[0m Size:       \033[32m%.2f GB\033[0m (%" PRIu64 " sectors)\n",
           (double)(region_map.total_sectors * PS3_SECTOR_SIZE) / (1024.0 * 1024.0 * 1024.0),
           region_map.total_sectors);
    printf("  \033[90m\xe2\x94\x94\xe2\x94\x80\033[0m Regions:    %u plaintext\n", region_map.count);

    for(uint32_t i = 0; i < region_map.count; i++)
        printf("       \033[90m%s\033[0m Plain: sectors 0x%X..0x%X\n",
               i == region_map.count - 1 ? "\xe2\x94\x94\xe2\x94\x80" : "\xe2\x94\x9c\xe2\x94\x80",
               region_map.regions[i].start_sector, region_map.regions[i].end_sector);

    /* ---- Phase 2: Load keys ---- */
    phase(2, "Loading keys");

    uint8_t d1[16]       = {0};
    uint8_t disc_key[16] = {0};

    /* Check if key_path is an IRD */
    if(ps3_parse_ird(key_path, &ird) == 0 && ird.valid)
    {
        have_ird = 1;
        memcpy(d1, ird.d1, 16);
        ps3_derive_disc_key(d1, disc_key);
        printf("  \033[32m\xe2\x9c\x93\033[0m IRD loaded: %s\n", ird.game_name);
        printf("  \033[32m\xe2\x9c\x93\033[0m d1: ");
        for(int i = 0; i < 12; i++) printf("%02X", d1[i]);
        printf("****\n");
    }
    else
    {
        int rc = load_d1_file(key_path, d1);
        if(rc < 0) { phase_err("Cannot load key file"); close(iso_fd); free(auto_key); return 1; }

        if(rc == 1)
        {
            /* d1 actually contains the disc key directly (dkey file) */
            memcpy(disc_key, d1, 16);
            printf("  \033[32m\xe2\x9c\x93\033[0m Disc key loaded from dkey file\n");
        }
        else
        {
            ps3_derive_disc_key(d1, disc_key);
            printf("  \033[32m\xe2\x9c\x93\033[0m d1 loaded, disc key derived\n");
        }
    }

    printf("  \033[32m\xe2\x9c\x93\033[0m Disc key: ");
    for(int i = 0; i < 12; i++) printf("%02X", disc_key[i]);
    printf("****\n");

    /* ---- Phase 3: Create output file ---- */
    phase(3, "Creating output file");

    int out_fd = open(out_path, O_CREAT | O_RDWR | O_EXCL, 0644);
    if(out_fd < 0)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Cannot create %s: %s", out_path, strerror(errno));
        phase_err(msg);
        close(iso_fd);
        ps3_ird_free(&ird);
        free(auto_key);
        return 1;
    }

    struct obmafs3_ioctl_set_ps3_image_arg pia;
    memset(&pia, 0, sizeof(pia));
    pia.disc_size    = region_map.total_sectors * PS3_SECTOR_SIZE;
    memcpy(pia.disc_key, disc_key, 16);
    pia.region_count = (uint16_t)region_map.count;
    for(uint32_t i = 0; i < region_map.count && i < OBMAFS3_PS3_MAX_REGIONS; i++)
    {
        pia.regions[i].start_sector = region_map.regions[i].start_sector;
        pia.regions[i].end_sector   = region_map.regions[i].end_sector;
    }

    if(ioctl(out_fd, OBMAFS3_IOC_SET_PS3_IMAGE, &pia) < 0)
    {
        phase_err("SET_PS3_IMAGE ioctl failed");
        close(out_fd);
        unlink(out_path);
        close(iso_fd);
        ps3_ird_free(&ird);
        free(auto_key);
        return 1;
    }

    phase_ok("File created and typed as PS3 disc image");

    /* ---- Phase 4: Import sector data ---- */
    phase(4, "Importing sector data");

    if(ps3_import(iso_fd, out_fd, disc_key, &region_map) != 0)
    {
        phase_err("Sector import failed");
        close(out_fd);
        close(iso_fd);
        ps3_ird_free(&ird);
        free(auto_key);
        return 1;
    }

    /* ---- Phase 5: Store media tags ---- */
    phase(5, "Storing media tags");
    {
        struct obmafs3_ioctl_tag_arg tag;

        /* Disc key */
        memset(&tag, 0, sizeof(tag));
        tag.tag_type = kPS3DiscKey;
        tag.data_length = 16;
        memcpy(tag.data, disc_key, 16);
        if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag) == 0) phase_ok("Disc key stored");
        else fprintf(stderr, "  \033[33m\xe2\x9a\xa0\033[0m Failed to store disc key (errno=%d)\n", errno);

        /* d1 */
        memset(&tag, 0, sizeof(tag));
        tag.tag_type = kPS3D1;
        tag.data_length = 16;
        memcpy(tag.data, d1, 16);
        if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag) == 0) phase_ok("d1 stored");
        else fprintf(stderr, "  \033[33m\xe2\x9a\xa0\033[0m Failed to store d1 (errno=%d)\n", errno);

        /* d2 (from IRD) */
        if(have_ird)
        {
            memset(&tag, 0, sizeof(tag));
            tag.tag_type = kPS3D2;
            tag.data_length = 16;
            memcpy(tag.data, ird.d2, 16);
            if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag) == 0) phase_ok("d2 stored");
            else fprintf(stderr, "  \033[33m\xe2\x9a\xa0\033[0m Failed to store d2 (errno=%d)\n", errno);
        }

        /* PIC (from IRD) */
        if(have_ird && ird.has_pic)
        {
            memset(&tag, 0, sizeof(tag));
            tag.tag_type = kPS3PIC;
            tag.data_length = 115;
            memcpy(tag.data, ird.pic, 115);
            if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag) == 0) phase_ok("PIC stored");
            else fprintf(stderr, "  \033[33m\xe2\x9a\xa0\033[0m Failed to store PIC (errno=%d)\n", errno);
        }

        /* Encryption map */
        {
            uint32_t map_size = 4 + region_map.count * 8;
            memset(&tag, 0, sizeof(tag));
            tag.tag_type    = kPS3EncryptionMap;
            tag.data_length = map_size;
            /* Serialize: count(4 LE) + regions(start(4 LE) + end(4 LE)) */
            uint32_t cnt = region_map.count;
            memcpy(tag.data, &cnt, 4);
            for(uint32_t i = 0; i < region_map.count; i++)
            {
                memcpy(tag.data + 4 + i * 8, &region_map.regions[i].start_sector, 4);
                memcpy(tag.data + 4 + i * 8 + 4, &region_map.regions[i].end_sector, 4);
            }
            if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag) == 0) phase_ok("Encryption map stored");
            else fprintf(stderr, "  \033[33m\xe2\x9a\xa0\033[0m Failed to store encryption map (errno=%d)\n", errno);
        }

        /* BD Disc Information sidecar (.di.bin) */
        {
            char *di_path = derive_sidecar_path(iso_path, "di.bin");
            if(di_path)
            {
                struct stat di_st;
                if(stat(di_path, &di_st) == 0 && di_st.st_size > 0 && di_st.st_size <= OBMAFS3_IOC_MAX_TAG_DATA)
                {
                    int di_fd = open(di_path, O_RDONLY);
                    if(di_fd >= 0)
                    {
                        memset(&tag, 0, sizeof(tag));
                        tag.tag_type    = kBdDI;
                        tag.data_length = (uint32_t)di_st.st_size;
                        if(read(di_fd, tag.data, tag.data_length) == (ssize_t)tag.data_length)
                        {
                            if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag) == 0)
                                phase_ok("BD Disc Information imported from .di.bin");
                            else
                                fprintf(stderr, "  \033[33m\xe2\x9a\xa0\033[0m Failed to store BD DI (errno=%d)\n", errno);
                        }
                        close(di_fd);
                    }
                }
                free(di_path);
            }
        }
    }

    /* ---- Phase 6: Parse PARAM.SFO and import metadata ---- */
    phase(6, "Importing metadata");

    struct ps3_sfo_data sfo;
    memset(&sfo, 0, sizeof(sfo));
    int have_sfo = 0;

    {
        uint8_t *sfo_data = NULL;
        size_t   sfo_size = 0;
        if(ps3_iso9660_read_file(iso_fd, "/PS3_GAME/PARAM.SFO", &sfo_data, &sfo_size) == 0)
        {
            if(ps3_parse_sfo(sfo_data, sfo_size, &sfo) == 0)
            {
                have_sfo = 1;
                const char *title = ps3_sfo_get(&sfo, "TITLE");
                if(title) printf("  \033[32m\xe2\x9c\x93\033[0m PARAM.SFO: \033[1m%s\033[0m\n", title);
            }
            free(sfo_data);
        }
        else
        {
            printf("  \033[33m\xe2\x9a\xa0\033[0m PARAM.SFO not found in ISO\n");
        }
    }

    ps3_import_metadata(out_fd, disc_id, have_sfo ? &sfo : NULL, have_ird ? &ird : NULL);
    phase_ok("Metadata stored");

    /* ---- Phase 7: Store IRD sidecar (only after successful import) ---- */
    if(have_ird)
    {
        phase(7, "Storing IRD sidecar");

        /* Build sidecar path: same as output but .ird extension */
        char *ird_out = derive_sidecar_path(out_path, "ird");
        if(ird_out)
        {
            /* Read the original IRD file and write it as a regular file */
            struct stat ist;
            if(stat(key_path, &ist) == 0)
            {
                int ird_in = open(key_path, O_RDONLY);
                if(ird_in >= 0)
                {
                    int ird_fd = open(ird_out, O_CREAT | O_WRONLY | O_EXCL, 0644);
                    if(ird_fd >= 0)
                    {
                        uint8_t buf[65536];
                        ssize_t n;
                        while((n = read(ird_in, buf, sizeof(buf))) > 0)
                            write(ird_fd, buf, (size_t)n);
                        close(ird_fd);
                        phase_ok("IRD sidecar written");
                    }
                    close(ird_in);
                }
            }
            free(ird_out);
        }
    }

    /* ---- Done ---- */
    close(out_fd);
    close(iso_fd);
    ps3_ird_free(&ird);
    free(auto_key);

    printf("\n\033[1;32m\xe2\x9c\x93 Import complete!\033[0m\n\n");
    return 0;
}
