/* halo2_import: copies the game's files out of the player's own copy of the disc and title update 1.5
 * (after Marvel vs. Capcom 2's mvc2_import).
 *
 *   halo2_import <source> <dest_dir> [--update <title update>]
 *   halo2_import --prepare <source> <dest_dir> [--mappack=<disc>] [--mappack-videos]   (RecompLauncher setup)
 *
 * <source> is an Xbox disc image (.iso: a full Redump image or a plain XISO) or a folder holding the disc's files
 * (the one with default.xbe). A folder that already has title update 1.5 applied is a complete source on its own.
 * <title update> is the update's folder from the console's drive (E:\TDATA\4D530064\$u, with default.xbe and
 * downloader.xbe) or its default.xbe. Without --update it is also looked for beside the disc image or folder (a $u
 * folder, a TitleUpdate folder, or the .xbe files themselves), in the install's data\TitleUpdate (where RecompLauncher
 * puts the update it asks for) and in <dest_dir> (a repair keeps it).
 *
 * Without a title update the disc's own default.xbe and downloader.xbe (the retail 1.0 files) are patched to 1.5
 * with the patches beside this program (patches\<file>.tu15.h2p, scripts/convert_tu_patches.py); each result must
 * match the pinned 1.5 file exactly, so a player's own update and the patch give the same game.
 *
 * Every file in manifest.h is written to <dest_dir> and checked against its pinned size and SHA-256 as it is copied;
 * nothing else is. Progress goes to stdout as "PROGRESS done total".
 *
 * --prepare is the RecompLauncher "prepare" step: no window, progress as "progress <0..1> <text>" lines, exit 0 only
 * when <dest_dir> holds a complete, verified copy. The files go to a staging folder beside <dest_dir> that is swapped
 * in at the end, so a failed run never leaves a half-written game folder (an existing one is kept until then).
 * A folder that already holds every file is kept, with no copy (RecompLauncher runs this again for every new
 * release; see sdk_prepare).
 *
 * --mappack=<disc> (RecompLauncher's optional "Map Pack disc" input; the argument is left out when the player gives
 * none): once the game files are ready, halo2_mappack.exe (beside this program) installs the Halo 2 Multiplayer Map
 * Pack disc's maps into <install>\data, and with --mappack-videos (a tick on the launcher's setup screen) its bonus
 * videos into <install>\extras, <install> being <dest_dir>'s parent. Its progress is the last part of this step's.
 * Files already there and right are kept, so running it again costs only the checks. Exit codes 8 (not the Map Pack
 * disc, or it could not be read) and 9 (its files are damaged) are the Map Pack's; 6 is any failed write.
 *
 * Exit codes: 0 ok, 2 source not found, 3 not an Xbox disc image, 4 not the supported Halo 2 disc, 5 files missing
 * or damaged, 6 cannot write, 7 title update 1.5 not found or not the right one (and no patch to make it). */
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "manifest.h"

#define SECTOR 2048ull
static const unsigned long long XDVDFS_OFFSETS[] = { 0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull };

typedef struct { unsigned long long sector, size; int found; } DiscFile;

static unsigned long long g_done, g_total;
static int g_have[MANIFEST_COUNT];
static WCHAR g_dest[MAX_PATH];
static HANDLE g_img = INVALID_HANDLE_VALUE;
static unsigned long long g_base;
static DiscFile g_disc[MANIFEST_COUNT];
static unsigned long long g_disc_xbe_sector;
static int g_disc_xbe_found;
static int g_sdk;
static double g_sdk_span = 1.0;   /* the game files' share of the progress (less with a Map Pack disc after them) */
/* the retail disc's default.xbe and downloader.xbe, kept for the title update patch */
static const unsigned char DISC_DL_SHA256[32] = {   /* 9b10c579...c03c698 */
    0x9b,0x10,0xc5,0x79,0x6f,0x73,0x75,0xdc,0x73,0x35,0x2d,0x77,0x51,0xae,0x45,0xa8,
    0x1d,0xb8,0xbe,0x39,0x06,0xab,0x3f,0x52,0x3d,0x21,0xf7,0xff,0x1c,0x03,0xc6,0x98 };
#define DISC_DL_SIZE 1134592ULL
static unsigned long long g_disc_dl_sector;
static int g_disc_dl_found;
static unsigned char *g_retail[2];          /* [0] default.xbe, [1] downloader.xbe */
static size_t g_retail_n[2];
static void keep_retail(int k, unsigned char *b, size_t n);
static unsigned char *read_all(const WCHAR *path, size_t *n);

static const char *g_progress_text = "Copying game files";
static void progress(void)
{
    if (g_sdk)
        printf("progress %.4f %s\n", g_sdk_span * (0.05 + 0.9 * (double)g_done / (double)(g_total ? g_total : 1)),
               g_progress_text);
    else
        printf("PROGRESS %llu %llu\n", g_done, g_total);
    fflush(stdout);
}

static int manifest_find(const char *rel)
{
    int i;
    for (i = 0; i < MANIFEST_COUNT; i++)
        if (_stricmp(MANIFEST[i].path, rel) == 0) return i;
    return -1;
}

static int make_dirs_for(const WCHAR *file)
{
    WCHAR dir[MAX_PATH], *sl;
    wcscpy_s(dir, MAX_PATH, file);
    if (!(sl = wcsrchr(dir, L'\\'))) return 1;
    *sl = 0;
    return SHCreateDirectoryExW(NULL, dir, NULL) == ERROR_SUCCESS || GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
}

