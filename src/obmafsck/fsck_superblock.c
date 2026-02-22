// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : fsck_superblock.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : OBMAFS3 filesystem check utility (obmafsck)
//
// --[ Description ] ----------------------------------------------------------
//
//     Superblock field range checks for obmafsck.
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

#include "fsck.h"

/**
 * Validate superblock field ranges and internal consistency.
 *
 * Checks block_size, dedup_block_size, total_bytes, checksum_type,
 * next_inode_id, bitmap parameters, LBA fields (in-range, unique),
 * volume_label NUL-termination, and creation_time plausibility.
 *
 * When a fixable mismatch is detected the user is prompted (unless
 * auto_yes / auto_no is set).  Fixes are written back via the
 * superblock write path.
 *
 * @param sb          Pointer to the in-memory superblock (modified on fix).
 * @param fd          File descriptor to write fixes.
 * @param file_size   Actual size of the backing file/device (from fstat).
 * @param auto_yes    If non-zero, always repair.
 * @param auto_no     If non-zero, never repair.
 * @param errors      In/out: incremented for each unfixed error.
 */
void validate_superblock_fields(struct obmafs3_sb *sb, int fd, uint64_t file_size,
                                int auto_yes, int auto_no, int *errors)
{
    int bad = 0, fixed = 0;
    uint64_t total_blocks = sb->total_bytes / sb->block_size;

    /* ---- block_size ---- */
    if(!is_power_of_two(sb->block_size))
    {
        printf("    block_size %" PRIu64 " is not a power of 2\n", sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set block_size to 4096?"))
        {
            sb->block_size = 4096;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }
    else if(sb->block_size < 4096)
    {
        printf("    block_size %" PRIu64 " is below minimum (4096)\n", sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set block_size to 4096?"))
        {
            sb->block_size = 4096;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }

    /* ---- dedup_block_size ---- */
    if(!is_power_of_two(sb->dedup_block_size))
    {
        printf("    dedup_block_size %" PRIu64 " is not a power of 2\n", sb->dedup_block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set dedup_block_size to 4194304?"))
        {
            sb->dedup_block_size = 4194304;
            fixed++;
        }
    }
    else if(sb->dedup_block_size < sb->block_size)
    {
        printf("    dedup_block_size %" PRIu64 " is smaller than block_size %" PRIu64 "\n",
               sb->dedup_block_size, sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set dedup_block_size to 4194304?"))
        {
            sb->dedup_block_size = 4194304;
            fixed++;
        }
    }
    else if(sb->dedup_block_size % sb->block_size != 0)
    {
        printf("    dedup_block_size %" PRIu64 " is not a multiple of block_size %" PRIu64 "\n",
               sb->dedup_block_size, sb->block_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set dedup_block_size to 4194304?"))
        {
            sb->dedup_block_size = 4194304;
            fixed++;
        }
    }

    /* ---- total_bytes ---- */
    if(sb->total_bytes % sb->block_size != 0)
    {
        printf("    total_bytes %" PRIu64 " is not a multiple of block_size %" PRIu64 "\n",
               sb->total_bytes, sb->block_size);
        bad++;
        uint64_t aligned = (sb->total_bytes / sb->block_size) * sb->block_size;
        printf("    (nearest aligned value: %" PRIu64 ")\n", aligned);
        if(ask_fix(auto_yes, auto_no, "    Round total_bytes down to block boundary?"))
        {
            sb->total_bytes = aligned;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }

    if(file_size > 0 && sb->total_bytes != file_size)
    {
        printf("    total_bytes %" PRIu64 " does not match actual file size %" PRIu64 "\n",
               sb->total_bytes, file_size);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set total_bytes to match file size?"))
        {
            sb->total_bytes = file_size;
            total_blocks = sb->total_bytes / sb->block_size;
            fixed++;
        }
    }

    /* ---- checksum_type ---- */
    if(sb->checksum_type != kChecksumTypeXXH64)
    {
        printf("    checksum_type %" PRIu16 " is not supported (expected %d)\n",
               sb->checksum_type, kChecksumTypeXXH64);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set checksum_type to XXH64 (0)?"))
        {
            sb->checksum_type = kChecksumTypeXXH64;
            fixed++;
        }
    }

    /* ---- next_inode_id ---- */
    if(sb->next_inode_id < 3)
    {
        printf("    next_inode_id %" PRIu64 " is below minimum (3)\n", sb->next_inode_id);
        bad++;
        if(ask_fix(auto_yes, auto_no, "    Set next_inode_id to 3?"))
        {
            sb->next_inode_id = 3;
            fixed++;
        }
    }

    /* ---- bitmap_lba ---- */
    if(sb->bitmap_lba == 0)
    {
        printf("    bitmap_lba is 0 (no allocation bitmap)\n");
        bad++;
    }
    else if(sb->bitmap_lba >= total_blocks)
    {
        printf("    bitmap_lba %" PRIu64 " is beyond total blocks %" PRIu64 "\n",
               sb->bitmap_lba, total_blocks);
        bad++;
    }

    /* ---- bitmap_blocks ---- */
    if(sb->bitmap_lba > 0 && sb->bitmap_blocks > 0)
    {
        uint64_t bitmap_bytes         = (total_blocks + 7) / 8;
        size_t   hdr_size             = sizeof(struct bitmap_header);
        uint64_t first_block_capacity = sb->block_size - hdr_size;
        uint64_t expected_bitmap_blks;
        if(bitmap_bytes <= first_block_capacity)
            expected_bitmap_blks = 1;
        else
            expected_bitmap_blks = 1 + (bitmap_bytes - first_block_capacity + sb->block_size - 1) / sb->block_size;

        if(sb->bitmap_blocks != expected_bitmap_blks)
        {
            printf("    bitmap_blocks %" PRIu64 " does not match expected %" PRIu64 "\n",
                   sb->bitmap_blocks, expected_bitmap_blks);
            bad++;
            if(ask_fix(auto_yes, auto_no, "    Fix bitmap_blocks?"))
            {
                sb->bitmap_blocks = expected_bitmap_blks;
                fixed++;
            }
        }

        if(sb->bitmap_lba + sb->bitmap_blocks > total_blocks)
        {
            printf("    bitmap extends beyond filesystem (LBA %" PRIu64 " + %" PRIu64 " blocks > %" PRIu64 ")\n",
                   sb->bitmap_lba, sb->bitmap_blocks, total_blocks);
            bad++;
        }
    }

    /* ---- keyset_lba / keyset_blocks ---- */
    if(sb->keyset_lba != 0)
    {
        if(sb->keyset_lba >= total_blocks)
        {
            printf("    keyset_lba %" PRIu64 " is beyond total blocks %" PRIu64 "\n",
                   sb->keyset_lba, total_blocks);
            bad++;
        }
        if(sb->keyset_blocks == 0)
        {
            printf("    keyset_lba is set but keyset_blocks is 0\n");
            bad++;
        }
        else if(sb->keyset_lba + sb->keyset_blocks > total_blocks)
        {
            printf("    keyset extends beyond filesystem (LBA %" PRIu64 " + %" PRIu64 " blocks > %" PRIu64 ")\n",
                   sb->keyset_lba, sb->keyset_blocks, total_blocks);
            bad++;
        }
    }

    /* ---- pending_lba / pending_blocks ---- */
    if(sb->pending_lba != 0)
    {
        if(sb->pending_lba >= total_blocks)
        {
            printf("    pending_lba %" PRIu64 " is beyond total blocks %" PRIu64 "\n",
                   sb->pending_lba, total_blocks);
            bad++;
        }
        if(sb->pending_blocks == 0)
        {
            printf("    pending_lba is set but pending_blocks is 0\n");
            bad++;
        }
        else if(sb->pending_lba + sb->pending_blocks > total_blocks)
        {
            printf("    pending extends beyond filesystem (LBA %" PRIu64 " + %" PRIu64 " blocks > %" PRIu64 ")\n",
                   sb->pending_lba, sb->pending_blocks, total_blocks);
            bad++;
        }
    }

    /* ---- LBA range checks ---- */
    struct { const char *name; uint64_t lba; } lba_fields[] = {
        { "catalog_lba",       sb->catalog_lba       },
        { "inode_lba",         sb->inode_lba         },
        { "overflow_lba",      sb->overflow_lba      },
        { "dedup_lba",         sb->dedup_lba         },
        { "metadata_lba",      sb->metadata_lba      },
        { "media_tag_lba",     sb->media_tag_lba     },
        { "cd_prefix_lba",     sb->cd_prefix_lba     },
        { "cd_suffix_lba",     sb->cd_suffix_lba     },
        { "cd_subchannel_lba", sb->cd_subchannel_lba },
        { "metadata_idx_lba",  sb->metadata_idx_lba  },
        { "refcount_lba",      sb->refcount_lba      },
    };
    int lba_count = (int)(sizeof(lba_fields) / sizeof(lba_fields[0]));

    for(int i = 0; i < lba_count; i++)
    {
        if(lba_fields[i].lba == 0) continue; /* optional field */
        if(lba_fields[i].lba >= total_blocks)
        {
            printf("    %s %" PRIu64 " is beyond total blocks %" PRIu64 "\n",
                   lba_fields[i].name, lba_fields[i].lba, total_blocks);
            bad++;
        }
    }

    /* ---- LBA uniqueness ---- */
    for(int i = 0; i < lba_count; i++)
    {
        if(lba_fields[i].lba == 0) continue;
        for(int j = i + 1; j < lba_count; j++)
        {
            if(lba_fields[j].lba == 0) continue;
            if(lba_fields[i].lba == lba_fields[j].lba)
            {
                printf("    %s and %s share the same LBA %" PRIu64 "\n",
                       lba_fields[i].name, lba_fields[j].name, lba_fields[i].lba);
                bad++;
            }
        }
    }

    /* ---- volume_label NUL-termination ---- */
    {
        int has_nul = 0;
        for(size_t i = 0; i < sizeof(sb->volume_label); i++)
        {
            if(sb->volume_label[i] == '\0')
            {
                has_nul = 1;
                break;
            }
        }
        if(!has_nul)
        {
            printf("    volume_label is not NUL-terminated\n");
            bad++;
            if(ask_fix(auto_yes, auto_no, "    NUL-terminate volume_label?"))
            {
                sb->volume_label[sizeof(sb->volume_label) - 1] = '\0';
                fixed++;
            }
        }
    }

    /* ---- btree_clump_size / dedup_clump_size ---- */
    if(sb->btree_clump_size == 0 || sb->dedup_clump_size == 0)
    {
        if(sb->btree_clump_size == 0)
            printf("    btree_clump_size is 0 (no clumping configured)\n");
        if(sb->dedup_clump_size == 0)
            printf("    dedup_clump_size is 0 (no clumping configured)\n");
        bad++;
        char clump_prompt[128];
        snprintf(clump_prompt, sizeof(clump_prompt),
                 "    Set clump sizes to defaults (btree=%d, dedup=%d)?",
                 OBMAFS3_DEFAULT_CLUMP_SIZE, OBMAFS3_DEDUP_CLUMP_SIZE);
        if(ask_fix(auto_yes, auto_no, clump_prompt))
        {
            if(sb->btree_clump_size == 0)
                sb->btree_clump_size = OBMAFS3_DEFAULT_CLUMP_SIZE;
            if(sb->dedup_clump_size == 0)
                sb->dedup_clump_size = OBMAFS3_DEDUP_CLUMP_SIZE;
            fixed++;
        }
    }

    /* ---- creation_time ---- */
    if(sb->creation_time == 0)
    {
        printf("    creation_time is 0 (not set)\n");
        bad++;
    }
    else
    {
        uint64_t now = (uint64_t)time(NULL);
        if(sb->creation_time > now)
        {
            printf("    creation_time %" PRIu64 " is in the future (now %" PRIu64 ")\n",
                   sb->creation_time, now);
            bad++;
        }
    }

    /* ---- Write fixes if any ---- */
    if(fixed > 0)
    {
        /* Recompute superblock checksum before writing */
        memset(sb->checksum, 0, sizeof(sb->checksum));
        obmafs3_checksum_block(sb, sizeof(*sb), sb->checksum);

        ssize_t n = pwrite(fd, sb, sizeof(*sb), 0);
        if(n < 0 || (size_t)n != sizeof(*sb))
            fprintf(stderr, "    Error: could not write superblock fix\n");
        else
        {
            printf("    Superblock updated (%d field(s) fixed).\n", fixed);
            /* Also update the backup superblock */
            if(sb->total_bytes > 0 && sb->block_size > 0)
            {
                uint64_t blba = OBMAFS3_BACKUP_SB_LBA(sb->total_bytes, sb->block_size);
                if(blba > 0)
                    pwrite(fd, sb, sizeof(*sb), (off_t)(blba * sb->block_size));
            }
        }
    }

    /* ---- Summary ---- */
    if(bad == 0)
    {
        result_ok("Field checks:", "");
    }
    else
    {
        if(fixed > 0)
            result_fixed("Field checks:", "%d error(s), %d fixed", bad, fixed);
        else
            result_bad("Field checks:", "%d error(s)", bad);
        *errors += (bad - fixed);
    }
}
