/* halo2_mappack: installs the map packs (and, if asked, the bonus videos) from the player's own Halo 2 Multiplayer
 * Map Pack disc.
 *
 *   halo2_mappack <source> <data dir> [--videos <extras dir> [--langs all|<code>,<code>...]]
 *
 * <source> is the disc's image (.iso: a full Redump image or a plain XISO) or a folder holding the disc's files.
 * <data dir> is the install's data folder (SaveData in a development tree).
 *
 * Maps: the disc's three packs (Bonus, Killtacular, Maptacular; Blastacular was never on a disc) go where Halo 2 looks
 * for downloaded content, <data dir>\TDATA\4d530064\$c\4d5300640000000N\, exactly as the console's installer puts
 * them. Every file is checked against its pinned size and SHA-256 (mappack_manifest.h) as it is copied. Each pack's
 * ContentMeta.xbx then gets the "install complete" flag the console's installer sets and is signed for 16 zero
 * bytes; the game signs it again at start for whatever hard-drive key it runs with (h2_content.c).
 *
 * Videos (--videos): the disc's five videos and their preview pictures go to <extras dir> in the disc's own layout
 * (movies\..., videos\...), unchanged (MPEG-1 with CRI ADX audio; config.exe's EXTRAS page plays them and can
 * re-encode them to H.264). "Another Day at the Beach" is on the disc once per language: --langs picks which
 * (default: English and the Windows display language). A video already re-encoded (an .mp4 beside where its .sfd
 * would go) is not copied again.
 *
 * On success it writes <data dir>\mappack.txt (maps=1, videos=0/1), which Setup reads to skip its Map Pack page.
 *
 * Progress goes to stdout as "PROGRESS done total". Exit codes: 0 ok, 2 source not found, 3 not an Xbox disc image,
 * 4 not the Halo 2 Multiplayer Map Pack disc, 5 files missing or damaged, 6 cannot write. */
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mappack_manifest.h"

#define SECTOR 2048ull
static const unsigned long long XDVDFS_OFFSETS[] = { 0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull };

static unsigned long long g_done, g_total;
static HANDLE g_img = INVALID_HANDLE_VALUE;
static unsigned long long g_base;
static struct { unsigned long long sector, size; int found; } g_disc[MP_MANIFEST_COUNT];
static WCHAR g_src_dir[MAX_PATH];   /* a folder source: the folder holding demos\ */
static int g_want[MP_MANIFEST_COUNT];

static void progress(void)
{
    printf("PROGRESS %llu %llu\n", g_done, g_total);
    fflush(stdout);
}

static void rel_w(const char *rel, WCHAR *out)
{
    WCHAR *p;
    MultiByteToWideChar(CP_ACP, 0, rel, -1, out, MAX_PATH);
    for (p = out; *p; p++) if (*p == L'/') *p = L'\\';
}

static int make_dirs_for(const WCHAR *file)
{
    WCHAR dir[MAX_PATH], *sl;
    wcscpy_s(dir, MAX_PATH, file);
    if (!(sl = wcsrchr(dir, L'\\'))) return 1;
    *sl = 0;
    return SHCreateDirectoryExW(NULL, dir, NULL) == ERROR_SUCCESS || GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
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

/* Copy len bytes from `in` (positioned) to dst, or only hash them (dst NULL). The file is written under a .part name
 * and renamed once its hash is right. 1 ok, 0 the hash or size is wrong, -1 cannot write. */
static int copy_checked(HANDLE in, unsigned long long len, const unsigned char want[32], const WCHAR *dst, int count)
{
    static unsigned char buf[1 << 20];
    unsigned char got[32];
    WCHAR part[MAX_PATH];
    HANDLE out = INVALID_HANDLE_VALUE;
    unsigned long long left = len;
    DWORD n, w;
    Sha s;
    if (!sha_begin(&s)) return -1;
    if (dst) {
        swprintf_s(part, MAX_PATH, L"%s.part", dst);
        if (!make_dirs_for(dst)) { sha_end(&s, got); return -1; }
        out = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) { sha_end(&s, got); return -1; }
    }
    while (left) {
        DWORD want_n = left > sizeof buf ? (DWORD)sizeof buf : (DWORD)left;
        if (!ReadFile(in, buf, want_n, &n, NULL) || n == 0) break;
        BCryptHashData(s.h, buf, n, 0);
        if (out != INVALID_HANDLE_VALUE && (!WriteFile(out, buf, n, &w, NULL) || w != n)) {
            CloseHandle(out); DeleteFileW(part); sha_end(&s, got); return -1;
        }
        left -= n;
        if (count) { g_done += n; progress(); }
    }
    sha_end(&s, got);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (left || memcmp(got, want, 32)) { if (dst) DeleteFileW(part); return 0; }
    if (dst && !MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(part); return -1; }
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

/* ---- disc image (XDVDFS) -------------------------------------------------------------------------------------- */
static int img_read(unsigned long long off, void *buf, DWORD n)
{
    LARGE_INTEGER p;
    DWORD got;
    p.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(g_img, p, NULL, FILE_BEGIN) && ReadFile(g_img, buf, n, &got, NULL) && got == n;
}

static int manifest_find(const char *rel)
{
    int i;
    for (i = 0; i < MP_MANIFEST_COUNT; i++)
        if (_stricmp(MP_MANIFEST[i].path, rel) == 0) return i;
    return -1;
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
            } else {
                int mi = manifest_find(rel);
                if (mi >= 0) { g_disc[mi].sector = sector; g_disc[mi].size = size; g_disc[mi].found = 1; }
            }
            continue;
        }
        sp--;
        if (right && sp < 511) { st[sp].pos = right * 4; st[sp].state = 0; sp++; }
    }
    free(d);
    return 1;
}