static void rel_w(const char *rel, WCHAR *out)
{
    WCHAR *p;
    MultiByteToWideChar(CP_ACP, 0, rel, -1, out, MAX_PATH);
    for (p = out; *p; p++) if (*p == L'/') *p = L'\\';
}

static void dest_path(const char *rel, WCHAR *out)
{
    WCHAR w[MAX_PATH];
    rel_w(rel, w);
    swprintf_s(out, MAX_PATH, L"%s\\%s", g_dest, w);
}

/* ---- SHA-256 while copying ------------------------------------------------------------------------------------ */
typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } Sha;
static int sha_begin(Sha *s)
{
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    if (BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0)) { BCryptCloseAlgorithmProvider(s->alg, 0); return 0; }
    return 1;
}
static void sha_end(Sha *s, unsigned char out[32])
{
    BCryptFinishHash(s->h, out, 32, 0);
    BCryptDestroyHash(s->h);
    BCryptCloseAlgorithmProvider(s->alg, 0);
}

/* Copy len bytes from `in` (positioned) to dest, or only hash them (dest NULL). 1 ok and the hash matches, 0 the
 * hash or size is wrong, -1 cannot write. */
static int copy_checked(HANDLE in, unsigned long long len, const unsigned char want[32], const char *dest_rel,
                        int count)
{
    static unsigned char buf[1 << 20];
    unsigned char got[32];
    WCHAR dst[MAX_PATH];
    HANDLE out = INVALID_HANDLE_VALUE;
    unsigned long long left = len;
    DWORD n, w;
    Sha s;
    if (!sha_begin(&s)) return -1;
    if (dest_rel) {
        dest_path(dest_rel, dst);
        if (!make_dirs_for(dst)) { sha_end(&s, got); return -1; }
        out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) { sha_end(&s, got); return -1; }
    }
    while (left) {
        DWORD want_n = left > sizeof buf ? (DWORD)sizeof buf : (DWORD)left;
        if (!ReadFile(in, buf, want_n, &n, NULL) || n == 0) break;
        BCryptHashData(s.h, buf, n, 0);
        if (out != INVALID_HANDLE_VALUE && (!WriteFile(out, buf, n, &w, NULL) || w != n)) {
            CloseHandle(out); DeleteFileW(dst); sha_end(&s, got); return -1;
        }
        left -= n;
        if (count) { g_done += n; progress(); }
    }
    sha_end(&s, got);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (left || memcmp(got, want, 32)) { if (dest_rel) DeleteFileW(dst); return 0; }
    return 1;
}

static int file_matches(const WCHAR *path, unsigned long long size, const unsigned char sha[32])
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz;
    int ok = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (GetFileSizeEx(f, &sz) && (unsigned long long)sz.QuadPart == size)
        ok = copy_checked(f, size, sha, NULL, 0) == 1;
    CloseHandle(f);
    return ok;
}

/* Copy one manifest entry from a file. 1 ok, 0 missing or wrong, -1 cannot write. */
static int copy_file_entry(int i, const WCHAR *src)
{
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz;
    int r = 0;
    if (in == INVALID_HANDLE_VALUE) return 0;
    if (GetFileSizeEx(in, &sz) && (unsigned long long)sz.QuadPart == MANIFEST[i].size)
        r = copy_checked(in, MANIFEST[i].size, MANIFEST[i].sha256, MANIFEST[i].path, 1);
    CloseHandle(in);
    return r;
}

/* ---- disc image (XDVDFS) -------------------------------------------------------------------------------------- */
static int img_read(unsigned long long off, void *buf, DWORD n)
{
    LARGE_INTEGER p;
    DWORD got;
    p.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(g_img, p, NULL, FILE_BEGIN) && ReadFile(g_img, buf, n, &got, NULL) && got == n;
}

static int walk(unsigned long long dir_sector, unsigned dir_size, const char *prefix, int depth)
{
    unsigned char *d;
    struct { unsigned pos; int state; } st[512];
    int sp = 1;
    if (!dir_size || depth > 16) return 1;
    if (!(d = (unsigned char *)malloc(dir_size)) || !img_read(g_base + dir_sector * SECTOR, d, dir_size)) {
        free(d);
        return 0;
    }
    st[0].pos = 0; st[0].state = 0;
    while (sp) {
        unsigned pos = st[sp - 1].pos, left, right, sector, size, nl;
        int s = st[sp - 1].state;
        unsigned char *e = d + pos;
        char name[256], rel[600];
        if (pos + 14 > dir_size) { sp--; continue; }
        left = e[0] | (e[1] << 8);
        right = e[2] | (e[3] << 8);
        if (left == 0xFFFF) { sp--; continue; }
        if (s == 0) {
            st[sp - 1].state = 1;
            if (left && sp < 512) { st[sp].pos = left * 4; st[sp].state = 0; sp++; }
            continue;
        }
        if (s == 1) {
            st[sp - 1].state = 2;
            sector = e[4] | (e[5] << 8) | (e[6] << 16) | ((unsigned)e[7] << 24);
            size = e[8] | (e[9] << 8) | (e[10] << 16) | ((unsigned)e[11] << 24);
            nl = e[13];
            if (pos + 14 + nl > dir_size) { sp--; continue; }
            memcpy(name, e + 14, nl);
            name[nl] = 0;
            snprintf(rel, sizeof rel, "%s%s", prefix, name);
            if (e[12] & 0x10) {
                char sub[600];
                snprintf(sub, sizeof sub, "%s/", rel);
                if (!walk(sector, size, sub, depth + 1)) { free(d); return 0; }
            } else if (!_stricmp(rel, "default.xbe")) {
                if (size == DISC_XBE_SIZE) { g_disc_xbe_sector = sector; g_disc_xbe_found = 1; }
            } else if (!_stricmp(rel, "downloader.xbe")) {
                if (size == DISC_DL_SIZE) { g_disc_dl_sector = sector; g_disc_dl_found = 1; }
            } else {
                int mi = manifest_find(rel);
                if (mi >= 0 && MANIFEST[mi].from == 0) {
                    g_disc[mi].sector = sector; g_disc[mi].size = size; g_disc[mi].found = 1;
                }
            }
            continue;
        }
        sp--;
        if (right && sp < 511) { st[sp].pos = right * 4; st[sp].state = 0; sp++; }
    }
    free(d);
    return 1;
}

