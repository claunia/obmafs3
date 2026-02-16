/*
 * obmafsck - Check and validate an OBMAFS3 filesystem
 */
#include "obmafs.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device-or-file>\n", argv[0]);
        return 1;
    }

    int rc = obmafs3_check(argv[1]);
    return rc == OBMAFS3_OK ? 0 : 1;
}