/* Opens the source: 0 ok, else the exit code. */
static int open_source(const WCHAR *source)
{
    DWORD a = GetFileAttributesW(source);
    unsigned char v[28];
    size_t i;
    int k;
    if (a == INVALID_FILE_ATTRIBUTES) { printf("ERROR The Map Pack disc image or folder was not found.\n"); return 2; }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        WCHAR t[MAX_PATH];
        /* the folder itself or one of its children holds demos\dlc */
        swprintf_s(t, MAX_PATH, L"%s\\demos\\dlc", source);
        if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) wcscpy_s(g_src_dir, MAX_PATH, source);
        else {
            WIN32_FIND_DATAW fd;
            HANDLE h;
            swprintf_s(t, MAX_PATH, L"%s\\*", source);
            if ((h = FindFirstFileW(t, &fd)) != INVALID_HANDLE_VALUE) {
                do {
                    WCHAR c[MAX_PATH];
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                    swprintf_s(c, MAX_PATH, L"%s\\%s\\demos\\dlc", source, fd.cFileName);
                    if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) {
                        swprintf_s(g_src_dir, MAX_PATH, L"%s\\%s", source, fd.cFileName);
                        break;
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (!g_src_dir[0]) { printf("ERROR This folder is not the Halo 2 Multiplayer Map Pack disc.\n"); return 4; }
        for (k = 0; k < MP_MANIFEST_COUNT; k++) {
            WCHAR rel[MAX_PATH], p[MAX_PATH];
            WIN32_FILE_ATTRIBUTE_DATA fa;
            rel_w(MP_MANIFEST[k].path, rel);
            swprintf_s(p, MAX_PATH, L"%s\\%s", g_src_dir, rel);
            if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && !(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                g_disc[k].found = 1;
                g_disc[k].size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            }
        }
        return 0;
    }
    g_img = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (g_img == INVALID_HANDLE_VALUE) { printf("ERROR The disc image could not be opened.\n"); return 2; }
    for (i = 0; i < sizeof XDVDFS_OFFSETS / sizeof *XDVDFS_OFFSETS; i++)
        if (img_read(XDVDFS_OFFSETS[i] + 32 * SECTOR, v, 28) && memcmp(v, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
            g_base = XDVDFS_OFFSETS[i];
            if (!walk(v[20] | (v[21] << 8) | (v[22] << 16) | ((unsigned)v[23] << 24),
                      v[24] | (v[25] << 8) | (v[26] << 16) | ((unsigned)v[27] << 24), "", 0)) {
                printf("ERROR The disc image is damaged or incomplete.\n");
                return 5;
            }
            return 0;
        }
    printf("ERROR This is not an Xbox disc image.\n");
    return 3;
}

/* Copies entry k to dst, checked. 1 ok, 0 missing or wrong, -1 cannot write. */
static int copy_entry(int k, const WCHAR *dst)
{
    int r = 0;
    if (!g_disc[k].found || g_disc[k].size != MP_MANIFEST[k].size) return 0;
    if (g_img != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER p;
        p.QuadPart = (LONGLONG)(g_base + g_disc[k].sector * SECTOR);
        if (!SetFilePointerEx(g_img, p, NULL, FILE_BEGIN)) return 0;
        return copy_checked(g_img, MP_MANIFEST[k].size, MP_MANIFEST[k].sha256, dst, 1);
    } else {
        WCHAR rel[MAX_PATH], src[MAX_PATH];
        HANDLE in;
        rel_w(MP_MANIFEST[k].path, rel);
        swprintf_s(src, MAX_PATH, L"%s\\%s", g_src_dir, rel);
        in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (in == INVALID_HANDLE_VALUE) return 0;
        r = copy_checked(in, MP_MANIFEST[k].size, MP_MANIFEST[k].sha256, dst, 1);
        CloseHandle(in);
    }
    return r;
}

/* ---- ContentMeta.xbx: the install-complete flag and the signature --------------------------------------------- */
static int hmac_sha1(const unsigned char *key, ULONG kn, const unsigned char *d, ULONG n, unsigned char out[20])
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

/* XAPI's content enumeration skips a package whose header flags (+0x20) lack bit 0, the bit the console's
 * installer sets when it finishes; packs 1-3 have it clear on the disc. Then the digest (the first 20 bytes):
 * HMAC-SHA1 over the header from +0x14, keyed with HMAC-SHA1(hard-drive key, title id). */
static int finish_meta(const WCHAR *path)
{
    static const unsigned char zero_key[16];
    unsigned char b[4096], key[20];
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0, w;
    unsigned hdr, tid, flags;
    int ok = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (ReadFile(f, b, sizeof b, &n, NULL) && n >= 0x28 && !memcmp(b + 20, "XCSF", 4)) {
        memcpy(&hdr, b + 0x18, 4);
        memcpy(&tid, b + 0x24, 4);
        memcpy(&flags, b + 0x20, 4);
        flags |= 1;
        memcpy(b + 0x20, &flags, 4);
        if (hdr > 0x14 && hdr <= n && hmac_sha1(zero_key, 16, (unsigned char *)&tid, 4, key) &&
            hmac_sha1(key, 20, b + 0x14, hdr - 0x14, b)) {
            LARGE_INTEGER z;
            z.QuadPart = 0;
            ok = SetFilePointerEx(f, z, NULL, FILE_BEGIN) && WriteFile(f, b, n, &w, NULL) && w == n;
        }
    }
    CloseHandle(f);
    return ok;
}

/* ---- languages of "Another Day at the Beach" --------------------------------------------------------------------- */
static const char *ui_lang_code(void)
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_GERMAN: return "de";
    case LANG_FRENCH: return "fr";
    case LANG_ITALIAN: return "it";
    case LANG_SPANISH: return "sp";
    case LANG_JAPANESE: return "jp";
    case LANG_KOREAN: return "ko";
    case LANG_CHINESE: return "ch";
    default: return "en";
    }
}

