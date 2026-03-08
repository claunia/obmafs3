// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : main.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-wiiu — Nintendo Wii U disc import tool
//
// --[ Description ] ----------------------------------------------------------
//
//     Entry point and phase orchestration for Wii U disc import.
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
    fprintf(stderr, "\033[1mimport-wiiu\033[0m — Nintendo Wii U disc importer for OBMAFS3\n\n");
    fprintf(stderr, "  \033[33mUsage:\033[0m %s <input.wud|wux> <output-path-on-obmafs>\n", prog);
    fprintf(stderr, "         %s <input.wud|wux> <disc-key-file> <output-path-on-obmafs>\n\n", prog);
    fprintf(stderr, "  Imports a Wii U WUD or WUX disc image into a mounted\n");
    fprintf(stderr, "  OBMAFS3 filesystem with deduplication.  Encrypted sectors\n");
    fprintf(stderr, "  are decrypted on import and re-encrypted on read for\n");
    fprintf(stderr, "  byte-identical reconstruction.\n\n");
    fprintf(stderr, "  If <disc-key-file> is omitted, a .key file next to the\n");
    fprintf(stderr, "  image is used automatically (e.g. game.key for game.wud).\n");
}

static void phase(int n, const char *desc)
{
    printf("\n\033[1;35m\xe2\x96\xb6 Phase %d:\033[0m \033[1m%s\033[0m\n", n, desc);
}

static void phase_ok(const char *msg) { printf("  \033[32m\xe2\x9c\x93\033[0m %s\n", msg); }

static void phase_err(const char *msg) { fprintf(stderr, "  \033[31m\xe2\x9c\x97\033[0m %s\n", msg); }

