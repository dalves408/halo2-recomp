/*
 * perf_sampler.c -- in-process statistical CPU profiler (Windows x64).
 *
 * Env (all off by default; nothing runs unless RECOMP_PROF is set):
 *   RECOMP_PROF=<report.txt>    enable; writes <report.txt> and <report.txt>.folded
 *   RECOMP_PROF_HZ=<n>          sample rate per thread (default 1000)
 *   RECOMP_PROF_SECS=<s>        sampling window length (default 20)
 *   RECOMP_PROF_START_S=<s>     start this many seconds after launch (default 0)
 *   RECOMP_PROF_TRIGGER=<file>  start when <file> exists instead (window still SECS)
 *
 * A sampler thread wakes HZ times a second. For every other thread that
 * consumed CPU cycles since its last visit (QueryThreadCycleTime), it
 * suspends the thread, reads its context, walks the stack and resumes it.
 * Threads that did not run are counted as idle without being suspended.
 *
 * Stack walking avoids any loader or dynamic-function-table lock that a
 * suspended thread might hold: frames inside the main executable unwind
 * through the image's own .pdata with RtlVirtualUnwind, and a leaf in any
 * other module (ntdll, d3d11, the GPU driver) is bridged to the first
 * plausible return address into the executable found on the stack.
 *
 * The report lists per-thread CPU use over the window and, for each busy
 * thread, the top self (exclusive) and total (inclusive) functions. The
 * .folded file is "thread;outer;...;leaf count" for flame graph tools.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "perf_sampler.h"

#define PS_MAX_THREADS 256
#define PS_MAX_DEPTH   32
#define PS_STACK_SCAN  (64 * 1024)

typedef struct {
    DWORD tid;
    HANDLE h;
    uintptr_t start;
    char name[96];
    uint64_t last_cycles;
    uint64_t cyc_begin, cyc_end;
    uint64_t samples_run, samples_idle;
    int seen_begin;
} PsThread;

typedef struct {
    uint32_t hash;
    uint16_t thread;
    uint16_t depth;
    uint32_t count;
    uintptr_t pc[PS_MAX_DEPTH];   /* pc[0] = leaf */
} PsStack;

static PsThread s_thr[PS_MAX_THREADS];
static int s_nthr;
static PsStack *s_stacks;
static uint32_t s_stack_cap, s_nstacks;
static uint32_t *s_index;       /* open addressing: stack index + 1 */
static uint64_t s_dropped;

static uintptr_t s_img_base, s_img_end, s_text_lo, s_text_hi;
static const RUNTIME_FUNCTION *s_pdata;
static uint32_t s_npdata;

static char s_path[MAX_PATH];
static unsigned s_hz = 1000, s_secs = 20, s_start_s;
static char s_trigger[MAX_PATH];
static double s_cycles_per_sec;

typedef HRESULT (WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);
typedef LONG (NTAPI *NtQIT_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static void ps_image_init(void)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *ex =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    WORD i;
    s_img_base = (uintptr_t)base;
    s_img_end = s_img_base + nt->OptionalHeader.SizeOfImage;
    s_pdata = (const RUNTIME_FUNCTION *)(base + ex->VirtualAddress);
    s_npdata = ex->Size / sizeof(RUNTIME_FUNCTION);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            uintptr_t lo = s_img_base + sec[i].VirtualAddress;
            uintptr_t hi = lo + sec[i].Misc.VirtualSize;
            if (!s_text_lo || lo < s_text_lo) s_text_lo = lo;
            if (hi > s_text_hi) s_text_hi = hi;
        }
    }
}

static const RUNTIME_FUNCTION *ps_lookup(uintptr_t pc)
{
    uint32_t rva, lo = 0, hi = s_npdata;
    if (pc < s_img_base || pc >= s_img_end)
        return NULL;
    rva = (uint32_t)(pc - s_img_base);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rva < s_pdata[mid].BeginAddress) hi = mid;
        else if (rva >= s_pdata[mid].EndAddress) lo = mid + 1;
        else return &s_pdata[mid];
    }
    return NULL;
}