static int find_xdvdfs(unsigned long long *root_sector, unsigned *root_size)
{
    unsigned char v[28];
    size_t i;
    for (i = 0; i < sizeof XDVDFS_OFFSETS / sizeof *XDVDFS_OFFSETS; i++)
        if (img_read(XDVDFS_OFFSETS[i] + 32 * SECTOR, v, 28) && memcmp(v, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
            g_base = XDVDFS_OFFSETS[i];
            *root_sector = v[20] | (v[21] << 8) | (v[22] << 16) | ((unsigned)v[23] << 24);
            *root_size = v[24] | (v[25] << 8) | (v[26] << 16) | ((unsigned)v[27] << 24);
            return 1;
        }
    return 0;
}

static int import_image(const WCHAR *source)
{
    unsigned long long rs;
    unsigned rsz;
    LARGE_INTEGER p;
    int i, missing = 0;
    g_img = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (g_img == INVALID_HANDLE_VALUE) { printf("ERROR The disc image could not be opened.\n"); return 2; }
    if (!find_xdvdfs(&rs, &rsz)) { printf("ERROR This is not an Xbox disc image.\n"); return 3; }
    if (!walk(rs, rsz, "", 0)) { printf("ERROR The disc image is damaged or incomplete.\n"); return 5; }
    /* the right disc: its own default.xbe, hashed in place */
    p.QuadPart = (LONGLONG)(g_base + g_disc_xbe_sector * SECTOR);
    if (!g_disc_xbe_found || !SetFilePointerEx(g_img, p, NULL, FILE_BEGIN) ||
        copy_checked(g_img, DISC_XBE_SIZE, DISC_XBE_SHA256, NULL, 0) != 1) {
        printf("ERROR This is not the supported Halo 2 disc (USA, Europe): its default.xbe differs.\n");
        return 4;
    }
    {   /* the disc's own default.xbe and downloader.xbe, for the title update patch */
        unsigned char *b = malloc((size_t)DISC_XBE_SIZE);
        if (b && img_read(g_base + g_disc_xbe_sector * SECTOR, b, (DWORD)DISC_XBE_SIZE)) keep_retail(0, b, (size_t)DISC_XBE_SIZE);
        else free(b);
        if (g_disc_dl_found && (b = malloc((size_t)DISC_DL_SIZE)) != NULL) {
            if (img_read(g_base + g_disc_dl_sector * SECTOR, b, (DWORD)DISC_DL_SIZE)) keep_retail(1, b, (size_t)DISC_DL_SIZE);
            else free(b);
        }
    }
    for (i = 0; i < MANIFEST_COUNT; i++) {
        int r;
        if (MANIFEST[i].from != 0 || g_have[i]) continue;
        if (!g_disc[i].found || g_disc[i].size != MANIFEST[i].size) { if (missing++ < 5) printf("MISSING %s\n", MANIFEST[i].path); continue; }
        p.QuadPart = (LONGLONG)(g_base + g_disc[i].sector * SECTOR);
        SetFilePointerEx(g_img, p, NULL, FILE_BEGIN);
        r = copy_checked(g_img, MANIFEST[i].size, MANIFEST[i].sha256, MANIFEST[i].path, 1);
        if (r < 0) { printf("ERROR Could not write to the install folder.\n"); return 6; }
        if (!r) { if (missing++ < 5) printf("DAMAGED %s\n", MANIFEST[i].path); continue; }
        g_have[i] = 1;
    }
    CloseHandle(g_img);
    g_img = INVALID_HANDLE_VALUE;
    if (missing) { printf("ERROR %d game files are missing or damaged (bad or incomplete dump).\n", missing); return 5; }
    return 0;
}

/* ---- folders ---------------------------------------------------------------------------------------------------- */
/* the folder that holds default.xbe: the folder itself or one of its children */
static int find_root(const WCHAR *dir, WCHAR *root)
{
    WCHAR pat[MAX_PATH], t[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(t, MAX_PATH, L"%s\\default.xbe", dir);
    if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(root, MAX_PATH, dir); return 1; }
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
            swprintf_s(t, MAX_PATH, L"%s\\%s\\default.xbe", dir, fd.cFileName);
            if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) {
                swprintf_s(root, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

static int import_folder(const WCHAR *source)
{
    WCHAR root[MAX_PATH], t[MAX_PATH], rel[MAX_PATH];
    int i, missing = 0, tu = -1, r;
    if (!find_root(source, root)) { printf("ERROR default.xbe was not found in that folder.\n"); return 4; }
    swprintf_s(t, MAX_PATH, L"%s\\default.xbe", root);
    for (i = 0; i < MANIFEST_COUNT; i++)
        if (!_stricmp(MANIFEST[i].path, "default.xbe")) tu = i;
    /* the disc's files, or the disc's files with the title update already applied */
    if (!file_matches(t, DISC_XBE_SIZE, DISC_XBE_SHA256) &&
        !(tu >= 0 && file_matches(t, MANIFEST[tu].size, MANIFEST[tu].sha256))) {
        printf("ERROR This is not the supported Halo 2 disc (USA, Europe): its default.xbe differs.\n");
        return 4;
    }
    {   /* a retail folder: its default.xbe and downloader.xbe, for the title update patch */
        size_t n;
        unsigned char *b;
        if ((b = read_all(t, &n)) != NULL) keep_retail(0, b, n);
        swprintf_s(t, MAX_PATH, L"%s\\downloader.xbe", root);
        if ((b = read_all(t, &n)) != NULL) keep_retail(1, b, n);
    }
    for (i = 0; i < MANIFEST_COUNT; i++) {
        if (g_have[i]) continue;
        rel_w(MANIFEST[i].path, rel);
        swprintf_s(t, MAX_PATH, L"%s\\%s", root, rel);
        r = copy_file_entry(i, t);
        if (r < 0) { printf("ERROR Could not write to the install folder.\n"); return 6; }
        if (r) { g_have[i] = 1; continue; }
        if (MANIFEST[i].from == 0 && missing++ < 5) printf("MISSING %s\n", MANIFEST[i].path);
    }
    if (missing) { printf("ERROR %d game files are missing or damaged (bad or incomplete dump).\n", missing); return 5; }
    return 0;
}

/* Title update 1.5's files from a folder (or the folder of a given .xbe). 1 all there, -1 cannot write. */
static int update_from(const WCHAR *where)
{
    WCHAR dir[MAX_PATH], t[MAX_PATH], rel[MAX_PATH], *sl, *name;
    DWORD a = GetFileAttributesW(where);
    int i, r, all = 1;
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    wcscpy_s(dir, MAX_PATH, where);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) && (sl = wcsrchr(dir, L'\\')) != NULL) *sl = 0;
    for (i = 0; i < MANIFEST_COUNT; i++) {
        if (MANIFEST[i].from != 1 || g_have[i]) continue;
        rel_w(MANIFEST[i].path, rel);
        name = wcsrchr(rel, L'\\') ? wcsrchr(rel, L'\\') + 1 : rel;
        swprintf_s(t, MAX_PATH, L"%s\\%s", dir, name);
        r = copy_file_entry(i, t);
        if (r < 0) return -1;
        if (r) g_have[i] = 1; else all = 0;
    }
    return all;
}

static int have_update(void)
{
    int i;
    for (i = 0; i < MANIFEST_COUNT; i++)
        if (MANIFEST[i].from == 1 && !g_have[i]) return 0;
    return 1;
}

/* Explicit --update, else beside the source: <dir>, <dir>\$u, <dir>\TitleUpdate, <dir>\TDATA\4D530064\$u; else the
 * copy already in the game folder (a repair). */
static int find_update(const WCHAR *source, const WCHAR *update, const WCHAR *existing)
{
    static const WCHAR *const subs[] = { L"", L"\\$u", L"\\TitleUpdate", L"\\TDATA\\4D530064\\$u", L"\\4D530064\\$u" };
    WCHAR dir[MAX_PATH], t[MAX_PATH], *sl;
    DWORD a;
    size_t i;
    if (update && *update) return update_from(update) < 0 ? -1 : have_update();
    wcscpy_s(dir, MAX_PATH, source);
    a = GetFileAttributesW(dir);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && (sl = wcsrchr(dir, L'\\')) != NULL) *sl = 0;
    for (i = 0; i < sizeof subs / sizeof *subs && !have_update(); i++) {
        swprintf_s(t, MAX_PATH, L"%s%s", dir, subs[i]);
        if (update_from(t) < 0) return -1;
    }
    if (!have_update()) {               /* RecompLauncher puts it in <install>\data\TitleUpdate (game.update) */
        wcscpy_s(dir, MAX_PATH, g_dest);
        if ((sl = wcsrchr(dir, L'\\')) != NULL) {
            *sl = 0;
            swprintf_s(t, MAX_PATH, L"%s\\data\\TitleUpdate", dir);
            if (update_from(t) < 0) return -1;
        }
    }
    if (!have_update() && existing && *existing) {
        WCHAR copy[MAX_PATH];               /* the game folder is written to: read the old copy from a side copy */
        swprintf_s(copy, MAX_PATH, L"%s.tu-%lu", g_dest, (unsigned long)GetCurrentProcessId());
        for (i = 0; i < MANIFEST_COUNT; i++)
            if (MANIFEST[i].from == 1) {
                WCHAR rel[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
                rel_w(MANIFEST[i].path, rel);
                swprintf_s(src, MAX_PATH, L"%s\\%s", existing, rel);
                swprintf_s(dst, MAX_PATH, L"%s\\%s", copy, rel);
                make_dirs_for(dst);
                CopyFileW(src, dst, FALSE);
            }
        if (update_from(copy) < 0) return -1;
        {   /* remove the side copy */
            WCHAR buf[MAX_PATH + 2];
            SHFILEOPSTRUCTW op;
            size_t n = wcslen(copy);
            memcpy(buf, copy, (n + 1) * sizeof(WCHAR));
            buf[n + 1] = 0;
            memset(&op, 0, sizeof op);
            op.wFunc = FO_DELETE; op.pFrom = buf;
            op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            SHFileOperationW(&op);
        }
    }
    return have_update();
}


/* ---- title update 1.5 from the included patch ------------------------------------------------------------------- */
static int sha_of(const unsigned char *b, size_t n, unsigned char out[32])
{
    Sha s;
    if (!sha_begin(&s)) return 0;
    BCryptHashData(s.h, (PUCHAR)b, (ULONG)n, 0);
    sha_end(&s, out);
    return 1;
}

/* Keep one of the disc's own files (the retail default.xbe or downloader.xbe) when it is the known one. */
static void keep_retail(int k, unsigned char *b, size_t n)
{
    unsigned char got[32];
    const unsigned char *want = k ? DISC_DL_SHA256 : DISC_XBE_SHA256;
    if (n == (k ? DISC_DL_SIZE : DISC_XBE_SIZE) && sha_of(b, n, got) && !memcmp(got, want, 32)) {
        free(g_retail[k]);
        g_retail[k] = b;
        g_retail_n[k] = n;
    } else
        free(b);
}

static unsigned char *read_all(const WCHAR *path, size_t *n)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz;
    unsigned char *b = NULL;
    DWORD got;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 28) && (b = malloc((size_t)sz.QuadPart)) != NULL &&
        (!ReadFile(f, b, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)) {
        free(b);
        b = NULL;
    }
    CloseHandle(f);
    if (b) *n = (size_t)sz.QuadPart;
    return b;
}