static int load_key(const char *path, uint8_t key[16])
{
    int fd = open(path, O_RDONLY);
    if(fd < 0)
    {
        fprintf(stderr, "Error: cannot open key file %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if(fstat(fd, &st) < 0 || st.st_size < 16)
    {
        fprintf(stderr, "Error: key file %s is too small (need 16 bytes)\n", path);
        close(fd);
        return -1;
    }

    if(read(fd, key, 16) != 16)
    {
        fprintf(stderr, "Error: cannot read 16 bytes from %s\n", path);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/**
 * Build a .key path from the image path by replacing the extension.
 * Returns a malloc'd string, or NULL on failure.
 */
static char *derive_key_path(const char *image_path)
{
    size_t len  = strlen(image_path);
    char  *path = malloc(len + 5); /* worst case: append ".key" */
    if(!path) return NULL;

    memcpy(path, image_path, len + 1);
    char *dot = strrchr(path, '.');
    char *sep = strrchr(path, '/');
    /* Only replace extension if dot is after the last directory separator */
    if(dot && (!sep || dot > sep))
        strcpy(dot, ".key");
    else
        strcat(path, ".key");

    return path;
}

int main(int argc, char **argv)
{
    if(argc < 3)
    {
        usage(argv[0]);
        return 1;
    }

    const char *image_path   = argv[1];
    const char *disckey_path = NULL;
    const char *out_path     = NULL;
    char       *auto_key     = NULL;

    if(argc >= 4)
    {
        /* Explicit key path given */
        disckey_path = argv[2];
        out_path     = argv[3];
    }
    else
    {
        /* argc == 3: try to find a .key sidecar automatically */
        out_path = argv[2];
        auto_key = derive_key_path(image_path);
        if(auto_key)
        {
            struct stat st;
            if(stat(auto_key, &st) == 0 && st.st_size >= 16)
                disckey_path = auto_key;
        }
        if(!disckey_path)
        {
            fprintf(stderr, "Error: no disc key file specified and no .key sidecar found\n");
            fprintf(stderr, "       Looked for: %s\n", auto_key ? auto_key : "(allocation failed)");
            free(auto_key);
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
    printf("\033[1;36m\xe2\x95\x91\033[0m  \033[1mimport-wiiu\033[0m"
           " \xe2\x80\x94 Wii U Disc Importer for OBMAFS3       \033[1;36m\xe2\x95\x91\033[0m\n");
    printf("\033[1;36m\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
           "\xe2\x95\x90\xe2\x95\x9d\033[0m\n");

    /* ---- Phase 1: Load keys and open disc image ---- */
    phase(1, "Opening source image and loading keys");

    uint8_t disc_key[16];
    if(load_key(disckey_path, disc_key) != 0) return 1;

    printf("  \033[32m\xe2\x9c\x93\033[0m Disc key loaded: ");
    for(int i = 0; i < 12; i++) printf("%02X", disc_key[i]);
    printf("********\n");

    struct wiiu_reader reader;
    if(wiiu_reader_open(image_path, &reader) != 0) return 1;

    /* Read disc header (sector 0, plaintext) */
    uint8_t header[WIIU_SECTOR_SIZE];
    if(wiiu_reader_pread(&reader, header, WIIU_SECTOR_SIZE, 0) < (ssize_t)WIIU_SECTOR_SIZE)
    {
        phase_err("Cannot read disc header");
        wiiu_reader_close(&reader);
        return 1;
    }

    /* Verify WUP magic ("WUP-") */
    if(memcmp(header, "WUP-", 4) != 0)
    {
        phase_err("Not a valid Wii U disc image (missing WUP- header)");
        wiiu_reader_close(&reader);
        return 1;
    }

    wiiu_print_disc_info(header, reader.disc_size);

    /* ---- Phase 2: Parse partition table ---- */
    phase(2, "Reading partition table");

    struct wiiu_partition parts[WIIU_MAX_PARTITIONS];
    int                  part_count = 0;
    if(wiiu_parse_toc(&reader, disc_key, parts, &part_count) != 0)
    {
        wiiu_reader_close(&reader);
        return 1;
    }

    phase_ok("Found partitions:");
    for(int i = 0; i < part_count; i++)
    {
        const char *ptype;
        if(strncmp(parts[i].identifier, "SI", 2) == 0)      ptype = "\033[34msystem info\033[0m";
        else if(strncmp(parts[i].identifier, "UP", 2) == 0)  ptype = "\033[33mupdate\033[0m";
        else if(strncmp(parts[i].identifier, "GM", 2) == 0)  ptype = "\033[32mgame\033[0m";
        else if(strncmp(parts[i].identifier, "GI", 2) == 0)  ptype = "\033[34mgame info\033[0m";
        else                                                  ptype = "\033[90munknown\033[0m";

        printf("    \033[90m%s\033[0m Partition %d: %s  \033[90m%s  sector=0x%X\033[0m\n",
               i == part_count - 1 ? "\xe2\x94\x94\xe2\x94\x80" : "\xe2\x94\x9c\xe2\x94\x80",
               i, ptype, parts[i].identifier, parts[i].start_sector);
    }

    /* ---- Phase 3: Extract title keys for GM partitions ---- */
    phase(3, "Extracting title keys from SI/GI partitions");
    int keys_found = wiiu_extract_title_keys(&reader, disc_key, parts, part_count);
    if(keys_found > 0)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "%d title key(s) extracted", keys_found);
        phase_ok(msg);
    }
    else
    {
        phase_ok("No title keys found (GM partitions will use disc key)");
    }

    /* ---- Phase 4: Create output file ---- */
    phase(4, "Creating output file");

    int out_fd = open(out_path, O_CREAT | O_RDWR | O_EXCL, 0644);
    if(out_fd < 0)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Cannot create %s: %s", out_path, strerror(errno));
        phase_err(msg);
        wiiu_reader_close(&reader);
        return 1;
    }

    /* Set as Nintendo image (disc_type = 2 for Wii U) */
    struct obmafs3_ioctl_set_nintendo_image_arg nia;
    memset(&nia, 0, sizeof(nia));
    nia.disc_type       = 2; /* Wii U */
    nia.disc_size       = reader.disc_size;
    nia.partition_count = (uint16_t)part_count;
    for(int i = 0; i < part_count && i < OBMAFS3_NGC_MAX_PARTITIONS; i++)
    {
        nia.partitions[i].data_offset = (uint64_t)parts[i].start_sector * WIIU_SECTOR_SIZE;
        /* Compute partition size: extends to next partition or disc end */
        uint64_t next_off = reader.disc_size;
        for(int j = 0; j < part_count; j++)
        {
            uint64_t joff = (uint64_t)parts[j].start_sector * WIIU_SECTOR_SIZE;
            if(joff > nia.partitions[i].data_offset && joff < next_off) next_off = joff;
        }
        nia.partitions[i].data_size = next_off - nia.partitions[i].data_offset;
        memcpy(nia.partitions[i].title_key, parts[i].key, 16);
    }

    if(ioctl(out_fd, OBMAFS3_IOC_SET_NINTENDO_IMAGE, &nia) < 0)
    {
        phase_err("SET_NINTENDO_IMAGE ioctl failed");
        close(out_fd);
        unlink(out_path);
        wiiu_reader_close(&reader);
        return 1;
    }

    phase_ok("File created and typed as Wii U disc image");

    /* ---- Phase 5: Import sector data ---- */
    phase(5, "Importing sector data");

    if(wiiu_import(&reader, out_fd, parts, part_count) != 0)
    {
        phase_err("Sector import failed");
        close(out_fd);
        wiiu_reader_close(&reader);
        return 1;
    }

    /* ---- Phase 6: Store disc key as media tag ---- */
    phase(6, "Storing disc key");
    {
        struct obmafs3_ioctl_tag_arg tag_arg;
        memset(&tag_arg, 0, sizeof(tag_arg));
        tag_arg.tag_type    = kNintendoWiiUDiscKey;
        tag_arg.data_length = 16;
        memcpy(tag_arg.data, disc_key, 16);
        if(ioctl(out_fd, OBMAFS3_IOC_SET_MEDIA_TAG, &tag_arg) < 0)
            fprintf(stderr, "  \033[33m\xe2\x9a\xa0\033[0m Failed to store disc key media tag\n");
        else
            phase_ok("Disc key stored as media tag");
    }

    /* ---- Phase 7: Import metadata ---- */
    phase(7, "Importing metadata");
    wiiu_import_metadata(out_fd, header);

    /* ---- Done ---- */
    close(out_fd);
    wiiu_reader_close(&reader);
    free(auto_key);

    printf("\n\033[1;32m\xe2\x9c\x93 Import complete!\033[0m\n\n");
    return 0;
}
