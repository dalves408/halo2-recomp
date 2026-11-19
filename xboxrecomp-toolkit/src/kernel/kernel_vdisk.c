/*
 * kernel_vdisk.c -- the hard disk's raw devices, emulated in memory (Win32).
 *
 * \Device\Harddisk0\Partition0 is the whole disk and Partition1-5 are the
 * partitions behind E:, C:, X:, Y: and Z:. A title opens them as devices: to
 * read the partition table (XAPI's utility-drive mount), to format and check
 * a cache partition, and the online stack to keep the console's network
 * settings and Xbox Live accounts in the config area of the disk (Partition0,
 * sectors 8-23). Files on those partitions do not go through here at all: the
 * path rules put them in plain host folders (UDATA, TDATA, Cache, ...).
 *
 * These used to be PartitionN.img files in the save folder, most of them
 * 750 MB sparse files nothing but a format header ever touched. Now each
 * device is a sparse set of 4 KB pages in memory, read as zeroes until
 * written, sized from the same retail geometry as the partition table. The
 * table itself is built in. The only part that outlives the process is the
 * config area, sectors 8-23 (8 KB): it is kept in one small file, by default
 * RECOMP_DISK_CONFIG, else <save dir>\DiskConfig.bin, and written back
 * whenever the title changes it. A cache partition starts unformatted each
 * run, which is what a title expects of a cache.
 *
 * A device handle is a host event object (signalled, so a wait on it returns
 * at once, as for a completed synchronous transfer), registered here; the
 * NtXxxFile calls in kernel_file.c ask xbox_vdisk_part() first.
 *
 * Enabled for Halo 2 1.5 by xbox_path_set_in_memory_partitions(1) before
 * xbox_path_init. The flag defaults off, so a 1.0 build keeps PartitionN.img.
 *
 * Existing PartitionN.img files are migrated once: the config sectors are
 * copied out of Partition0.img and the images are deleted.
 */
#if defined(_WIN32)

#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VD_PAGE        4096u
#define VD_CHUNK_PAGES 1024u
#define VD_CFG_OFFSET  0x1000ull          /* sector 8  */
/* Sectors 8-23: network settings (8), machine account (9), the user account
 * directory (11) and the eight user account slots (12-19), with room to
 * spare. A file kept by an earlier version (sectors 8-15 only) still loads:
 * the sectors it lacks read as empty. */
#define VD_CFG_SIZE    0x2000u
#define VD_MAX_HANDLES 64

/* Sizes in 512-byte sectors, as in the partition table (kernel_path.c). */
static const uint64_t s_sectors[6] = {
    0x009C4800ull, /* 0: whole disk: up to the end of E: */
    0x00465400ull, /* 1: E: */
    0x000FA000ull, /* 2: C: */
    0x00177000ull, /* 3: X: */
    0x00177000ull, /* 4: Y: */
    0x00177000ull, /* 5: Z: */
};

typedef struct {
    uint8_t ***chunks;      /* [chunk][page] -> 4 KB, NULL = zeroes */
    uint32_t nchunks;
} VDISK;

typedef struct {
    HANDLE   h;
    int      part;
    uint64_t pos;
} VD_HANDLE;

static VDISK            s_disk[6];
static VD_HANDLE        s_handles[VD_MAX_HANDLES];
static CRITICAL_SECTION s_cs;
static volatile LONG    s_ready;
static WCHAR            s_cfg_path[MAX_PATH];
static WCHAR            s_save_dir[MAX_PATH];

const wchar_t *xbox_vdisk_save_dir(void) { return s_save_dir; }
static uint8_t          s_cfg_saved[VD_CFG_SIZE];

static void vd_lock(void)
{
    if (InterlockedCompareExchange(&s_ready, 1, 0) == 0) {
        InitializeCriticalSection(&s_cs);
        InterlockedExchange(&s_ready, 2);
    }
    while (s_ready != 2)
        Sleep(0);
    EnterCriticalSection(&s_cs);
}

static void vd_unlock(void) { LeaveCriticalSection(&s_cs); }

uint64_t xbox_vdisk_size(int part)
{
    return (part >= 0 && part <= 5) ? s_sectors[part] * 512ull : 0;
}