static long long offtin(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return (v & 0x8000000000000000ull) ? -(long long)(v & 0x7FFFFFFFFFFFFFFFull) : (long long)v;
}

/* bspatch over an .h2p (BSDIFF40 with its blocks stored, scripts/convert_tu_patches.py). NULL on a bad patch. */
static unsigned char *bspatch(const unsigned char *old, size_t old_n, const unsigned char *p, size_t pn, size_t *out_n)
{
    long long clen, dlen, nsize, np = 0, op = 0, x, y, z, i;
    const unsigned char *ctrl, *diff, *extra, *end = p + pn;
    unsigned char *nw;
    if (pn < 32 || memcmp(p, "H2TUPAT1", 8)) return NULL;
    clen = offtin(p + 8); dlen = offtin(p + 16); nsize = offtin(p + 24);
    if (clen < 0 || dlen < 0 || nsize <= 0 || nsize > (1 << 28) || 32 + clen + dlen > (long long)pn) return NULL;
    ctrl = p + 32; diff = ctrl + clen; extra = diff + dlen;
    if (!(nw = malloc((size_t)nsize))) return NULL;
    while (np < nsize) {
        if (ctrl + 24 > diff) goto bad;
        x = offtin(ctrl); y = offtin(ctrl + 8); z = offtin(ctrl + 16); ctrl += 24;
        if (x < 0 || y < 0 || np + x + y > nsize || diff + x > extra || extra + y > end) goto bad;
        for (i = 0; i < x; i++)
            nw[np + i] = (unsigned char)(diff[i] + (op + i >= 0 && op + i < (long long)old_n ? old[op + i] : 0));
        diff += x; np += x; op += x;
        memcpy(nw + np, extra, (size_t)y);
        extra += y; np += y; op += z;
    }
    *out_n = (size_t)nsize;
    return nw;
bad:
    free(nw);
    return NULL;
}

