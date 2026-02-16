/*
 * fuse_ops.h - OBMAFS3 FUSE operation declarations
 */
#ifndef OBMAFS3_FUSE_OPS_H
#define OBMAFS3_FUSE_OPS_H

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include "obmafs.h"

extern struct fuse_operations obmafs3_fuse_ops;
extern struct obmafs3_ctx *g_ctx;

#endif /* OBMAFS3_FUSE_OPS_H */
