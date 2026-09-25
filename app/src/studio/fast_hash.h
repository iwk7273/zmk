/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <pb_encode.h>

/* Hash the exact protobuf bytes used by the read-only fast-load responses. */
#define STUDIO_FAST_HASH_OFFSET UINT64_C(14695981039346656037)
#define STUDIO_FAST_HASH_PRIME UINT64_C(1099511628211)

static bool studio_fast_hash_write(pb_ostream_t *stream, const uint8_t *buf, size_t count) {
    uint64_t *hash = (uint64_t *)stream->state;
    for (size_t i = 0; i < count; i++) {
        *hash = (*hash ^ buf[i]) * STUDIO_FAST_HASH_PRIME;
    }
    return true;
}

static pb_ostream_t studio_fast_hash_stream(uint64_t *hash) {
    *hash = STUDIO_FAST_HASH_OFFSET;
    pb_ostream_t stream = {studio_fast_hash_write, hash, SIZE_MAX, 0};
    return stream;
}
