/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/stdlib.h>
#include <string.h>

/*
 * ANSI C version of strlcpy
 * Based on the NetBSD strlcpy man page.
 *
 * Nathan Myers <ncm-nospam@cantrip.org>, 2003/06/03
 * Placed in the public domain.
 *
 * Terminate at the copied length. Only writing dst[size-1] leaves leftover
 * characters from a previous longer string.
 */

size_t strlcpy(char *dst, const char *src, size_t size) {
    const size_t len = strlen(src);
    if (size != 0) {
        const size_t copy = (len >= size) ? size - 1 : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