/* Write a whole buffer as manifest entry i, checked. 1 ok, 0 wrong, -1 cannot write. */
static int write_entry(int i, const unsigned char *b, size_t n)
{
    unsigned char got[32];
    WCHAR dst[MAX_PATH];
    HANDLE out;
    DWORD w;
    if (n != MANIFEST[i].size || !sha_of(b, n, got) || memcmp(got, MANIFEST[i].sha256, 32)) return 0;
    dest_path(MANIFEST[i].path, dst);
    if (!make_dirs_for(dst)) return -1;
    out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return -1;
    if (!WriteFile(out, b, (DWORD)n, &w, NULL) || w != n) { CloseHandle(out); DeleteFileW(dst); return -1; }
    CloseHandle(out);
    g_done += n;
    progress();
    return 1;
}

/* The title update's files made from the disc's own with the included patches. 1 done, 0 not possible (no retail
 * files or no patches), -1 cannot write. */
static int patch_update(void)
{
    WCHAR dir[MAX_PATH], pp[MAX_PATH], *sl;
    int i, k;
    if (!GetModuleFileNameW(NULL, dir, MAX_PATH) || !(sl = wcsrchr(dir, L'\\'))) return 0;
    *sl = 0;
    for (i = 0; i < MANIFEST_COUNT; i++) {
        unsigned char *patch, *nw;
        size_t pn, nn;
        int r;
        if (MANIFEST[i].from != 1 || g_have[i]) continue;
        k = !_stricmp(MANIFEST[i].path, "downloader.xbe") ? 1 : !_stricmp(MANIFEST[i].path, "default.xbe") ? 0 : -1;
        if (k < 0 || !g_retail[k]) return 0;
        swprintf_s(pp, MAX_PATH, L"%s\\patches\\%hs.tu15.h2p", dir, MANIFEST[i].path);
        if (!(patch = read_all(pp, &pn))) return 0;
        nw = bspatch(g_retail[k], g_retail_n[k], patch, pn, &nn);
        free(patch);
        if (!nw) { printf("ERROR The included title update patch for %s is damaged.\n", MANIFEST[i].path); return 0; }
        r = write_entry(i, nw, nn);
        free(nw);
        if (r < 0) return -1;
        if (!r) { printf("ERROR The patched %s does not match title update 1.5.\n", MANIFEST[i].path); return 0; }
        g_have[i] = 1;
    }
    return have_update();
}

