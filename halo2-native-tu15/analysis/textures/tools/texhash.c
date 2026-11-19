/* Byte-for-byte copy of hash_bytes() in src/gpu/d3d11_backend.c, exported for
 * the texture extractor so pack file names carry the hash the runtime can
 * compute over a texture's guest bytes.
 *
 * Build: gcc -O2 -shared -o texhash.dll texhash.c
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

__declspec(dllexport) uint64_t h2_hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
