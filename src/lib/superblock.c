/*
 * superblock.c - OBMAFS3 superblock read/write/validation
 */
#include "obmafs.h"

#include <string.h>
#include <unistd.h>

int obmafs3_sb_read(int fd, struct obmafs3_sb *sb)
{
    ssize_t n = pread(fd, sb, sizeof(*sb), 0);
    if (n < 0 || (size_t)n != sizeof(*sb))
        return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

int obmafs3_sb_write(int fd, const struct obmafs3_sb *sb)
{
    ssize_t n = pwrite(fd, sb, sizeof(*sb), 0);
    if (n < 0 || (size_t)n != sizeof(*sb))
        return OBMAFS3_ERR_IO;
    return OBMAFS3_OK;
}

int obmafs3_sb_validate(const struct obmafs3_sb *sb)
{
    if (sb->magic != OBMAFS3_SB_MAGIC)
        return OBMAFS3_ERR_BADMAGIC;
    if (sb->block_size == 0 || sb->dedup_block_size == 0)
        return OBMAFS3_ERR_INVAL;
    if (sb->total_bytes == 0)
        return OBMAFS3_ERR_INVAL;
    if (sb->catalog_lba == 0 || sb->inode_lba == 0)
        return OBMAFS3_ERR_INVAL;
    return OBMAFS3_OK;
}
