// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : sidecar.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : import-aif
//
// --[ Description ] ----------------------------------------------------------
//
//     Sidecar file exports (CICM XML, Aaru JSON, dump hardware JSON).
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

/**
 * Write a binary blob as a sidecar file.
 *
 * @param data      Data buffer to write.
 * @param data_len  Length of data.
 * @param path      Output file path.
 * @param label     Human-readable label for log messages.
 */
static void write_sidecar(const uint8_t *data, size_t data_len,
                           const char *path, const char *label)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if(fd >= 0)
    {
        ssize_t written = write(fd, data, data_len);
        if(written < 0 || (size_t)written != data_len)
            fprintf(stderr, "Warning: incomplete write of %s\n", label);
        else
            fprintf(stderr, "%s saved to %s\n", label, path);
        close(fd);
    }
    else
    {
        fprintf(stderr, "Warning: failed to create '%s' (errno=%d: %s)\n",
                path, errno, strerror(errno));
    }
}

/**
 * Export CICM metadata XML sidecar if available.
 */
static void export_cicm_metadata(void *aaruf_ctx, const char *output_path, size_t base_len)
{
    size_t cicm_len = 0;
    if(aaruf_get_cicm_metadata(aaruf_ctx, NULL, &cicm_len) != AARUF_ERROR_BUFFER_TOO_SMALL || cicm_len == 0)
        return;

    uint8_t *cicm_buf = malloc(cicm_len);
    if(!cicm_buf) return;

    if(aaruf_get_cicm_metadata(aaruf_ctx, cicm_buf, &cicm_len) == AARUF_STATUS_OK)
    {
        char *xml_path = malloc(base_len + sizeof(".metadata.xml"));
        if(xml_path)
        {
            memcpy(xml_path, output_path, base_len);
            memcpy(xml_path + base_len, ".metadata.xml", sizeof(".metadata.xml"));
            write_sidecar(cicm_buf, cicm_len, xml_path, "CICM metadata");
            free(xml_path);
        }
    }
    free(cicm_buf);
}

/**
 * Export Aaru JSON metadata sidecar if available.
 */
static void export_aaru_json_metadata(void *aaruf_ctx, const char *output_path, size_t base_len)
{
    size_t json_len = 0;
    if(aaruf_get_aaru_json_metadata(aaruf_ctx, NULL, &json_len) != AARUF_ERROR_BUFFER_TOO_SMALL || json_len == 0)
        return;

    uint8_t *json_buf = malloc(json_len);
    if(!json_buf) return;

    if(aaruf_get_aaru_json_metadata(aaruf_ctx, json_buf, &json_len) == AARUF_STATUS_OK)
    {
        char *json_path = malloc(base_len + sizeof(".metadata.json"));
        if(json_path)
        {
            memcpy(json_path, output_path, base_len);
            memcpy(json_path + base_len, ".metadata.json", sizeof(".metadata.json"));
            write_sidecar(json_buf, json_len, json_path, "Aaru JSON metadata");
            free(json_path);
        }
    }
    free(json_buf);
}

/**
 * Export dump hardware as JSON sidecar if available.
 *
 * Parses the raw binary block returned by aaruf_get_dumphw (no internal
 * struct casting) and serializes it as JSON.
 */
