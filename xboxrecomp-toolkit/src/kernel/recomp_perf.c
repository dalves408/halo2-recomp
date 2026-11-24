/* recomp_perf.c -- see recomp_perf.h. */
#include "recomp_perf.h"

#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_rpz_on = -1;

static volatile LONG64 s_ticks[RPZ_COUNT], s_counts[RPZ_COUNT];
static LONG64 s_prev_ticks[RPZ_COUNT], s_prev_counts[RPZ_COUNT];
static FILE *s_csv;
static double s_ms_per_tick;
static uint64_t s_t0, s_last_flip;
static HANDLE s_main_thread, s_ack_thread;
static uint64_t s_main_cyc, s_ack_cyc;
static double s_cyc_per_ms;
static uint32_t s_flip_no;
static DWORD s_last_flush;

static const char *const s_names[RPZ_COUNT] = {
    "ack_pass", "ack_idle", "pb_walk", "flip_held", "readback", "present",
    "draw", "draw_pixel", "draw_tex", "draw_vertex", "draw_submit", "gpu_wait",
    "shader_compile", "tex_upload", "clear", "fb_present", "guest_gpu_wait",
    "user0", "user1", "user2", "user3",
};

static void rpz_open(void)
{
    const char *path = getenv("RECOMP_PERF_CSV");
    LARGE_INTEGER f, q;
    int i;
    if (!path || !*path) { g_rpz_on = 0; return; }
    s_csv = fopen(path, "w");
    if (!s_csv) {
        fprintf(stderr, "[PERF] cannot open RECOMP_PERF_CSV %s\n", path);
        g_rpz_on = 0;
        return;
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    s_ms_per_tick = 1000.0 / (double)f.QuadPart;
    s_t0 = s_last_flip = (uint64_t)q.QuadPart;
    {   /* thread cycle counter rate (TSC), for CPU ms per frame */
        LARGE_INTEGER a, b; uint64_t c0, c1;
        QueryPerformanceCounter(&a); c0 = __rdtsc();
        Sleep(50);
        QueryPerformanceCounter(&b); c1 = __rdtsc();
        s_cyc_per_ms = (double)(c1 - c0) / ((double)(b.QuadPart - a.QuadPart) * s_ms_per_tick);
    }
    fprintf(s_csv, "flip,t_launch_ms,t_us,frame_ms,draws,main_cpu_ms,ack_cpu_ms");
    for (i = 0; i < RPZ_COUNT; i++)
        fprintf(s_csv, ",%s_ms,%s_n", s_names[i], s_names[i]);
    fprintf(s_csv, ",ab\n");
    fflush(s_csv);
    g_rpz_on = 1;
    fprintf(stderr, "[PERF] per-frame zones -> %s\n", path);
}

static volatile LONG s_ab_mask;
static volatile LONG64 s_ab_next;
static int s_ab_state = -1;            /* -1 unread, 0 off, 1 on */
static char s_ab_path[MAX_PATH];

int rpz_ab_off(int bit)
{
    ULONGLONG now;
    if (s_ab_state < 0) {
        const char *p = getenv("RECOMP_PERF_AB");
        if (p && *p && strlen(p) < sizeof s_ab_path) {
            strcpy(s_ab_path, p);
            fprintf(stderr, "[PERF] A/B switch file %s\n", p);
            s_ab_state = 1;
        } else s_ab_state = 0;
    }
    if (!s_ab_state) return 0;
    now = GetTickCount64();
    if ((LONG64)now >= s_ab_next) {
        LONG mask = 0;
        FILE *f = fopen(s_ab_path, "r");
        if (f) {
            char buf[32] = {0};
            fgets(buf, sizeof buf, f);
            fclose(f);
            mask = buf[0] ? (LONG)strtol(buf, NULL, 0) : 0xFF;
            if (!mask) mask = 0xFF;
        }
        InterlockedExchange(&s_ab_mask, mask);
        InterlockedExchange64(&s_ab_next, (LONG64)now + 200);
    }
    return (s_ab_mask & bit) != 0;
}

uint64_t rpz_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint64_t)q.QuadPart;
}