/* movies/movie1/<lang>/... is wanted when <lang> is in the list ("all": every one) */
static int lang_wanted(const char *path, const char *langs)
{
    char code[3], list[64], *t, *ctx = NULL;
    if (_strnicmp(path, "movies/movie1/", 14) != 0) return 1;
    if (!_stricmp(langs, "all")) return 1;
    code[0] = path[14]; code[1] = path[15]; code[2] = 0;
    strncpy_s(list, sizeof list, langs, _TRUNCATE);
    for (t = strtok_s(list, ",", &ctx); t; t = strtok_s(NULL, ",", &ctx))
        if (!_stricmp(t, code)) return 1;
    return 0;
}

/* where entry k goes; maps: <data>\TDATA\4d530064\$c\4d530064000000NN\<file>; videos: <extras>\<disc path> */
static void dest_of(int k, const WCHAR *data, const WCHAR *extras, WCHAR *out)
{
    const char *p = MP_MANIFEST[k].path;
    WCHAR rel[MAX_PATH];
    if (MP_MANIFEST[k].group == 0) {
        /* demos/dlc/0000000N/<file> */
        WCHAR file[MAX_PATH];
        rel_w(p + 19, file);
        swprintf_s(out, MAX_PATH, L"%s\\TDATA\\4d530064\\$c\\4d530064%c%c%c%c%c%c%c%c\\%s", data,
                   p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17], file);
    } else {
        rel_w(p, rel);
        swprintf_s(out, MAX_PATH, L"%s\\%s", extras, rel);
    }
}

/* a video re-encoded by config.exe: <name>.mp4 beside <name>.sfd (movie2's parts: movies\movie2\killtacular.mp4) */
static int reencoded(const WCHAR *sfd)
{
    WCHAR t[MAX_PATH], *dot;
    wcscpy_s(t, MAX_PATH, sfd);
    if (wcsstr(t, L"\\movie2\\kill_part")) {
        WCHAR *sl = wcsrchr(t, L'\\');
        wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - t), L"killtacular.mp4");
    } else if ((dot = wcsrchr(t, L'.')) != NULL && !_wcsicmp(dot, L".sfd"))
        wcscpy_s(dot, MAX_PATH - (dot - t), L".mp4");
    else
        return 0;
    return GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES;
}

