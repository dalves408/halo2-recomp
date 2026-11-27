/* h2_content.c: downloaded content (the map packs) signed for the hard-drive key the game runs with.
 *
 * Each package in <data>\TDATA\4d530064\$c\<offering id>\ has a ContentMeta.xbx whose first 20 bytes are
 * HMAC-SHA1(HMAC-SHA1(hard-drive key, title id), header from +0x14), and XAPI drops a package whose digest does not
 * match. The key is the player's console's when a console identity is given, else 16 zero bytes (kernel_bridge.c,
 * XboxHDKey). Packs installed from the Map Pack disc (halo2_mappack) are signed for zero bytes, and packs downloaded
 * from Insignia for the identity's key, so adding or removing an identity would hide them. At every start, before the
 * game runs, a ContentMeta.xbx that does not match the current key is signed again for it. Nothing else in the file
 * changes, and a package already right is not written. */
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "h2_layout.h"

int xbox_ConsoleHdKey(uint8_t out[16]);

static int hmac_sha1(const uint8_t *key, ULONG kn, const uint8_t *d, ULONG n, uint8_t out[20])
{
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return 0;
    if (!BCryptCreateHash(a, &h, NULL, 0, (PUCHAR)key, kn, 0)) {
        ok = !BCryptHashData(h, (PUCHAR)d, n, 0) && !BCryptFinishHash(h, out, 20, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

/* 1 signed again, 0 already right, -1 not a content header or not writable */
static int resign(const char *path, const uint8_t hdkey[16])
{
    uint8_t b[8192], key[20], dig[20];
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0, w;
    uint32_t hdr, tid;
    int r = -1;
    if (f == INVALID_HANDLE_VALUE) return -1;
    if (ReadFile(f, b, sizeof b, &n, NULL) && n >= 0x28 && !memcmp(b + 20, "XCSF", 4)) {
        memcpy(&hdr, b + 0x18, 4);
        memcpy(&tid, b + 0x24, 4);
        if (hdr > 0x14 && hdr <= n && hmac_sha1(hdkey, 16, (uint8_t *)&tid, 4, key) &&
            hmac_sha1(key, 20, b + 0x14, hdr - 0x14, dig)) {
            if (!memcmp(dig, b, 20)) r = 0;
            else {
                LARGE_INTEGER z;
                z.QuadPart = 0;
                r = SetFilePointerEx(f, z, NULL, FILE_BEGIN) && WriteFile(f, dig, 20, &w, NULL) && w == 20 ? 1 : -1;
            }
        }
    }
    CloseHandle(f);
    return r;
}

void halo2_content_sign(void)
{
    uint8_t hdkey[16];
    char root[MAX_PATH], dir[MAX_PATH], pat[MAX_PATH], meta[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int fixed = 0, bad = 0, n = 0;
    memset(hdkey, 0, sizeof hdkey);
    xbox_ConsoleHdKey(hdkey);
    halo2_install_root(root, sizeof root);
    h2_data_path(root, "TDATA\\4d530064\\$c", dir, sizeof dir);
    snprintf(pat, sizeof pat, "%s\\*", dir);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        int r;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        snprintf(meta, sizeof meta, "%s\\%s\\ContentMeta.xbx", dir, fd.cFileName);
        if (GetFileAttributesA(meta) == INVALID_FILE_ATTRIBUTES) continue;
        n++;
        r = resign(meta, hdkey);
        if (r > 0) fixed++;
        else if (r < 0) bad++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    SecureZeroMemory(hdkey, sizeof hdkey);
    fprintf(stderr, "[CONTENT] %d downloaded content package(s); %d signed again for this hard-drive key%s\n",
            n, fixed, bad ? "; some could not be read or written" : "");
}