static void manifest_id(char out[65]);
static void stamp_remove(const WCHAR *dir);
static void stamp_write(const WCHAR *dir, const char *id);

static int run_import(const WCHAR *source, const WCHAR *destination, const WCHAR *update, const WCHAR *existing)
{
    DWORD attr;
    int i, r;
    size_t n;

    g_done = 0; g_total = MANIFEST_BYTES; g_base = 0; g_disc_xbe_found = 0; g_disc_dl_found = 0;
    memset(g_have, 0, sizeof g_have);
    memset(g_disc, 0, sizeof g_disc);
    wcscpy_s(g_dest, MAX_PATH, destination);
    n = wcslen(g_dest);
    while (n > 3 && (g_dest[n - 1] == L'\\' || g_dest[n - 1] == L'/')) g_dest[--n] = 0;
    attr = GetFileAttributesW(source);
    if (attr == INVALID_FILE_ATTRIBUTES) { printf("ERROR The disc image or folder was not found.\n"); return 2; }
    if (SHCreateDirectoryExW(NULL, g_dest, NULL) != ERROR_SUCCESS && GetFileAttributesW(g_dest) == INVALID_FILE_ATTRIBUTES) {
        printf("ERROR Could not write to the install folder.\n");
        return 6;
    }
    stamp_remove(g_dest);               /* the folder is about to change */
    r = (attr & FILE_ATTRIBUTE_DIRECTORY) ? import_folder(source) : import_image(source);
    if (r) return r;
    r = find_update(source, update, existing);
    if (r < 0) { printf("ERROR Could not write to the install folder.\n"); return 6; }
    if (!r && !(update && *update)) {    /* no update given or found: make it from the disc's own files */
        r = patch_update();
        if (r < 0) { printf("ERROR Could not write to the install folder.\n"); return 6; }
        if (r) printf("PATCHED title update 1.5 made from the disc's files with the included patch\n");
    }
    if (!r) {
        for (i = 0; i < MANIFEST_COUNT; i++)
            if (MANIFEST[i].from == 1 && !g_have[i]) printf("MISSING %s (title update 1.5)\n", MANIFEST[i].path);
        printf("ERROR Title update 1.5 could not be made from the disc's files (or, given with --update, is not the "
               "right one).\n");
        return 7;
    }
    {   /* every file copied and checked: Setup's install is stamped too (see stamp_write) */
        char id[65];
        manifest_id(id);
        if (id[0])
            stamp_write(g_dest, id);
    }
    printf("OK %d files\n", MANIFEST_COUNT);
    return 0;
}

static void remove_tree(const WCHAR *dir)
{
    WCHAR buf[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    size_t n = wcslen(dir);
    if (n == 0 || n >= MAX_PATH) return;
    memcpy(buf, dir, (n + 1) * sizeof(WCHAR));
    buf[n + 1] = 0;
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE; op.pFrom = buf;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

/* ---- prepare again on every release --------------------------------------------------------------------------
 * RecompLauncher runs the setup step again whenever a new release is installed. The game files are the same for every
 * release (the disc's files at title update 1.5, pinned in manifest.h), so a folder that already holds them is kept:
 *  - the stamp a complete run or install left (<dest_dir>\.halo2-prepared: the manifest's own SHA-256, and each
 *    file's size and last-write time then) with every file unchanged: done at once ("Already built");
 *  - no stamp (folders from 1.0.8 and before, or a stamp from another manifest): every file is read and checked
 *    against its pinned SHA-256, which is reading, not copying; all good: stamped and done;
 *  - anything missing, a different size or a different hash: the full import from the player's copy, as before.
 * The stamp is written only after the folder is complete, so a run cut short never looks finished. */
#define PREPARED_STAMP L".halo2-prepared"

static void manifest_id(char out[65])
{
    unsigned char h[32];
    Sha s;
    int i;
    out[0] = 0;
    if (!sha_begin(&s)) return;
    for (i = 0; i < MANIFEST_COUNT; i++) {
        BCryptHashData(s.h, (PUCHAR)MANIFEST[i].path, (ULONG)strlen(MANIFEST[i].path) + 1, 0);
        BCryptHashData(s.h, (PUCHAR)&MANIFEST[i].size, sizeof MANIFEST[i].size, 0);
        BCryptHashData(s.h, (PUCHAR)MANIFEST[i].sha256, 32, 0);
    }
    sha_end(&s, h);
    for (i = 0; i < 32; i++)
        sprintf_s(out + i * 2, 3, "%02x", h[i]);
}

static void entry_path(const WCHAR *dir, int i, WCHAR *out)
{
    WCHAR rel[MAX_PATH];
    rel_w(MANIFEST[i].path, rel);
    swprintf_s(out, MAX_PATH, L"%s\\%s", dir, rel);
}

/* The stamp: "H2PREP2 <manifest id>", then one line per manifest file, in order: its size and last-write time
 * (FILETIME) as they were when the folder was complete. A later check is one attribute lookup per file: a file
 * missing, of another size or written since (a mod, another program, an interrupted copy over it) fails it. Silent
 * disk damage that changes no write time is not seen; running Setup again (Repair) checks every hash. */
static int file_stat(const WCHAR *path, unsigned long long *size, unsigned long long *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    *size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    *mtime = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return 1;
}

static int stamp_matches(const WCHAR *dir, const char *id)
{
    WCHAR path[MAX_PATH];
    char line[160];
    FILE *f;
    int i, ok = 1;
    swprintf_s(path, MAX_PATH, L"%s\\%s", dir, PREPARED_STAMP);
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, "H2PREP2 ", 8) || strncmp(line + 8, id, 64)) ok = 0;
    for (i = 0; ok && i < MANIFEST_COUNT; i++) {
        unsigned long long want_size, want_time, size, time;
        entry_path(dir, i, path);
        if (!fgets(line, sizeof line, f) || sscanf_s(line, "%llu %llu", &want_size, &want_time) != 2 ||
            !file_stat(path, &size, &time) || size != want_size || size != MANIFEST[i].size || time != want_time)
            ok = 0;
    }
    fclose(f);
    return ok;
}