void rpz_add(RpzZone z, uint64_t ticks)
{
    InterlockedAdd64(&s_ticks[z], (LONG64)ticks);
    InterlockedIncrement64(&s_counts[z]);
}

void rpz_count(RpzZone z, uint64_t n)
{
    InterlockedAdd64(&s_counts[z], (LONG64)n);
}

/* The two handles are kept with or without RECOMP_PERF_CSV: rpz_usage_report
 * reads their CPU time for the log. */
void rpz_init_main_thread(void)
{
    if (!s_main_thread)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &s_main_thread, THREAD_QUERY_INFORMATION, FALSE, 0);
    if (g_rpz_on < 0) rpz_open();
}

void rpz_register_ack_thread(void)
{
    if (!s_ack_thread)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &s_ack_thread, THREAD_QUERY_INFORMATION, FALSE, 0);
    if (g_rpz_on < 0) rpz_open();
}

/* ---- CPU and GPU use in the log -----------------------------------------------
 *
 * With the 10-second fps line: the process's CPU time (of one core, and of all
 * of them), the game thread's and the GPU command thread's, and the game's use
 * of the GPU's 3D engine, as Task Manager shows it. Both threads spin while they
 * wait for each other, so near 100% there means "never idle", not "all work";
 * the GPU figure says which side waits: a low GPU use with a low frame rate is
 * a CPU-bound PC. The CPU times are three system calls every 10 s. The GPU
 * figure comes from the "GPU Engine" performance counters (PDH), read by its
 * own idle-priority thread every 10 s, never on the game's threads. At the
 * first report: whether the install's drive is an SSD or a hard disk. */
#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
#pragma comment(lib, "pdh.lib")

static volatile LONG s_gpu_util_x10 = -1;   /* 3D engine use, tenths of a percent; -1 none yet */

static DWORD WINAPI usage_gpu_thread(LPVOID unused)
{
    PDH_HQUERY q = NULL;
    PDH_HCOUNTER c = NULL;
    WCHAR path[128];
    PDH_FMT_COUNTERVALUE_ITEM_W *items = NULL;
    DWORD cap = 0;
    (void)unused;
    swprintf(path, sizeof path / sizeof path[0],
             L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", (unsigned long)GetCurrentProcessId());
    if (PdhOpenQueryW(NULL, 0, &q) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(q, path, 0, &c) != ERROR_SUCCESS) {
        fprintf(stderr, "[USAGE] GPU use is not available on this Windows (no GPU Engine counters)\n");
        if (q) PdhCloseQuery(q);
        return 0;
    }
    PdhCollectQueryData(q);
    for (;;) {
        DWORD bytes = 0, n = 0, i;
        double sum = 0;
        PDH_STATUS st;
        Sleep(10000);
        if (PdhCollectQueryData(q) != ERROR_SUCCESS)
            continue;
        st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &n, NULL);
        if (st != PDH_MORE_DATA || !bytes)
            continue;
        if (bytes > cap) {
            free(items);
            items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(bytes);
            cap = items ? bytes : 0;
            if (!items) continue;
        }
        bytes = cap;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &n, items) != ERROR_SUCCESS)
            continue;
        for (i = 0; i < n; i++)     /* the 3D engines only (not copy, video or compute) */
            if (items[i].szName && wcsstr(items[i].szName, L"engtype_3D") &&
                items[i].FmtValue.CStatus == ERROR_SUCCESS)
                sum += items[i].FmtValue.doubleValue;
        InterlockedExchange(&s_gpu_util_x10, (LONG)(sum * 10.0 + 0.5));
    }
}