/* The page holding byte `off`, allocated (zeroed) when `make`. */
static uint8_t *vd_page(int part, uint64_t off, int make)
{
    VDISK *d = &s_disk[part];
    uint64_t page = off / VD_PAGE;
    uint32_t c = (uint32_t)(page / VD_CHUNK_PAGES), i = (uint32_t)(page % VD_CHUNK_PAGES);
    if (!d->chunks) {
        if (!make) return NULL;
        d->nchunks = (uint32_t)((xbox_vdisk_size(part) / VD_PAGE + VD_CHUNK_PAGES - 1) / VD_CHUNK_PAGES);
        d->chunks = (uint8_t ***)calloc(d->nchunks, sizeof *d->chunks);
        if (!d->chunks) return NULL;
    }
    if (c >= d->nchunks) return NULL;
    if (!d->chunks[c]) {
        if (!make) return NULL;
        d->chunks[c] = (uint8_t **)calloc(VD_CHUNK_PAGES, sizeof **d->chunks);
        if (!d->chunks[c]) return NULL;
    }
    if (!d->chunks[c][i] && make)
        d->chunks[c][i] = (uint8_t *)calloc(1, VD_PAGE);
    return d->chunks[c][i];
}

static void vd_copy_in(int part, uint64_t off, const uint8_t *src, uint32_t n)
{
    while (n) {
        uint32_t in = (uint32_t)(off % VD_PAGE), k = VD_PAGE - in;
        uint8_t *p = vd_page(part, off, 1);
        if (k > n) k = n;
        if (p) memcpy(p + in, src, k);
        off += k; src += k; n -= k;
    }
}

static void vd_copy_out(int part, uint64_t off, uint8_t *dst, uint32_t n)
{
    while (n) {
        uint32_t in = (uint32_t)(off % VD_PAGE), k = VD_PAGE - in;
        const uint8_t *p = vd_page(part, off, 0);
        if (k > n) k = n;
        if (p) memcpy(dst, p + in, k); else memset(dst, 0, k);
        off += k; dst += k; n -= k;
    }
}

/* The config area back to its file, when it changed. */
static void vd_save_config(void)
{
    uint8_t now[VD_CFG_SIZE];
    HANDLE f;
    DWORD w = 0;
    if (!s_cfg_path[0]) return;
    vd_copy_out(0, VD_CFG_OFFSET, now, VD_CFG_SIZE);
    if (!memcmp(now, s_cfg_saved, VD_CFG_SIZE)) return;
    f = CreateFileW(s_cfg_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "  [DISK] could not save the drive's config sectors (err=%lu)\n", GetLastError());
        return;
    }
    if (WriteFile(f, now, VD_CFG_SIZE, &w, NULL) && w == VD_CFG_SIZE)
        memcpy(s_cfg_saved, now, VD_CFG_SIZE);
    CloseHandle(f);
}

static int vd_read_file(const WCHAR *path, uint64_t off, uint8_t *buf, uint32_t n)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER li;
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) return -1;
    li.QuadPart = (LONGLONG)off;
    memset(buf, 0, n);
    if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN) || !ReadFile(f, buf, n, &got, NULL)) got = 0;
    CloseHandle(f);
    return (int)got;
}


/* Halo 2's XAPI formats Z:/Y: if it does not see a FATX volume, then
 * reboots the title through HalReturnToFirmware. A cache that starts
 * unformatted therefore exits on the first run. Seed the same empty
 * FATX layout the previous Partition5.img files carried (0x17000 bytes:
 * header, one reserved FAT entry, a zero gap, then 0xFF free entries).
 * Host files (fonts, preferences, cache maps) still live under SaveData. */
/* Remove everything under dir (files and subfolders), keeping dir itself.
 * Returns the number of files removed. */
