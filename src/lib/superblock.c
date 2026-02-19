/*
 * superblock.c - OBMAFS3 superblock read/write/validation
 */
#include "obmafs.h"
#include "debug.h"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

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
        DBG_RETURN_ERRNO(OBMAFS3_ERR_IO,
                         "sb pread expected=%zu got=%zd", sizeof(*sb), n);
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
    ssize_t n = pwrite(fd, sb, sizeof(*sb), 0);
    if(n < 0 || (size_t)n != sizeof(*sb))
        DBG_RETURN_ERRNO(OBMAFS3_ERR_IO,
                         "sb pwrite expected=%zu got=%zd", sizeof(*sb), n);
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
        DBG_RETURN(OBMAFS3_ERR_BADMAGIC,
                   "bad magic 0x%" PRIx64 " expected 0x%" PRIx64,
                   sb->magic, (uint64_t)OBMAFS3_SB_MAGIC);
    if(sb->block_size == 0 || sb->dedup_block_size == 0)
        DBG_RETURN(OBMAFS3_ERR_INVAL,
                   "bad block_size=%" PRIu64 " dedup_block_size=%" PRIu64,
                   sb->block_size, sb->dedup_block_size);
    if(sb->total_bytes == 0)
        DBG_RETURN(OBMAFS3_ERR_INVAL, "total_bytes=0");
    if(sb->catalog_lba == 0 || sb->inode_lba == 0)
        DBG_RETURN(OBMAFS3_ERR_INVAL,
                   "catalog_lba=%" PRIu64 " inode_lba=%" PRIu64,
                   sb->catalog_lba, sb->inode_lba);
    return OBMAFS3_OK;
}