/* SSD or hard disk under the folder the program runs from (no seek penalty = SSD). */
static const char *usage_drive_kind(void)
{
    WCHAR exe[MAX_PATH], vol[MAX_PATH], dev[64];
    STORAGE_PROPERTY_QUERY pq;
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp;
    HANDLE h;
    DWORD got = 0;
    BOOL ok;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH) || !GetVolumePathNameW(exe, vol, MAX_PATH))
        return "unknown";
    if (wcslen(vol) < 2 || vol[1] != L':')
        return "unknown (network or mounted folder)";
    swprintf(dev, sizeof dev / sizeof dev[0], L"\\\\.\\%c:", vol[0]);
    h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return "unknown";
    memset(&pq, 0, sizeof pq);
    pq.PropertyId = StorageDeviceSeekPenaltyProperty;
    pq.QueryType = PropertyStandardQuery;
    memset(&sp, 0, sizeof sp);
    ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &pq, sizeof pq, &sp, sizeof sp, &got, NULL);
    CloseHandle(h);
    if (!ok || got < sizeof sp)
        return "unknown";
    return sp.IncursSeekPenalty ? "a hard disk (HDD)" : "an SSD";
}

/* Disk: reads, bytes, ticks waited and the slowest read since the last report.
 * A read over a quarter of a second is also logged on its own with the file's
 * name (no folder), at most 20 a session. */
static volatile LONG64 s_disk_reads, s_disk_bytes, s_disk_ticks, s_disk_max;
static volatile LONG s_disk_slow_logged;
static double s_disk_ms_per_tick;

static double disk_ms_per_tick(void)
{
    if (!s_disk_ms_per_tick) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_disk_ms_per_tick = 1000.0 / (double)f.QuadPart;
    }
    return s_disk_ms_per_tick;
}

void rpz_disk_read(uint64_t bytes, uint64_t ticks, void *handle)
{
    LONG64 prev;
    double ms;
    InterlockedIncrement64(&s_disk_reads);
    InterlockedAdd64(&s_disk_bytes, (LONG64)bytes);
    InterlockedAdd64(&s_disk_ticks, (LONG64)ticks);
    prev = s_disk_max;
    while ((LONG64)ticks > prev &&
           InterlockedCompareExchange64(&s_disk_max, (LONG64)ticks, prev) != prev)
        prev = s_disk_max;
    ms = (double)ticks * disk_ms_per_tick();
    if (ms >= 250.0) {
        LONG n = InterlockedIncrement(&s_disk_slow_logged);
        if (n <= 20) {
            WCHAR path[MAX_PATH], *name;
            DWORD len = handle ? GetFinalPathNameByHandleW((HANDLE)handle, path, MAX_PATH, 0) : 0;
            name = len && len < MAX_PATH ? wcsrchr(path, L'\\') : NULL;
            fprintf(stderr, "[DISK] slow read: %.0f KB from %ls took %.0f ms%s\n",
                    (double)bytes / 1024.0, name ? name + 1 : L"(a game file)", ms,
                    n == 20 ? " (further slow reads are not listed)" : "");
        }
    }
}

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static uint64_t thread_100ns(HANDLE h)
{
    FILETIME a, b, k, u;
    return h && GetThreadTimes(h, &a, &b, &k, &u) ? ft64(k) + ft64(u) : 0;
}