static int vd_clear_dir(const WCHAR *dir)
{
    WIN32_FIND_DATAW fd;
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    HANDLE f;
    int n = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        swprintf_s(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n += vd_clear_dir(sub);
            RemoveDirectoryW(sub);
        } else {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(sub, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(sub))
                n++;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

/* A title formatting a cache partition (3-5) writes a new FATX header at its
 * start, then dismounts it: on a console the volume comes back empty. The
 * partition's files live in <save>\Cache here, which the in-memory header
 * does not touch, so a title that reformats (Halo 2 after its title cache
 * says the volume is stale) then fails to create files that "already
 * exist" (fonts\font_table.txt) and shows the disc-error screen. Empty the
 * folder when the header is written, as the format does on a console.
 * RECOMP_CACHE_FORMAT_CLEARS=0 keeps the old behaviour. */
static void vd_cache_formatted(int part)
{
    static int on = -1;
    WCHAR dir[MAX_PATH];
    int n;
    if (on < 0) {
        const char *e = getenv("RECOMP_CACHE_FORMAT_CLEARS");
        on = !(e && e[0] == '0');
    }
    if (!on || !s_save_dir[0])
        return;
    /* Partitions 3-5 share <save>\Cache (kernel_path.c rules), so only the
     * Z: volume's format (Partition5: fonts, preferences) empties it; Halo 2
     * reformats its second utility drive (Partition4) on every boot. */
    if (part != 5) {
        fprintf(stderr, "  [DISK] Partition%d formatted by the title (shared cache folder kept)\n", part);
        return;
    }
    swprintf_s(dir, MAX_PATH, L"%s\\Cache", s_save_dir);
    n = vd_clear_dir(dir);
    CreateDirectoryW(dir, NULL);
    fprintf(stderr, "  [DISK] Partition%d formatted by the title: emptied its cache folder"
                    " (%d file(s) removed)\n", part, n);
}

/* The FATX header and FAT of a cache partition (the span vd_seed_cache_fatx
 * writes) persist in <save>\CacheHeaderN.bin, as the config sectors do in
 * DiskConfig.bin. A title records its cache volume (Halo 2 in its title
 * cache); re-seeding a generic header every launch made that record stale,
 * so the title reformatted its cache on every boot. */
#define VD_CACHE_HDR 0x17000u

/* Sector 4 (0x800) starts as the partition table, and XAPI's utility-drive
 * mount writes its cache record there (which title owns which cache partition
 * and when it was formatted). Rebuilt every launch, the record was lost, so
 * XAPI found no record and reformatted its cache on every boot. Once a title
 * writes the sector it is kept in <save>\CacheDB.bin and loaded at start-up. */
#define VD_CACHEDB_OFFSET 0x800u
#define VD_CACHEDB_SIZE   0x200u

static void vd_cachedb_path(WCHAR *out)
{
    swprintf_s(out, MAX_PATH, L"%s\\CacheDB.bin", s_save_dir);
}

static void vd_save_cachedb(void)
{
    WCHAR path[MAX_PATH];
    uint8_t b[VD_CACHEDB_SIZE];
    HANDLE f;
    DWORD w = 0;
    if (!s_save_dir[0]) return;
    vd_copy_out(0, VD_CACHEDB_OFFSET, b, VD_CACHEDB_SIZE);
    vd_cachedb_path(path);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, b, VD_CACHEDB_SIZE, &w, NULL);
        CloseHandle(f);
    }
}

static void vd_cache_hdr_path(int part, WCHAR *out)
{
    swprintf_s(out, MAX_PATH, L"%s\\CacheHeader%d.bin", s_save_dir, part);
}

static void vd_save_cache_hdr(int part)
{
    WCHAR path[MAX_PATH];
    uint8_t *b = (uint8_t *)malloc(VD_CACHE_HDR);
    HANDLE f;
    DWORD w = 0;
    if (!b || !s_save_dir[0]) { free(b); return; }
    vd_copy_out(part, 0, b, VD_CACHE_HDR);
    vd_cache_hdr_path(part, path);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, b, VD_CACHE_HDR, &w, NULL);
        CloseHandle(f);
    }
    free(b);
}

/* 1 when a saved header was loaded into the partition. */
static int vd_load_cache_hdr(int part)
{
    WCHAR path[MAX_PATH];
    uint8_t *b = (uint8_t *)malloc(VD_CACHE_HDR);
    int ok = 0;
    if (!b) return 0;
    vd_cache_hdr_path(part, path);
    if (vd_read_file(path, 0, b, VD_CACHE_HDR) == (int)VD_CACHE_HDR && !memcmp(b, "FATX", 4)) {
        vd_copy_in(part, 0, b, VD_CACHE_HDR);
        ok = 1;
    }
    free(b);
    return ok;
}