static void stamp_remove(const WCHAR *dir)
{
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\%s", dir, PREPARED_STAMP);
    DeleteFileW(path);
}

/* Written last, and only for a complete folder: a run cut short never looks finished. */
static void stamp_write(const WCHAR *dir, const char *id)
{
    WCHAR path[MAX_PATH], tmp[MAX_PATH];
    FILE *f;
    int i, ok = 1;
    swprintf_s(path, MAX_PATH, L"%s\\%s", dir, PREPARED_STAMP);
    swprintf_s(tmp, MAX_PATH, L"%s.tmp", path);
    if (_wfopen_s(&f, tmp, L"wb") || !f) return;
    fprintf(f, "H2PREP2 %.64s\n", id);
    for (i = 0; i < MANIFEST_COUNT; i++) {
        unsigned long long size, time;
        WCHAR file[MAX_PATH];
        entry_path(dir, i, file);
        if (!file_stat(file, &size, &time)) { ok = 0; break; }
        fprintf(f, "%llu %llu\n", size, time);
    }
    if (fclose(f)) ok = 0;
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
}

/* Every file there and matching its pinned size and SHA-256. */
static int folder_complete(const WCHAR *dir)
{
    int i;
    g_done = 0;
    g_total = 0;
    for (i = 0; i < MANIFEST_COUNT; i++)
        g_total += MANIFEST[i].size;
    g_progress_text = "Checking the game files you already have";
    for (i = 0; i < MANIFEST_COUNT; i++) {
        WCHAR path[MAX_PATH];
        HANDLE f;
        LARGE_INTEGER sz;
        int ok = 0;
        entry_path(dir, i, path);
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            if (GetFileSizeEx(f, &sz) && (unsigned long long)sz.QuadPart == MANIFEST[i].size)
                ok = copy_checked(f, MANIFEST[i].size, MANIFEST[i].sha256, NULL, 1) == 1;
            CloseHandle(f);
        }
        if (!ok) {
            g_progress_text = "Copying game files";
            g_done = g_total = 0;
            return 0;
        }
    }
    g_progress_text = "Copying game files";
    g_done = g_total = 0;
    return 1;
}

static int sdk_prepare_game(const WCHAR *source, const WCHAR *destination)
{
    WCHAR dest[MAX_PATH], stage[MAX_PATH], backup[MAX_PATH], parent[MAX_PATH], *sl;
    DWORD n = GetFullPathNameW(destination, MAX_PATH, dest, NULL), attr;
    char id[65];
    int r, had_old = 0;

    g_sdk = 1;
    while (n > 3 && (dest[n - 1] == L'\\' || dest[n - 1] == L'/')) dest[--n] = 0;
    if (!n || n >= MAX_PATH - 64) { printf("ERROR The game folder path is not usable.\n"); return 6; }
    swprintf_s(stage, MAX_PATH, L"%s.stage-%lu", dest, (unsigned long)GetCurrentProcessId());
    swprintf_s(backup, MAX_PATH, L"%s.old-%lu", dest, (unsigned long)GetCurrentProcessId());
    wcscpy_s(parent, MAX_PATH, dest);
    if ((sl = wcsrchr(parent, L'\\')) != NULL) { *sl = 0; SHCreateDirectoryExW(NULL, parent, NULL); }
    attr = GetFileAttributesW(dest);
    manifest_id(id);
    if (id[0] && attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (stamp_matches(dest, id)) {
            printf("progress %g Already built\n", g_sdk_span);
            fflush(stdout);
            return 0;
        }
        if (folder_complete(dest)) {
            stamp_write(dest, id);
            printf("progress %g Game files are ready (already in place)\n", g_sdk_span);
            fflush(stdout);
            return 0;
        }
    }
    puts("progress 0.02 Checking your Xbox game source");
    fflush(stdout);
    remove_tree(stage);
    r = run_import(source, stage, NULL, attr != INVALID_FILE_ATTRIBUTES ? dest : NULL);
    if (r) { remove_tree(stage); return r; }
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            printf("ERROR The game folder is not a plain folder.\n"); remove_tree(stage); return 6;
        }
        if (!MoveFileW(dest, backup)) { printf("ERROR Could not replace the old game files.\n"); remove_tree(stage); return 6; }
        had_old = 1;
    }
    if (!MoveFileW(stage, dest)) {
        printf("ERROR Could not put the game files in place.\n");
        if (had_old) MoveFileW(backup, dest);
        remove_tree(stage);
        return 6;
    }
    if (had_old) remove_tree(backup);
    if (id[0])
        stamp_write(dest, id);
    printf("progress %g Game files are ready\n", g_sdk_span);
    fflush(stdout);
    return 0;
}

