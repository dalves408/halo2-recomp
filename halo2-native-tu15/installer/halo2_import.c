/* halo2_import: copies the game's files out of the player's own copy of the disc and title update 1.5
 * (after Marvel vs. Capcom 2's mvc2_import).
 *
 *   halo2_import <source> <dest_dir> [--update <title update>]
 *   halo2_import --prepare <source> <dest_dir>       (RecompLauncher setup: see below)
 *
 * <source> is an Xbox disc image (.iso: a full Redump image or a plain XISO) or a folder holding the disc's files
 * (the one with default.xbe). A folder that already has title update 1.5 applied is a complete source on its own.
 * <title update> is the update's folder from the console's drive (E:\TDATA\4D530064\$u, with default.xbe and
 * downloader.xbe) or its default.xbe. Without --update it is also looked for beside the disc image or folder (a $u
 * folder, a TitleUpdate folder, or the .xbe files themselves), in the install's data\TitleUpdate (where RecompLauncher
 * puts the update it asks for) and in <dest_dir> (a repair keeps it).
 *
 * Every file in manifest.h is written to <dest_dir> and checked against its pinned size and SHA-256 as it is copied;
 * nothing else is. Progress goes to stdout as "PROGRESS done total".
 *
 * --prepare is the RecompLauncher "prepare" step: no window, progress as "progress <0..1> <text>" lines, exit 0 only
 * when <dest_dir> holds a complete, verified copy. The files go to a staging folder beside <dest_dir> that is swapped
 * in at the end, so a failed run never leaves a half-written game folder (an existing one is kept until then).
 *
 * Exit codes: 0 ok, 2 source not found, 3 not an Xbox disc image, 4 not the supported Halo 2 disc, 5 files missing
 * or damaged, 6 cannot write, 7 title update 1.5 not found or not the right one. */
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

static void progress(void)
{
    if (g_sdk)
        printf("progress %.4f Copying game files\n", 0.05 + 0.9 * (double)g_done / (double)(g_total ? g_total : 1));
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

static int run_import(const WCHAR *source, const WCHAR *destination, const WCHAR *update, const WCHAR *existing)
{
    DWORD attr;
    int i, r;
    size_t n;

    g_done = 0; g_total = MANIFEST_BYTES; g_base = 0; g_disc_xbe_found = 0;
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
    r = (attr & FILE_ATTRIBUTE_DIRECTORY) ? import_folder(source) : import_image(source);
    if (r) return r;
    r = find_update(source, update, existing);
    if (r < 0) { printf("ERROR Could not write to the install folder.\n"); return 6; }
    if (!r) {
        for (i = 0; i < MANIFEST_COUNT; i++)
            if (MANIFEST[i].from == 1 && !g_have[i]) printf("MISSING %s (title update 1.5)\n", MANIFEST[i].path);
        printf("ERROR Title update 1.5 was not found, or it is not the right one: select the update's folder from "
               "your console's drive (E:\\TDATA\\4D530064\\$u).\n");
        return 7;
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

static int sdk_prepare(const WCHAR *source, const WCHAR *destination)
{
    WCHAR dest[MAX_PATH], stage[MAX_PATH], backup[MAX_PATH], parent[MAX_PATH], *sl;
    DWORD n = GetFullPathNameW(destination, MAX_PATH, dest, NULL), attr;
    int r, had_old = 0;

    g_sdk = 1;
    while (n > 3 && (dest[n - 1] == L'\\' || dest[n - 1] == L'/')) dest[--n] = 0;
    if (!n || n >= MAX_PATH - 64) { printf("ERROR The game folder path is not usable.\n"); return 6; }
    swprintf_s(stage, MAX_PATH, L"%s.stage-%lu", dest, (unsigned long)GetCurrentProcessId());
    swprintf_s(backup, MAX_PATH, L"%s.old-%lu", dest, (unsigned long)GetCurrentProcessId());
    wcscpy_s(parent, MAX_PATH, dest);
    if ((sl = wcsrchr(parent, L'\\')) != NULL) { *sl = 0; SHCreateDirectoryExW(NULL, parent, NULL); }
    puts("progress 0.02 Checking your Xbox game source");
    fflush(stdout);
    remove_tree(stage);
    attr = GetFileAttributesW(dest);
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
    puts("progress 1 Game files are ready");
    fflush(stdout);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 4 && !wcscmp(argv[1], L"--prepare")) return sdk_prepare(argv[2], argv[3]);
    if (argc == 3) return run_import(argv[1], argv[2], NULL, argv[2]);
    if (argc == 5 && !wcscmp(argv[3], L"--update")) return run_import(argv[1], argv[2], argv[4], argv[2]);
    printf("usage: halo2_import <iso or folder> <dest folder> [--update <title update folder>]\n"
           "       halo2_import --prepare <iso or folder> <dest folder>\n");
    return 2;
}