static void vd_seed_cache_fatx(int part)
{
    const uint32_t n = 0x17000u;
    uint8_t *b = (uint8_t *)malloc(n);
    if (!b)
        return;
    memset(b, 0xFF, n);
    memcpy(b, "FATX", 4);
    b[4] = 0x90; b[5] = 0xD0; b[6] = 0x50; b[7] = 0xA0;
    b[8] = 0x80; b[9] = b[10] = b[11] = 0;
    b[12] = 1;   b[13] = b[14] = b[15] = 0;
    b[16] = b[17] = 0;
    memset(b + 0x1004, 0, 0x7000 - 0x1004);
    b[0x1000] = 0xF8;
    vd_copy_in(part, 0, b, n);
    free(b);
}

void xbox_vdisk_init(const WCHAR *save_dir)
{
    uint8_t table[512], cfg[VD_CFG_SIZE];
    WCHAR img[MAX_PATH];
    const char *ov = getenv("RECOMP_DISK_CONFIG");
    int p, have, removed = 0;

    vd_lock();
    wcscpy_s(s_save_dir, MAX_PATH, save_dir);
    if (ov && ov[0])
        MultiByteToWideChar(CP_UTF8, 0, ov, -1, s_cfg_path, MAX_PATH);
    else
        swprintf_s(s_cfg_path, MAX_PATH, L"%s\\DiskConfig.bin", save_dir);

    xbox_build_partition_table(table);
    {   /* the cache partitions' own folders (kernel_path.c rules) */
        static const WCHAR *const dirs[] = { L"Cache" };
        WCHAR d[MAX_PATH];
        for (p = 0; p < 1; p++) {
            swprintf_s(d, MAX_PATH, L"%s\\%s", save_dir, dirs[p]);
            CreateDirectoryW(d, NULL);
        }
    }
    {
        WCHAR dbp[MAX_PATH];
        uint8_t db[VD_CACHEDB_SIZE];
        vd_cachedb_path(dbp);
        if (vd_read_file(dbp, 0, db, VD_CACHEDB_SIZE) == (int)VD_CACHEDB_SIZE) {
            memcpy(table, db, sizeof table);
            fprintf(stderr, "  [DISK] kept the saved cache record (sector 4)\n");
        }
    }
    vd_copy_in(0, 0x800, table, sizeof table);

    have = vd_read_file(s_cfg_path, 0, cfg, VD_CFG_SIZE) >= 0;
    /* One-time move from the old image: its config sectors, then the images. */
    swprintf_s(img, MAX_PATH, L"%s\\Partition0.img", save_dir);
    if (!have && vd_read_file(img, VD_CFG_OFFSET, cfg, VD_CFG_SIZE) > 0) {
        HANDLE f = CreateFileW(s_cfg_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD w = 0;
        if (f != INVALID_HANDLE_VALUE) {
            have = WriteFile(f, cfg, VD_CFG_SIZE, &w, NULL) && w == VD_CFG_SIZE;
            CloseHandle(f);
        }
        if (have)
            fprintf(stderr, "  [DISK] moved the drive's config sectors out of Partition0.img\n");
        else
            fprintf(stderr, "  [DISK] could not move the config sectors out of Partition0.img; keeping the images\n");
    } else if (!have) {
        memset(cfg, 0, sizeof cfg);
        have = 1;      /* nothing to keep: a new drive */
    }
    if (have) {
        for (p = 0; p <= 5; p++) {
            swprintf_s(img, MAX_PATH, L"%s\\Partition%d.img", save_dir, p);
            if (DeleteFileW(img)) removed++;
        }
        if (removed)
            fprintf(stderr, "  [DISK] removed %d old partition image(s) from the save folder\n", removed);
    }
    vd_copy_in(0, VD_CFG_OFFSET, cfg, VD_CFG_SIZE);
    memcpy(s_cfg_saved, cfg, VD_CFG_SIZE);
    {
        int kept = 0;
        for (p = 3; p <= 5; p++) {
            if (vd_load_cache_hdr(p))
                kept |= 1 << p;
            else if (p >= 4)
                vd_seed_cache_fatx(p);
        }
        if (kept)
            fprintf(stderr, "  [DISK] cache partitions: kept the saved FATX header of%s%s%s\n",
                    kept & 8 ? " 3" : "", kept & 16 ? " 4" : "", kept & 32 ? " 5" : "");
        if ((kept & 48) != 48)
            fprintf(stderr, "  [DISK] seeded an empty FATX on the other cache partitions (4/5)\n");
    }
    vd_unlock();
}

HANDLE xbox_vdisk_open(int part)
{
    HANDLE h;
    int i;
    if (part < 0 || part > 5) return NULL;
    h = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!h) return NULL;
    vd_lock();
    for (i = 0; i < VD_MAX_HANDLES; i++)
        if (!s_handles[i].h) {
            s_handles[i].h = h; s_handles[i].part = part; s_handles[i].pos = 0;
            vd_unlock();
            fprintf(stderr, "  [DISK] Partition%d opened (emulated device)\n", part);
            return h;
        }
    vd_unlock();
    CloseHandle(h);
    return NULL;
}

