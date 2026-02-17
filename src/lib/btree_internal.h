/*
 * btree_internal.h - Internal helpers shared across B+Tree source files
 */
#ifndef BTREE_INTERNAL_H
#define BTREE_INTERNAL_H

#include "obmafs.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * Compute and store the checksum for a btree node block.
 * The node size is derived from keys_length in the header.
 */
static inline void compute_node_checksum(uint8_t *buf)
{
    struct btree_node_header *nhdr = (struct btree_node_header *)buf;
    size_t data_size = sizeof(struct btree_node_header) + nhdr->keys_length;
    memset(nhdr->checksum, 0, sizeof(nhdr->checksum));
    obmafs3_checksum_block(buf, data_size, nhdr->checksum);
}

#endif /* BTREE_INTERNAL_H */
