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

void rpz_init_main_thread(void)
{
    if (g_rpz_on < 0) rpz_open();
    if (g_rpz_on <= 0) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_main_thread, THREAD_QUERY_INFORMATION, FALSE, 0);
}

void rpz_register_ack_thread(void)
{
    if (g_rpz_on < 0) rpz_open();
    if (g_rpz_on <= 0) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_ack_thread, THREAD_QUERY_INFORMATION, FALSE, 0);
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
void rpz_flip(uint32_t draws) { (void)draws; }
int rpz_ab_off(int bit) { (void)bit; return 0; }
#endif