void rpz_usage_report(void)
{
    static uint64_t last_wall, last_proc, last_main, last_ack;
    static int started;
    static DWORD ncpu;
    FILETIME a, b, k, u, now_ft;
    uint64_t wall, proc, mainT, ackT;
    LONG gpu;
    GetSystemTimeAsFileTime(&now_ft);
    wall = ft64(now_ft);
    proc = GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u) ? ft64(k) + ft64(u) : 0;
    mainT = thread_100ns(s_main_thread);
    ackT = thread_100ns(s_ack_thread);
    if (!started) {
        SYSTEM_INFO si;
        HANDLE t;
        started = 1;
        GetSystemInfo(&si);
        ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
        fprintf(stderr, "[USAGE] the game runs from %s\n", usage_drive_kind());
        t = CreateThread(NULL, 0, usage_gpu_thread, NULL, 0, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_IDLE);
            CloseHandle(t);
        }
    } else if (wall > last_wall) {
        double span = (double)(wall - last_wall);
        double p = 100.0 * (double)(proc - last_proc) / span;
        gpu = s_gpu_util_x10;
        fprintf(stderr, "[USAGE] last %.0f s: CPU %.0f%% of one core (%.0f%% of all %lu);"
                        " game thread %.0f%%, GPU command thread %.0f%%; GPU 3D ",
                span / 1e7, p, p / ncpu, (unsigned long)ncpu,
                s_main_thread ? 100.0 * (double)(mainT - last_main) / span : -1.0,
                s_ack_thread ? 100.0 * (double)(ackT - last_ack) / span : -1.0);
        if (gpu >= 0)
            fprintf(stderr, "%.0f%%", gpu / 10.0);
        else
            fprintf(stderr, "n/a");
        {
            LONG64 reads = InterlockedExchange64(&s_disk_reads, 0);
            LONG64 bytes = InterlockedExchange64(&s_disk_bytes, 0);
            LONG64 ticks = InterlockedExchange64(&s_disk_ticks, 0);
            LONG64 worst = InterlockedExchange64(&s_disk_max, 0);
            double tick_ms = disk_ms_per_tick();
            fprintf(stderr, "; disk %.1f MB in %lld reads, %.0f ms waiting, slowest %.0f ms\n",
                    (double)bytes / 1048576.0, (long long)reads, (double)ticks * tick_ms,
                    (double)worst * tick_ms);
        }
    }
    last_wall = wall;
    last_proc = proc;
    last_main = mainT;
    last_ack = ackT;
}

static double thread_ms(HANDLE h, uint64_t *prev)
{
    ULONG64 c = 0;
    double ms;
    if (!h || !QueryThreadCycleTime(h, &c)) return -1;
    ms = *prev ? (double)(c - *prev) / s_cyc_per_ms : 0;
    *prev = c;
    return ms;
}

void rpz_flip(uint32_t draws)
{
    uint64_t now;
    int i;
    DWORD tick;
    if (g_rpz_on <= 0 || !s_csv) return;
    now = rpz_now();
    s_flip_no++;
    fprintf(s_csv, "%u,%.1f,%llu,%.3f,%u,%.3f,%.3f", s_flip_no,
            (double)(now - s_t0) * s_ms_per_tick,
            (unsigned long long)((double)now * s_ms_per_tick * 1000.0),
            (double)(now - s_last_flip) * s_ms_per_tick, draws,
            thread_ms(s_main_thread, &s_main_cyc), thread_ms(s_ack_thread, &s_ack_cyc));
    for (i = 0; i < RPZ_COUNT; i++) {
        LONG64 t = s_ticks[i], n = s_counts[i];
        fprintf(s_csv, ",%.3f,%lld", (double)(t - s_prev_ticks[i]) * s_ms_per_tick,
                (long long)(n - s_prev_counts[i]));
        s_prev_ticks[i] = t;
        s_prev_counts[i] = n;
    }
    rpz_ab_off(0);
    fprintf(s_csv, ",%ld\n", (long)s_ab_mask);
    s_last_flip = now;
    tick = GetTickCount();
    if (tick - s_last_flush > 500) { fflush(s_csv); s_last_flush = tick; }
}

#else  /* not Windows: zones compile to nothing */
int g_rpz_on = 0;
uint64_t rpz_now(void) { return 0; }
void rpz_add(RpzZone z, uint64_t ticks) { (void)z; (void)ticks; }
void rpz_count(RpzZone z, uint64_t n) { (void)z; (void)n; }
void rpz_init_main_thread(void) {}
void rpz_register_ack_thread(void) {}
void rpz_usage_report(void) {}
void rpz_disk_read(uint64_t bytes, uint64_t ticks, void *handle) { (void)bytes; (void)ticks; (void)handle; }
void rpz_flip(uint32_t draws) { (void)draws; }
int rpz_ab_off(int bit) { (void)bit; return 0; }
#endif
