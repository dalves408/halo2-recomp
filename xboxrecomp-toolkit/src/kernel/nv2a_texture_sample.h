#ifndef XBOXRECOMP_NV2A_TEXTURE_SAMPLE_H
#define XBOXRECOMP_NV2A_TEXTURE_SAMPLE_H

#include <stddef.h>
#include <stdint.h>

/* Bilinear taps can be negative even though their coordinates are carried as uint32_t. */
static inline uint32_t nv2a_texture_wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;
    if (sc >= 0 && sc < n)
        return c;
    if (mode == 1)
        return (uint32_t)(((sc % n) + n) % n);
    if (mode == 2) {
        int32_t m = ((sc % (2 * n)) + 2 * n) % (2 * n);
        return (uint32_t)(m < n ? m : 2 * n - 1 - m);
    }
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);
}

static inline size_t nv2a_texture_dxt_slot(uintptr_t key, uint32_t block_bytes)
{
    /* Current DXT blocks have 8 or 16 bytes; preserve the fallback for other sizes. */
    uintptr_t block = block_bytes == 8 ? key >> 3 :
                      block_bytes == 16 ? key >> 4 : key / block_bytes;
    return (block ^ (key >> 16)) & 4095u;
}

#endif