static int ps_in_text(uintptr_t a) { return a >= s_text_lo && a < s_text_hi; }

/* Does the code before return address a look like a call? */
static int ps_after_call(uintptr_t a)
{
    const uint8_t *p = (const uint8_t *)a;
    if (!ps_in_text(a - 7))
        return 0;
    if (p[-5] == 0xE8) return 1;                                   /* call rel32 */
    if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return 1;        /* call r/m */
    if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return 1;
    if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return 1;        /* call [rip+d32] */
    if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2) return 1;
    return 0;
}

/* Walk. Fills pcs (leaf first). */
static int ps_walk(CONTEXT *ctx, uintptr_t *pcs, uintptr_t stack_lo, uintptr_t stack_hi)
{
    int n = 0;
    __try {
        pcs[n++] = (uintptr_t)ctx->Rip;
        if (!ps_in_text((uintptr_t)ctx->Rip)) {
            /* Foreign leaf: bridge to the first return address into the exe. */
            uintptr_t sp = (uintptr_t)ctx->Rsp, end = sp + PS_STACK_SCAN;
            if (end > stack_hi) end = stack_hi;
            for (; sp + 8 <= end; sp += 8) {
                uintptr_t v = *(const uintptr_t *)sp;
                if (ps_in_text(v) && ps_after_call(v)) {
                    ctx->Rip = v;
                    ctx->Rsp = sp + 8;
                    pcs[n++] = v;
                    break;
                }
            }
            if (n == 1)
                return n;
        }
        while (n < PS_MAX_DEPTH) {
            const RUNTIME_FUNCTION *rf = ps_lookup((uintptr_t)ctx->Rip);
            if (rf) {
                PVOID handler_data; DWORD64 establisher;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, s_img_base, ctx->Rip,
                                 (PRUNTIME_FUNCTION)rf, ctx, &handler_data,
                                 &establisher, NULL);
            } else {
                if (ctx->Rsp < stack_lo || ctx->Rsp + 8 > stack_hi) break;
                ctx->Rip = *(const DWORD64 *)ctx->Rsp;
                ctx->Rsp += 8;
            }
            if (!ctx->Rip || ctx->Rsp < stack_lo || ctx->Rsp > stack_hi)
                break;
            if (!ps_in_text((uintptr_t)ctx->Rip))
                break;
            pcs[n++] = (uintptr_t)ctx->Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static uint32_t ps_hash(uint16_t t, const uintptr_t *pcs, int n)
{
    uint64_t h = 1469598103934665603ull ^ t;
    int i;
    for (i = 0; i < n; i++) { h ^= pcs[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return (uint32_t)(h ^ (h >> 32)) | 1u;
}

static void ps_add(uint16_t t, const uintptr_t *pcs, int n)
{
    uint32_t h = ps_hash(t, pcs, n), mask = s_stack_cap * 2 - 1, i = h & mask;
    for (;;) {
        uint32_t idx = s_index[i];
        if (!idx) break;
        {
            PsStack *s = &s_stacks[idx - 1];
            if (s->hash == h && s->thread == t && s->depth == n &&
                !memcmp(s->pc, pcs, n * sizeof *pcs)) { s->count++; return; }
        }
        i = (i + 1) & mask;
    }
    if (s_nstacks >= s_stack_cap) { s_dropped++; return; }
    {
        PsStack *s = &s_stacks[s_nstacks++];
        s->hash = h; s->thread = t; s->depth = (uint16_t)n; s->count = 1;
        memcpy(s->pc, pcs, n * sizeof *pcs);
        s_index[i] = s_nstacks;
    }
}

static void ps_name_thread(PsThread *t)
{
    static GetThreadDescription_t gtd;
    static NtQIT_t nqit;
    static int init;
    PWSTR w = NULL;
    if (!init) {
        init = 1;
        gtd = (GetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                              "GetThreadDescription");
        nqit = (NtQIT_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                "NtQueryInformationThread");
    }
    t->name[0] = 0;
    if (gtd && SUCCEEDED(gtd(t->h, &w)) && w) {
        if (w[0]) WideCharToMultiByte(CP_UTF8, 0, w, -1, t->name, sizeof t->name, NULL, NULL);
        LocalFree(w);
    }
    if (nqit) {
        PVOID start = NULL;
        if (nqit(t->h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL) >= 0)
            t->start = (uintptr_t)start;
    }
}

static void ps_refresh_threads(DWORD self)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) do {
        int i, known = 0;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        for (i = 0; i < s_nthr; i++) if (s_thr[i].tid == te.th32ThreadID) { known = 1; break; }
        if (known || s_nthr >= PS_MAX_THREADS) continue;
        {
            PsThread *t = &s_thr[s_nthr];
            memset(t, 0, sizeof *t);
            t->tid = te.th32ThreadID;
            t->h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_QUERY_INFORMATION, FALSE, t->tid);
            if (!t->h) continue;
            ps_name_thread(t);
            QueryThreadCycleTime(t->h, &t->last_cycles);
            s_nthr++;
        }
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static void ps_calibrate(void)
{
    LARGE_INTEGER f, a, b;
    uint64_t c0, c1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a); c0 = __rdtsc();
    Sleep(200);
    QueryPerformanceCounter(&b); c1 = __rdtsc();
    s_cycles_per_sec = (double)(c1 - c0) * (double)f.QuadPart / (double)(b.QuadPart - a.QuadPart);
}

/* ---- report ---- */

/* pc -> interned function name id. Names are interned by content, so all
 * pcs inside one function aggregate together. Fixed capacity: nothing is
 * reallocated while ids or name pointers are held. */
#define PS_SYM_CAP  (1u << 19)
#define PS_NAME_CAP (1u << 18)
typedef struct { uintptr_t pc; int32_t id; } PsSym;
static PsSym *s_symtab;                 /* open addressing on pc */
static char (*s_names)[200];
static uint32_t *s_name_index;          /* open addressing on content: id + 1 */
static uint32_t s_nnames;

static uint32_t ps_strhash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int32_t ps_intern(const char *name)
{
    uint32_t i = ps_strhash(name) & (PS_NAME_CAP * 2 - 1);
    for (;;) {
        uint32_t v = s_name_index[i];
        if (!v) break;
        if (!strcmp(s_names[v - 1], name)) return (int32_t)(v - 1);
        i = (i + 1) & (PS_NAME_CAP * 2 - 1);
    }
    if (s_nnames >= PS_NAME_CAP) return 0;
    strncpy(s_names[s_nnames], name, sizeof s_names[0] - 1);
    s_names[s_nnames][sizeof s_names[0] - 1] = 0;
    s_name_index[i] = ++s_nnames;
    return (int32_t)(s_nnames - 1);
}

static int32_t ps_sym_id(uintptr_t pc)
{
    uint64_t h = (uint64_t)pc * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 40) & (PS_SYM_CAP - 1);
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    HMODULE mod = NULL;
    char modname[MAX_PATH] = "?", name[256];
    for (;;) {
        if (!s_symtab[i].pc) break;
        if (s_symtab[i].pc == pc) return s_symtab[i].id;
        i = (i + 1) & (PS_SYM_CAP - 1);
    }
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)pc, &mod) && mod) {
        char full[MAX_PATH]; const char *slash;
        full[0] = 0;
        GetModuleFileNameA(mod, full, sizeof full);
        slash = strrchr(full, '\\');
        strncpy(modname, slash ? slash + 1 : full, sizeof modname - 1);
    }
    memset(si, 0, sizeof(SYMBOL_INFO));
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)pc, &disp, si)) {
        si->Name[255] = 0;
        if ((uintptr_t)mod == s_img_base)
            _snprintf(name, sizeof name, "%s", si->Name);
        else
            _snprintf(name, sizeof name, "%s!%s", modname, si->Name);
    } else if (mod) {
        _snprintf(name, sizeof name, "%s!+0x%llx", modname,
                  (unsigned long long)(pc - (uintptr_t)mod));
    } else {
        _snprintf(name, sizeof name, "?!0x%llx", (unsigned long long)pc);
    }
    name[sizeof name - 1] = 0;
    s_symtab[i].pc = pc;
    s_symtab[i].id = ps_intern(name);
    return s_symtab[i].id;
}