/* <data dir>\mappack.txt: the Map Pack disc was installed here (Setup does not ask for it again on an update or a
 * repair). Written after every successful run; "videos=1" once a run copied the videos too. */
static void write_marker(const WCHAR *data, int videos)
{
    WCHAR path[MAX_PATH];
    char old[256] = "";
    SYSTEMTIME t;
    FILE *f;
    swprintf_s(path, MAX_PATH, L"%s\\mappack.txt", data);
    if ((f = _wfopen(path, L"rb")) != NULL) {   /* videos copied by an earlier run stay noted */
        fread(old, 1, sizeof old - 1, f);
        fclose(f);
        if (strstr(old, "videos=1")) videos = 1;
    }
    GetLocalTime(&t);
    if ((f = _wfopen(path, L"wb")) == NULL) return;
    fprintf(f, "Halo 2 Multiplayer Map Pack disc installed by halo2_mappack\r\n"
               "maps=1\r\nvideos=%d\r\ndate=%04d-%02d-%02d\r\n", videos, t.wYear, t.wMonth, t.wDay);
    fclose(f);
}

int wmain(int argc, wchar_t **argv)
{
    const WCHAR *source, *data, *extras = NULL;
    char langs[64];
    int i, k, r, missing = 0;
    if (argc < 3) {
        printf("usage: halo2_mappack <iso or folder> <data dir> [--videos <extras dir> [--langs all|en,fr,...]]\n");
        return 2;
    }
    source = argv[1];
    data = argv[2];
    snprintf(langs, sizeof langs, "en,%s", ui_lang_code());
    for (i = 3; i < argc; i++) {
        if (!wcscmp(argv[i], L"--videos") && i + 1 < argc) extras = argv[++i];
        else if (!wcscmp(argv[i], L"--langs") && i + 1 < argc)
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, langs, sizeof langs, NULL, NULL);
    }
    if ((r = open_source(source)) != 0) return r;
    /* the right disc: every map pack file there at its pinned size */
    for (k = 0; k < MP_MANIFEST_COUNT; k++)
        if (MP_MANIFEST[k].group == 0 && (!g_disc[k].found || g_disc[k].size != MP_MANIFEST[k].size)) {
            printf("ERROR This is not the Halo 2 Multiplayer Map Pack disc, or the image is incomplete.\n");
            return 4;
        }
    for (k = 0; k < MP_MANIFEST_COUNT; k++) {
        WCHAR dst[MAX_PATH];
        if (MP_MANIFEST[k].group == 1 && (!extras || !lang_wanted(MP_MANIFEST[k].path, langs))) continue;
        dest_of(k, data, extras, dst);
        if (MP_MANIFEST[k].group == 1 && reencoded(dst)) continue;
        /* A copy already there and right is kept. ContentMeta.xbx changes once installed, and a pack downloaded
         * from Xbox Live (Insignia) has its own, newer one: one already there is never replaced. */
        if (strstr(MP_MANIFEST[k].path, "ContentMeta") ? GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES
                                                       : file_matches(dst, MP_MANIFEST[k].size, MP_MANIFEST[k].sha256))
            continue;
        g_want[k] = 1;
        g_total += MP_MANIFEST[k].size;
    }
    progress();
    for (k = 0; k < MP_MANIFEST_COUNT; k++) {
        WCHAR dst[MAX_PATH];
        if (!g_want[k]) continue;
        dest_of(k, data, extras, dst);
        r = copy_entry(k, dst);
        if (r < 0) { printf("ERROR Could not write to the install folder.\n"); return 6; }
        if (!r) { if (missing++ < 5) printf("DAMAGED %s\n", MP_MANIFEST[k].path); continue; }
        if (strstr(MP_MANIFEST[k].path, "ContentMeta") && !finish_meta(dst)) {
            printf("ERROR Could not finish %s.\n", MP_MANIFEST[k].path);
            return 6;
        }
    }
    if (g_img != INVALID_HANDLE_VALUE) CloseHandle(g_img);
    if (missing) { printf("ERROR %d files are missing or damaged (bad or incomplete dump).\n", missing); return 5; }
    g_done = g_total;
    progress();
    write_marker(data, extras != NULL);
    printf("OK map packs installed%s\n", extras ? ", videos copied" : "");
    return 0;
}
