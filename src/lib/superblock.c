// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : superblock.c
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     OBMAFS3 Superblock read/write/validation.
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

#include "debug.h"
#include "obmafs.h"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <xxhash.h>

/**
 * Read the superblock from disk.
 *
 * Reads @c sizeof(struct obmafs3_sb) bytes from offset 0 of the file
 * descriptor.
 *
 * @param fd  Open file descriptor for the filesystem image.
 * @param sb  Output superblock structure.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on failure.
 */
int obmafs3_sb_read(int fd, struct obmafs3_sb *sb)
{
    ssize_t n = pread(fd, sb, sizeof(*sb), 0);
    if(n < 0 || (size_t)n != sizeof(*sb))
        DBG_RETURN_ERRNO(OBMAFS3_ERR_IO, "sb pread expected=%zu got=%zd", sizeof(*sb), n);
    return OBMAFS3_OK;
}

/**
 * Write the superblock to disk.
 *
 * Writes @c sizeof(struct obmafs3_sb) bytes to offset 0 of the file
 * descriptor.
 *
 * @param fd  Open file descriptor for the filesystem image.
 * @param sb  Pointer to the superblock structure to write.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on failure.
 */
int obmafs3_sb_write(int fd, const struct obmafs3_sb *sb)
{
    /* Compute V1 checksum: zero field, hash first OBMAFS3_SB_V1_SIZE bytes */
    struct obmafs3_sb tmp = *sb;
    memset(tmp.checksum, 0, sizeof(tmp.checksum));
    obmafs3_checksum_block(&tmp, OBMAFS3_SB_V1_SIZE, tmp.checksum);

    /* Compute extension checksum: zero field, hash bytes 526..4095 */
    memset(tmp.checksum2, 0, sizeof(tmp.checksum2));
    obmafs3_checksum_block((const uint8_t *)&tmp + OBMAFS3_SB_V1_SIZE,
                           sizeof(tmp) - OBMAFS3_SB_V1_SIZE, tmp.checksum2);

    /* Write the primary superblock at LBA 0 */
    ssize_t n = pwrite(fd, &tmp, sizeof(tmp), 0);
    if(n < 0 || (size_t)n != sizeof(tmp))
        DBG_RETURN_ERRNO(OBMAFS3_ERR_IO, "sb pwrite expected=%zu got=%zd", sizeof(tmp), n);

    /* Write the backup superblock at the last block */
    if(tmp.total_bytes > 0 && tmp.block_size > 0)
    {
        uint64_t backup_lba = OBMAFS3_BACKUP_SB_LBA(tmp.total_bytes, tmp.block_size);
        if(backup_lba > 0)
        {
            off_t   backup_off = (off_t)(backup_lba * tmp.block_size);
            ssize_t nb         = pwrite(fd, &tmp, sizeof(tmp), backup_off);
            if(nb < 0 || (size_t)nb != sizeof(tmp))
                DBG_RETURN_ERRNO(OBMAFS3_ERR_IO, "backup sb pwrite lba=%" PRIu64 " expected=%zu got=%zd", backup_lba,
                                 sizeof(tmp), nb);
        }
    }

    return OBMAFS3_OK;
}

/**
 * Read a superblock from disk (lenient).
 *
 * Reads the superblock and validates magic.  The checksum is verified
 * but a mismatch is reported via @p checksum_ok rather than causing
 * an error return.
 *
 * @param fd           Open file descriptor for the filesystem image.
 * @param sb           Output superblock structure.
 * @param checksum_ok  Set to 1 if the checksum matches, 0 otherwise.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on failure.
 */
int obmafs3_sb_read_lenient(int fd, struct obmafs3_sb *sb, int *checksum_ok)
{
    int rc = obmafs3_sb_read(fd, sb);
    if(rc != OBMAFS3_OK) return rc;

    /* Verify V1 checksum: covers bytes 0..OBMAFS3_SB_V1_SIZE-1 */
    uint8_t stored[32];
    memcpy(stored, sb->checksum, 32);
    memset(sb->checksum, 0, 32);
    uint8_t computed[32];
    obmafs3_checksum_block(sb, OBMAFS3_SB_V1_SIZE, computed);
    memcpy(sb->checksum, stored, 32);
    *checksum_ok = (memcmp(stored, computed, 32) == 0);

    /* Verify extension checksum (checksum2): covers bytes 526..4095 */
    if(*checksum_ok)
    {
        uint8_t stored2[32];
        memcpy(stored2, sb->checksum2, 32);
        memset(sb->checksum2, 0, 32);
        uint8_t computed2[32];
        obmafs3_checksum_block((const uint8_t *)sb + OBMAFS3_SB_V1_SIZE,
                               sizeof(*sb) - OBMAFS3_SB_V1_SIZE, computed2);
        memcpy(sb->checksum2, stored2, 32);
        /* Extension checksum is only meaningful when non-zero (old FS have all-zero extension) */
        uint8_t zero[32];
        memset(zero, 0, 32);
        if(memcmp(stored2, zero, 32) != 0)
            *checksum_ok = (memcmp(stored2, computed2, 32) == 0);
    }

    return OBMAFS3_OK;
}