static const char *ps_sym(uintptr_t pc) { return s_names[ps_sym_id(pc)]; }

typedef struct { int32_t id; uint64_t self, total; } PsAgg;

static int ps_cmp_self(const void *a, const void *b)
{
    const PsAgg *x = (const PsAgg *)a, *y = (const PsAgg *)b;
    return x->self < y->self ? 1 : x->self > y->self ? -1 : 0;
}
static int ps_cmp_total(const void *a, const void *b)
{
    const PsAgg *x = (const PsAgg *)a, *y = (const PsAgg *)b;
    return x->total < y->total ? 1 : x->total > y->total ? -1 : 0;
}

static void ps_thread_label(const PsThread *t, char *out, size_t n)
{
    if (t->name[0]) _snprintf(out, n, "%s", t->name);
    else if (t->start) _snprintf(out, n, "%s", ps_sym(t->start));
    else _snprintf(out, n, "tid%lu", (unsigned long)t->tid);
    out[n - 1] = 0;
}

static void ps_report(double window_s)
{
    FILE *f = fopen(s_path, "w");
    char folded_path[MAX_PATH + 16];
    FILE *ff;
    int t;
    uint32_t i;
    uint64_t *self, *total;
    uint32_t *stamp;
    s_symtab = (PsSym *)VirtualAlloc(NULL, PS_SYM_CAP * sizeof *s_symtab,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_names = (char (*)[200])VirtualAlloc(NULL, (SIZE_T)PS_NAME_CAP * sizeof s_names[0],
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_name_index = (uint32_t *)VirtualAlloc(NULL, PS_NAME_CAP * 2 * sizeof(uint32_t),
                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    self = (uint64_t *)calloc(PS_NAME_CAP, sizeof *self);
    total = (uint64_t *)calloc(PS_NAME_CAP, sizeof *total);
    stamp = (uint32_t *)calloc(PS_NAME_CAP, sizeof *stamp);
    _snprintf(folded_path, sizeof folded_path, "%s.folded", s_path);
    ff = fopen(folded_path, "w");
    if (!f || !s_symtab || !s_names || !s_name_index || !self || !total || !stamp) {
        fprintf(stderr, "[PROF] cannot write report\n");
        return;
    }
    ps_intern("?");
    SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymRefreshModuleList(GetCurrentProcess());

    fprintf(f, "RECOMP_PROF report: window %.2f s, %u Hz, %u unique stacks, %llu dropped\n",
            window_s, s_hz, s_nstacks, (unsigned long long)s_dropped);
    fprintf(f, "Per-thread CPU (thread cycles at %.0f MHz over the window; 100%% = one core)\n\n",
            s_cycles_per_sec / 1e6);
    fprintf(f, "  %7s  %8s  %8s  %6s  %s\n", "cpu%", "run-smp", "idle-smp", "tid", "thread");
    for (t = 0; t < s_nthr; t++) {
        PsThread *th = &s_thr[t];
        char label[160];
        double pct = th->seen_begin && th->cyc_end > th->cyc_begin
            ? 100.0 * (double)(th->cyc_end - th->cyc_begin) / s_cycles_per_sec / window_s : 0;
        if (!th->samples_run && pct < 0.5) continue;
        ps_thread_label(th, label, sizeof label);
        fprintf(f, "  %6.1f%%  %8llu  %8llu  %6lu  %s\n", pct,
                (unsigned long long)th->samples_run, (unsigned long long)th->samples_idle,
                (unsigned long)th->tid, label);
    }
    for (t = 0; t < s_nthr; t++) {
        PsThread *th = &s_thr[t];
        char label[160];
        uint32_t cur = 0, nused = 0, k;
        uint64_t tot = th->samples_run;
        int32_t *used;
        PsAgg *list;
        if (tot < (uint64_t)(s_hz * window_s * 0.02)) continue;   /* < 2% of a core */
        ps_thread_label(th, label, sizeof label);
        used = (int32_t *)malloc(PS_NAME_CAP * sizeof *used);
        if (!used) break;
        for (i = 0; i < s_nstacks; i++) {
            PsStack *s = &s_stacks[i];
            int d;
            if (s->thread != t) continue;
            cur++;
            for (d = 0; d < s->depth; d++) {
                int32_t id = ps_sym_id(s->pc[d]);
                if (!self[id] && !total[id]) used[nused++] = id;
                if (d == 0) self[id] += s->count;
                if (stamp[id] != cur) { stamp[id] = cur; total[id] += s->count; }
            }
            if (ff) {
                fprintf(ff, "%s", label);
                for (d = s->depth - 1; d >= 0; d--)
                    fprintf(ff, ";%s", ps_sym(s->pc[d]));
                fprintf(ff, " %u\n", s->count);
            }
        }
        list = (PsAgg *)malloc((nused ? nused : 1) * sizeof *list);
        for (k = 0; k < nused; k++) {
            list[k].id = used[k]; list[k].self = self[used[k]]; list[k].total = total[used[k]];
            self[used[k]] = total[used[k]] = 0; stamp[used[k]] = 0;
        }
        fprintf(f, "\n=== thread %s (tid %lu): %llu running samples ===\n",
                label, (unsigned long)th->tid, (unsigned long long)tot);
        qsort(list, nused, sizeof *list, ps_cmp_self);
        fprintf(f, "  -- self (exclusive) --\n");
        for (k = 0; k < nused && k < 50; k++)
            if (list[k].self)
                fprintf(f, "  %6.2f%%  %7llu  %s\n", 100.0 * list[k].self / tot,
                        (unsigned long long)list[k].self, s_names[list[k].id]);
        qsort(list, nused, sizeof *list, ps_cmp_total);
        fprintf(f, "  -- total (inclusive) --\n");
        for (k = 0; k < nused && k < 70; k++)
            fprintf(f, "  %6.2f%%  %7llu  %s\n", 100.0 * list[k].total / tot,
                    (unsigned long long)list[k].total, s_names[list[k].id]);
        free(list);
        free(used);
    }
    fclose(f);
    if (ff) fclose(ff);
    fprintf(stderr, "[PROF] report written to %s\n", s_path);
}

static int ps_file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI ps_thread(LPVOID arg)
{
    DWORD self = GetCurrentThreadId();
    LARGE_INTEGER qf, q0, qn;
    uint64_t period_ticks, next;
    double window_s;
    int t;
    (void)arg;
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ps_calibrate();
    if (s_trigger[0]) {
        while (!ps_file_exists(s_trigger)) Sleep(100);
    } else if (s_start_s) {
        Sleep(s_start_s * 1000u);
    }
    fprintf(stderr, "[PROF] sampling %u Hz for %u s\n", s_hz, s_secs);
    QueryPerformanceFrequency(&qf);
    period_ticks = (uint64_t)qf.QuadPart / s_hz;
    ps_refresh_threads(self);
    for (t = 0; t < s_nthr; t++) {
        QueryThreadCycleTime(s_thr[t].h, &s_thr[t].cyc_begin);
        s_thr[t].last_cycles = s_thr[t].cyc_begin;
        s_thr[t].seen_begin = 1;
    }
    QueryPerformanceCounter(&q0);
    next = (uint64_t)q0.QuadPart;
    for (;;) {
        static uint64_t refresh;
        QueryPerformanceCounter(&qn);
        if ((uint64_t)(qn.QuadPart - q0.QuadPart) >= (uint64_t)qf.QuadPart * s_secs)
            break;
        if (++refresh % 256 == 0) {
            int before = s_nthr;
            ps_refresh_threads(self);
            for (t = before; t < s_nthr; t++) {
                s_thr[t].cyc_begin = s_thr[t].last_cycles;
                s_thr[t].seen_begin = 1;
            }
        }
        for (t = 0; t < s_nthr; t++) {
            PsThread *th = &s_thr[t];
            uint64_t cyc = 0;
            CONTEXT ctx;
            NT_TIB tib;
            if (!QueryThreadCycleTime(th->h, &cyc)) continue;
            if (cyc == th->last_cycles) { th->samples_idle++; continue; }
            th->last_cycles = cyc;
            if (SuspendThread(th->h) == (DWORD)-1) continue;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(th->h, &ctx)) {
                uintptr_t pcs[PS_MAX_DEPTH];
                int n;
                /* Stack bounds from the TEB (GS base is not in CONTEXT; use
                 * a conservative window above RSP instead). */
                uintptr_t lo = (uintptr_t)ctx.Rsp, hi = lo + 8u * 1024 * 1024;
                MEMORY_BASIC_INFORMATION mbi;
                (void)tib;
                if (VirtualQuery((LPCVOID)ctx.Rsp, &mbi, sizeof mbi))
                    hi = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                n = ps_walk(&ctx, pcs, lo, hi);
                ResumeThread(th->h);
                if (n > 0) { ps_add((uint16_t)t, pcs, n); th->samples_run++; }
            } else {
                ResumeThread(th->h);
            }
        }
        next += period_ticks;
        QueryPerformanceCounter(&qn);
        if ((int64_t)(next - (uint64_t)qn.QuadPart) > 0) {
            int64_t ms = (int64_t)((next - (uint64_t)qn.QuadPart) * 1000 / (uint64_t)qf.QuadPart);
            if (ms > 0) Sleep((DWORD)ms);
            else SwitchToThread();
        } else {
            next = (uint64_t)qn.QuadPart;
        }
    }
    for (t = 0; t < s_nthr; t++)
        QueryThreadCycleTime(s_thr[t].h, &s_thr[t].cyc_end);
    QueryPerformanceCounter(&qn);
    window_s = (double)(qn.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    ps_report(window_s);
    timeEndPeriod(1);
    return 0;
}

void perf_sampler_start(void)
{
    const char *p = getenv("RECOMP_PROF");
    const char *e;
    if (!p || !*p) return;
    strncpy(s_path, p, sizeof s_path - 1);
    if ((e = getenv("RECOMP_PROF_HZ")) && atoi(e) > 0) s_hz = (unsigned)atoi(e);
    if ((e = getenv("RECOMP_PROF_SECS")) && atoi(e) > 0) s_secs = (unsigned)atoi(e);
    if ((e = getenv("RECOMP_PROF_START_S"))) s_start_s = (unsigned)atoi(e);
    if ((e = getenv("RECOMP_PROF_TRIGGER")) && *e) strncpy(s_trigger, e, sizeof s_trigger - 1);
    ps_image_init();
    s_stack_cap = 1u << 19;
    s_stacks = (PsStack *)VirtualAlloc(NULL, (SIZE_T)s_stack_cap * sizeof *s_stacks,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_index = (uint32_t *)calloc((size_t)s_stack_cap * 2, sizeof *s_index);
    if (!s_stacks || !s_index) {
        fprintf(stderr, "[PROF] cannot allocate sample storage\n");
        return;
    }
    CloseHandle(CreateThread(NULL, 0, ps_thread, NULL, 0, NULL));
    fprintf(stderr, "[PROF] sampler armed -> %s (%u pdata entries)\n", s_path, s_npdata);
}