/* The Map Pack disc after the game files: halo2_mappack.exe <disc> <install>\data [--videos <install>\extras], its
 * "PROGRESS done total" lines turned into this step's progress and its "ERROR ..." line passed on. */
static int sdk_mappack(const WCHAR *disc, const WCHAR *destination, int videos)
{
    WCHAR dest[MAX_PATH], root[MAX_PATH], exe[MAX_PATH], data[MAX_PATH], extras[MAX_PATH], *sl;
    WCHAR *cmd;
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 99, n;
    char line[512], c;
    size_t ln = 0, cn;
    const char *what = videos ? "Map Pack disc: maps and bonus videos" : "Map Pack disc: maps";

    GetFullPathNameW(destination, MAX_PATH, dest, NULL);
    n = (DWORD)wcslen(dest);
    while (n > 3 && (dest[n - 1] == L'\\' || dest[n - 1] == L'/')) dest[--n] = 0;
    wcscpy_s(root, MAX_PATH, dest);
    if ((sl = wcsrchr(root, L'\\')) != NULL) *sl = 0;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if ((sl = wcsrchr(exe, L'\\')) != NULL) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - exe), L"halo2_mappack.exe");
    swprintf_s(data, MAX_PATH, L"%s\\data", root);
    swprintf_s(extras, MAX_PATH, L"%s\\extras", root);
    cn = wcslen(exe) + wcslen(disc) + wcslen(data) + wcslen(extras) + 64;
    cmd = (WCHAR *)calloc(cn, sizeof(WCHAR));
    if (!cmd) return 6;
    if (videos) swprintf_s(cmd, cn, L"\"%s\" \"%s\" \"%s\" --videos \"%s\"", exe, disc, data, extras);
    else swprintf_s(cmd, cn, L"\"%s\" \"%s\" \"%s\"", exe, disc, data);
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { free(cmd); return 6; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    printf("progress %.4f %s\n", g_sdk_span, what);
    fflush(stdout);
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        printf("ERROR halo2_mappack.exe could not be started (error %lu).\n", GetLastError());
        CloseHandle(rd); CloseHandle(wr); free(cmd);
        return 6;
    }
    free(cmd);
    CloseHandle(wr);
    while (ReadFile(rd, &c, 1, &n, NULL) && n) {
        if (c == '\r') continue;
        if (c != '\n') { if (ln < sizeof line - 1) line[ln++] = c; continue; }
        line[ln] = 0;
        ln = 0;
        if (!strncmp(line, "PROGRESS ", 9)) {
            unsigned long long done = 0, total = 0;
            if (sscanf(line + 9, "%llu %llu", &done, &total) == 2 && total) {
                printf("progress %.4f %s\n", g_sdk_span + (1.0 - g_sdk_span) * (double)done / (double)total, what);
                fflush(stdout);
            }
        } else if (!strncmp(line, "ERROR ", 6)) {
            printf("%s\n", line);
            fflush(stdout);
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code == 0) {
        printf("progress 1 %s installed\n", videos ? "Map Pack maps and videos" : "Map Pack maps");
        fflush(stdout);
        return 0;
    }
    return code == 6 ? 6 : code == 5 ? 9 : 8;
}

static int sdk_prepare(const WCHAR *source, const WCHAR *destination, const WCHAR *mappack, int videos)
{
    int r;
    g_sdk_span = mappack && *mappack ? 0.6 : 1.0;
    r = sdk_prepare_game(source, destination);
    if (r || !mappack || !*mappack) return r;
    return sdk_mappack(mappack, destination, videos);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 4 && !wcscmp(argv[1], L"--prepare")) {
        const WCHAR *mappack = NULL;
        int i, videos = 0;
        for (i = 4; i < argc; i++) {
            if (!wcsncmp(argv[i], L"--mappack=", 10)) mappack = argv[i] + 10;
            else if (!wcscmp(argv[i], L"--mappack-videos")) videos = 1;
        }
        return sdk_prepare(argv[2], argv[3], mappack, videos);
    }
    if (argc == 3) return run_import(argv[1], argv[2], NULL, argv[2]);
    if (argc == 5 && !wcscmp(argv[3], L"--update")) return run_import(argv[1], argv[2], argv[4], argv[2]);
    printf("usage: halo2_import <iso or folder> <dest folder> [--update <title update folder>]\n"
           "       halo2_import --prepare <iso or folder> <dest folder> [--mappack=<disc>] [--mappack-videos]\n");
    return 2;
}