/**
 * Validate a superblock's integrity.
 *
 * Checks the magic number, block sizes, total size, and required LBAs.
 *
 * @param sb  Pointer to the superblock to validate.
 * @return @c OBMAFS3_OK if valid, or an appropriate error code.
 */
int obmafs3_sb_validate(const struct obmafs3_sb *sb)
{
    if(sb->magic != OBMAFS3_SB_MAGIC)
        DBG_RETURN(OBMAFS3_ERR_BADMAGIC, "bad magic 0x%" PRIx64 " expected 0x%" PRIx64, sb->magic,
                   (uint64_t)OBMAFS3_SB_MAGIC);
    if(sb->block_size == 0 || sb->dedup_block_size == 0)
        DBG_RETURN(OBMAFS3_ERR_INVAL, "bad block_size=%" PRIu64 " dedup_block_size=%" PRIu64, sb->block_size,
                   sb->dedup_block_size);
    if(sb->total_bytes == 0) DBG_RETURN(OBMAFS3_ERR_INVAL, "total_bytes=0");
    if(sb->catalog_lba == 0 || sb->inode_lba == 0)
        DBG_RETURN(OBMAFS3_ERR_INVAL, "catalog_lba=%" PRIu64 " inode_lba=%" PRIu64, sb->catalog_lba, sb->inode_lba);
    return OBMAFS3_OK;
}

/**
 * Read the backup superblock from disk.
 *
 * The backup superblock is stored at the last block of the filesystem.
 * The caller must supply @p block_size and @p total_bytes so the
 * backup LBA can be computed.
 *
 * @param fd          Open file descriptor for the filesystem image.
 * @param block_size  Block size in bytes.
 * @param total_bytes Total filesystem size in bytes.
 * @param sb          Output superblock structure.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on failure.
 */
int obmafs3_sb_read_backup(int fd, uint64_t block_size, uint64_t total_bytes, struct obmafs3_sb *sb)
{
    if(block_size == 0 || total_bytes == 0) DBG_RETURN(OBMAFS3_ERR_INVAL, "block_size or total_bytes is 0");

    uint64_t backup_lba = OBMAFS3_BACKUP_SB_LBA(total_bytes, block_size);
    if(backup_lba == 0) DBG_RETURN(OBMAFS3_ERR_INVAL, "backup lba would be 0");

    off_t   offset = (off_t)(backup_lba * block_size);
    ssize_t n      = pread(fd, sb, sizeof(*sb), offset);
    if(n < 0 || (size_t)n != sizeof(*sb))
        DBG_RETURN_ERRNO(OBMAFS3_ERR_IO, "backup sb pread lba=%" PRIu64 " expected=%zu got=%zd", backup_lba,
                         sizeof(*sb), n);
    return OBMAFS3_OK;
}

/**
 * Read the backup superblock from disk (lenient).
 *
 * Like @c obmafs3_sb_read_backup but verifies the checksum and reports
 * the result via @p checksum_ok rather than failing.
 *
 * @param fd          Open file descriptor for the filesystem image.
 * @param block_size  Block size in bytes.
 * @param total_bytes Total filesystem size in bytes.
 * @param sb          Output superblock structure.
 * @param checksum_ok Set to 1 if the checksum matches, 0 otherwise.
 * @return @c OBMAFS3_OK on success, or @c OBMAFS3_ERR_IO on failure.
 */
int obmafs3_sb_read_backup_lenient(int fd, uint64_t block_size, uint64_t total_bytes, struct obmafs3_sb *sb,
                                   int *checksum_ok)
{
    int rc = obmafs3_sb_read_backup(fd, block_size, total_bytes, sb);
    if(rc != OBMAFS3_OK) return rc;

    /* Verify V1 checksum: covers bytes 0..OBMAFS3_SB_V1_SIZE-1 */
    uint8_t stored[32];
    memcpy(stored, sb->checksum, 32);
    memset(sb->checksum, 0, 32);
    uint8_t computed[32];
    obmafs3_checksum_block(sb, OBMAFS3_SB_V1_SIZE, computed);
    memcpy(sb->checksum, stored, 32);
    *checksum_ok = (memcmp(stored, computed, 32) == 0);

    /* Verify extension checksum (checksum2) */
    if(*checksum_ok)
    {
        uint8_t stored2[32];
        memcpy(stored2, sb->checksum2, 32);
        memset(sb->checksum2, 0, 32);
        uint8_t computed2[32];
        obmafs3_checksum_block((const uint8_t *)sb + OBMAFS3_SB_V1_SIZE,
                               sizeof(*sb) - OBMAFS3_SB_V1_SIZE, computed2);
        memcpy(sb->checksum2, stored2, 32);
        uint8_t zero[32];
        memset(zero, 0, 32);
        if(memcmp(stored2, zero, 32) != 0)
            *checksum_ok = (memcmp(stored2, computed2, 32) == 0);
    }

    return OBMAFS3_OK;
}