static void export_dumphw_json(void *aaruf_ctx, const char *output_path, size_t base_len)
{
    size_t dumphw_size = 0;
    if(aaruf_get_dumphw(aaruf_ctx, NULL, &dumphw_size) != AARUF_ERROR_BUFFER_TOO_SMALL || dumphw_size == 0)
        return;

    uint8_t *dumphw_buf = malloc(dumphw_size);
    if(!dumphw_buf) return;

    if(aaruf_get_dumphw(aaruf_ctx, dumphw_buf, &dumphw_size) != AARUF_STATUS_OK || dumphw_size < 18)
    {
        free(dumphw_buf);
        return;
    }

    /*
     * Parse the raw binary block.
     * Layout (all little-endian, packed):
     *   DumpHardwareHeader (18 bytes):
     *     uint32 identifier, uint16 entries, uint32 length, uint64 crc64
     *   Per entry:
     *     DumpHardwareEntry (36 bytes): 9 × uint32 length fields
     *     Variable-length UTF-8 strings (manufacturer, model, revision,
     *       firmware, serial, softwareName, softwareVersion, softwareOS)
     *     Extents array (16 bytes each: uint64 start + uint64 end)
     */
    uint16_t entries;
    memcpy(&entries, dumphw_buf + 4, 2);

    size_t off = 18; /* skip header */

    /* Build JSON in a dynamic buffer */
    size_t cap = 4096;
    size_t pos = 0;
    char  *jbuf = malloc(cap);

    if(!jbuf || entries == 0)
    {
        free(jbuf);
        free(dumphw_buf);
        return;
    }

    #define JAPPEND(...)                                                     \
        do {                                                                 \
            int _n = snprintf(jbuf + pos, cap - pos, __VA_ARGS__);           \
            if(_n < 0) break;                                                \
            while(pos + (size_t)_n >= cap) {                                 \
                cap *= 2;                                                    \
                char *_tmp = realloc(jbuf, cap);                             \
                if(!_tmp) { free(jbuf); jbuf = NULL; break; }                \
                jbuf = _tmp;                                                 \
                _n = snprintf(jbuf + pos, cap - pos, __VA_ARGS__);           \
            }                                                                \
            if(!jbuf) break;                                                 \
            pos += (size_t)_n;                                               \
        } while(0)

    JAPPEND("[\n");
    for(uint16_t i = 0; i < entries && jbuf; i++)
    {
        if(off + 36 > dumphw_size) break;

        /* DumpHardwareEntry: 9 × uint32_t LE (36 bytes) */
        uint32_t lens[9];
        memcpy(lens, dumphw_buf + off, 36);
        off += 36;
        /* lens[0]=manufacturer, [1]=model, [2]=revision, [3]=firmware,
           [4]=serial, [5]=softwareName, [6]=softwareVersion,
           [7]=softwareOperatingSystem, [8]=extents (count) */

        static const char *const field_names[] = {
            "manufacturer", "model", "revision", "firmware",
            "serial", "software_name", "software_version", "software_os"
        };

        /* Compute pointers to each string in the buffer */
        const uint8_t *strs[8];
        for(int f = 0; f < 8; f++)
        {
            strs[f] = (off + lens[f] <= dumphw_size) ? dumphw_buf + off : NULL;
            off += lens[f];
        }

        JAPPEND("  {\n");

        for(int f = 0; f < 8 && jbuf; f++)
        {
            if(strs[f] && lens[f] > 0)
                JAPPEND("    \"%s\": \"%.*s\",\n", field_names[f],
                        (int)lens[f], strs[f]);
        }

        uint32_t n_extents = lens[8];
        if(n_extents > 0 && off + n_extents * 16 <= dumphw_size)
        {
            JAPPEND("    \"extents\": [\n");
            for(uint32_t x = 0; x < n_extents && jbuf; x++)
            {
                uint64_t ext_start, ext_end;
                memcpy(&ext_start, dumphw_buf + off, 8);
                memcpy(&ext_end, dumphw_buf + off + 8, 8);
                off += 16;
                JAPPEND("      {\"start\": %" PRIu64 ", \"end\": %" PRIu64 "}%s\n",
                        ext_start, ext_end,
                        (x + 1 < n_extents) ? "," : "");
            }
            JAPPEND("    ]\n");
        }
        else
        {
            off += (size_t)n_extents * 16;
            /* Remove trailing comma from last string field */
            if(jbuf && pos >= 2 && jbuf[pos - 2] == ',')
            {
                jbuf[pos - 2] = '\n';
                pos--;
            }
        }

        JAPPEND("  }%s\n", (i + 1 < entries) ? "," : "");
    }
    if(jbuf) JAPPEND("]\n");

    #undef JAPPEND

    if(jbuf)
    {
        char *hw_path = malloc(base_len + sizeof(".dumphw.json"));
        if(hw_path)
        {
            memcpy(hw_path, output_path, base_len);
            memcpy(hw_path + base_len, ".dumphw.json", sizeof(".dumphw.json"));
            write_sidecar((const uint8_t *)jbuf, pos, hw_path, "Dump hardware");
            free(hw_path);
        }
        free(jbuf);
    }

    free(dumphw_buf);
}

/**
 * Export all sidecar files (CICM XML, Aaru JSON metadata, dump hardware JSON).
 *
 * @param aaruf_ctx   libaaruformat context.
 * @param output_path Path to the imported image file.
 * @param base_len    Length of output_path without extension.
 */
void export_sidecar_files(void *aaruf_ctx, const char *output_path, size_t base_len)
{
    export_cicm_metadata(aaruf_ctx, output_path, base_len);
    export_aaru_json_metadata(aaruf_ctx, output_path, base_len);
    export_dumphw_json(aaruf_ctx, output_path, base_len);
}
