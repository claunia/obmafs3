// /***************************************************************************
// Object Based Media Archival File System v3 (OBMAFS3)
// ----------------------------------------------------------------------------
//
// Filename       : debug.h
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : obmafs3
//
// --[ Description ] ----------------------------------------------------------
//
//     Debug and error logging macros for OBMAFS3.
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

/**
 * @file debug.h
 * @brief OBMAFS3 debug/error logging macros.
 *
 * When the environment variable @c OBMAFS3_DEBUG is set (to any
 * non-empty, non-"0" value), every error return site that uses
 * these macros will emit a line to stderr with the file, line,
 * function, and a brief message.  This is invaluable for tracking
 * down the origin of I/O errors at runtime.
 *
 * Usage in library code:
 * @code
 *   if (pread(...) < 0) DBG_RETURN(OBMAFS3_ERR_IO, "pread lba=%" PRIu64, lba);
 * @endcode
 *
 * Usage in FUSE code (returns negative errno):
 * @code
 *   if (rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "inode_put failed rc=%d", rc);
 * @endcode
 */
#ifndef OBMAFS3_DEBUG_H
#define OBMAFS3_DEBUG_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Global debug flag — non-zero when debug output is enabled. */
extern int obmafs3_debug;

/** Initialise the debug flag from the @c OBMAFS3_DEBUG environment variable. */
static inline void obmafs3_debug_init(void)
{
    const char *env = getenv("OBMAFS3_DEBUG");
    obmafs3_debug   = (env && *env && *env != '0');
}

/**
 * Log a debug message to stderr (only when obmafs3_debug is set).
 *
 * Includes file name, line number and function for instant
 * identification of the call-site.
 */
#define OBMAFS3_DBG(fmt, ...)                                                                             \
    do                                                                                                    \
    {                                                                                                     \
        if(obmafs3_debug)                                                                                 \
            fprintf(stderr, "OBMAFS3 [%s:%d %s] " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    } while(0)

/**
 * Log an error and return from a library function.
 *
 * @param err  OBMAFS3_ERR_* code to return.
 * @param fmt  printf format string describing the failure context.
 */
#define DBG_RETURN(err, fmt, ...)                                                                       \
    do                                                                                                  \
    {                                                                                                   \
        if(obmafs3_debug)                                                                               \
            fprintf(stderr, "OBMAFS3 ERR %d [%s:%d %s] " fmt "\n", (err), __FILE__, __LINE__, __func__, \
                    ##__VA_ARGS__);                                                                     \
        return (err);                                                                                   \
    } while(0)

/**
 * Log an error (with saved errno) and return from a library function.
 *
 * Captures @c errno before the fprintf call so it is not clobbered.
 *
 * @param err  OBMAFS3_ERR_* code to return.
 * @param fmt  printf format string (may reference @c _saved_errno).
 */
#define DBG_RETURN_ERRNO(err, fmt, ...)                                                                               \
    do                                                                                                                \
    {                                                                                                                 \
        int _saved_errno = errno;                                                                                     \
        if(obmafs3_debug)                                                                                             \
            fprintf(stderr, "OBMAFS3 ERR %d [%s:%d %s] " fmt " (errno=%d %s)\n", (err), __FILE__, __LINE__, __func__, \
                    ##__VA_ARGS__, _saved_errno, strerror(_saved_errno));                                             \
        return (err);                                                                                                 \
    } while(0)

/**
 * Log an error and return from a FUSE callback.
 *
 * @param err  Negative errno value (e.g. -EIO) to return.
 * @param fmt  printf format string describing the failure context.
 */
#define FUSE_RETURN(err, fmt, ...)                                                                                   \
    do                                                                                                               \
    {                                                                                                                \
        if(obmafs3_debug)                                                                                            \
            fprintf(stderr, "FUSE ERR %d [%s:%d %s] " fmt "\n", (err), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
        return (err);                                                                                                \
    } while(0)

/**
 * Propagate a non-OK return code, logging the call-site.
 *
 * Unlike DBG_RETURN this does not require a format string — it just
 * notes the file/line/function where the error is being forwarded.
 *
 * @param rc  The return code to propagate.
 */
#define DBG_PROPAGATE(rc)                                                                            \
    do                                                                                               \
    {                                                                                                \
        int _prop_rc = (rc);                                                                         \
        if(_prop_rc != 0 && obmafs3_debug)                                                           \
            fprintf(stderr, "OBMAFS3 PROP %d [%s:%d %s]\n", _prop_rc, __FILE__, __LINE__, __func__); \
        return _prop_rc;                                                                             \
    } while(0)

#endif /* OBMAFS3_DEBUG_H */