static VD_HANDLE *vd_find(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    for (i = 0; i < VD_MAX_HANDLES; i++)
        if (s_handles[i].h == h) return &s_handles[i];
    return NULL;
}

int xbox_vdisk_part(HANDLE h)
{
    VD_HANDLE *v;
    int part;
    if (s_ready != 2 || !h) return -1;
    vd_lock();
    v = vd_find(h);
    part = v ? v->part : -1;
    vd_unlock();
    return part;
}

int xbox_vdisk_close(HANDLE h)
{
    VD_HANDLE *v;
    if (s_ready != 2) return 0;
    vd_lock();
    v = vd_find(h);
    if (v) memset(v, 0, sizeof *v);
    vd_unlock();
    if (!v) return 0;
    CloseHandle(h);
    return 1;
}

/* Reads and writes: at `off`, or at the handle's position when off is NULL.
 * Past the end reads nothing (end of file) and writes are cut at the end. */
NTSTATUS xbox_vdisk_io(HANDLE h, int write, void *buf, uint32_t len, const LARGE_INTEGER *off, uint32_t *done)
{
    VD_HANDLE *v;
    uint64_t at, size;
    uint32_t n;
    *done = 0;
    vd_lock();
    v = vd_find(h);
    if (!v) { vd_unlock(); return STATUS_INVALID_HANDLE; }
    at = (off && off->QuadPart >= 0) ? (uint64_t)off->QuadPart : v->pos;
    size = xbox_vdisk_size(v->part);
    if (at >= size) { vd_unlock(); return len ? STATUS_END_OF_FILE : STATUS_SUCCESS; }
    n = (uint64_t)len > size - at ? (uint32_t)(size - at) : len;
    {   /* debug: RECOMP_VDISK_TRACE=1 logs each raw access (partition, offset, size) */
        static int trace = -1;
        if (trace < 0) { const char *e = getenv("RECOMP_VDISK_TRACE"); trace = e && e[0] == '1'; }
        if (trace)
            fprintf(stderr, "  [VDISK] %s Partition%d off 0x%llX len 0x%X\n", write ? "write" : "read ",
                    v->part, (unsigned long long)at, n);
    }
    if (write) {
        if (v->part >= 3 && v->part <= 5 && at == 0 && n >= 4 &&
            !memcmp(buf, "FATX", 4))
            vd_cache_formatted(v->part);
        vd_copy_in(v->part, at, (const uint8_t *)buf, n);
        if (v->part >= 3 && v->part <= 5 && at < VD_CACHE_HDR)
            vd_save_cache_hdr(v->part);
        if (v->part == 0 && at < VD_CFG_OFFSET + VD_CFG_SIZE && at + n > VD_CFG_OFFSET)
            vd_save_config();
        if (v->part == 0 && at < VD_CACHEDB_OFFSET + VD_CACHEDB_SIZE && at + n > VD_CACHEDB_OFFSET)
            vd_save_cachedb();
    } else
        vd_copy_out(v->part, at, (uint8_t *)buf, n);
    v->pos = at + n;
    *done = n;
    vd_unlock();
    return STATUS_SUCCESS;
}

int xbox_vdisk_get_pos(HANDLE h, uint64_t *pos)
{
    VD_HANDLE *v;
    vd_lock();
    v = vd_find(h);
    if (v) *pos = v->pos;
    vd_unlock();
    return v != NULL;
}

int xbox_vdisk_set_pos(HANDLE h, uint64_t pos)
{
    VD_HANDLE *v;
    vd_lock();
    v = vd_find(h);
    if (v) v->pos = pos;
    vd_unlock();
    return v != NULL;
}

#endif /* _WIN32 */
