/* h2_diag.c - what a player's problem report needs, in every build, at no cost while the game plays
 * (after Marvel vs. Capcom 2's mvc2_diag.c).
 *
 *   log file   the process the player starts copies everything it and its game and downloader processes
 *              print to <data>\logs\halo2-<date>-<time>.log and passes it on to the original stdout/stderr
 *              (a launch script's file, or RecompLauncher's pipe). The last three runs are kept, a log stops
 *              taking routine lines at 20 MB, and the Windows user name is replaced by %USERPROFILE%/%USERNAME%.
 *   [BUILD]    one line at start: version, symbol-file id, Windows, CPU, GPU, memory.
 *   [VIDEO]    the video settings in effect: once when the game is up, when the F1 menu or F11 changes one,
 *              and in every freeze or crash report.
 *   [FREEZE]   no frame for 10 s while no menu is open: every thread's call stack as module+offset; again 20 s
 *              later; a line when frames move again. scripts/symbolize_log.py names the offsets with the
 *              release's kept .pdb (out\symbols\<version>); generated functions are named after their guest
 *              address (sub_XXXXXXXX), so a stack is the game's own call chain.
 *   [CRASH]    a fatal exception nothing handled: code, where, call stack.
 *
 * Thread stacks are taken by suspending one thread at a time and unwinding with RtlVirtualUnwind (no heap, no
 * dbghelp while it is suspended). Output goes through WriteFile, never the C runtime's stream lock, which a stuck
 * thread may hold. */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#include "h2_layout.h"
#include "settings.h"

extern volatile LONG g_gpu_present_count;      /* toolkit gpu_present.c */
extern int halo2_overlay_active(void);

#define LOG_KEEP 3
#define LOG_CAP  (20u << 20)
#define MAX_FRAMES 24
#define FREEZE_SECS 10

static void put(const char *s) { DWORD n; WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)strlen(s), &n, NULL); }

static void putf(const char *fmt, ...)
{
    char line[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    put(line);
}

/* ---- where an address is ------------------------------------------------------------------------------------ */
static void where(const void *addr, char *out, size_t n)
{
    HMODULE mod = NULL;
    char path[MAX_PATH], *name;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod) || !GetModuleFileNameA(mod, path, sizeof path)) {
        snprintf(out, n, "0x%llX", (unsigned long long)(uintptr_t)addr);
        return;
    }
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    if (strchr(name, '.')) *strrchr(name, '.') = 0;
    snprintf(out, n, "%s+0x%llX", name, (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
}

static int unwind(CONTEXT *ctx, DWORD64 *pcs, int max)
{
    int d = 0;
    while (d < max && ctx->Rip) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f;
        pcs[d++] = ctx->Rip;
        f = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
        if (!f) {
            __try { ctx->Rip = *(const DWORD64 *)ctx->Rsp; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            ctx->Rsp += 8;
        } else {
            PVOID hd = NULL;
            DWORD64 ef = 0;
            __try { RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, f, ctx, &hd, &ef, NULL); }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        }
    }
    return d;
}

static void put_stack(const char *tag, DWORD tid, const DWORD64 *pcs, int n)
{
    char line[4096], w[200];
    int len = snprintf(line, sizeof line, "%s thread %lu:", tag, (unsigned long)tid), i;
    for (i = 0; i < n && len < (int)sizeof line - 220; i++) {
        where((const void *)(uintptr_t)pcs[i], w, sizeof w);
        len += snprintf(line + len, sizeof line - len, "%s %s", i ? " <" : "", w);
    }
    snprintf(line + len, sizeof line - len, "\n");
    put(line);
}

/* ---- the log file ------------------------------------------------------------------------------------------- */
static HANDLE s_file = INVALID_HANDLE_VALUE, s_orig, s_rd, s_wr;
static DWORD s_tee_tid;
static volatile LONG s_tee_busy;
static unsigned long long s_file_bytes;
static int s_capped;
static char s_profile[MAX_PATH], s_user[128];
static size_t s_profile_len, s_user_len;

static int important(const char *s, size_t n)
{
    static const char *const keys[] = { "[CRASH]", "[FREEZE]", "[BUILD]", "ERROR", "[FATAL]", "[LOG]", "[VIDEO]" };
    size_t i, k;
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
        for (k = 0; k + strlen(keys[i]) <= n; k++)
            if (!memcmp(s + k, keys[i], strlen(keys[i]))) return 1;
    return 0;
}

static size_t scrub(const char *in, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n && o + 16 < cap) {
        if (s_profile_len && i + s_profile_len <= n && !_strnicmp(in + i, s_profile, s_profile_len)) {
            memcpy(out + o, "%USERPROFILE%", 13); o += 13; i += s_profile_len; continue;
        }
        if (s_user_len && (in[i] == '\\' || in[i] == '/') && i + 1 + s_user_len < n &&
            !_strnicmp(in + i + 1, s_user, s_user_len) && (in[i + 1 + s_user_len] == '\\' || in[i + 1 + s_user_len] == '/')) {
            out[o++] = in[i];
            memcpy(out + o, "%USERNAME%", 10); o += 10; i += 1 + s_user_len; continue;
        }
        out[o++] = in[i++];
    }
    return o;
}

static void emit(const char *line, size_t n)
{
    static char clean[8192 + 64];
    DWORD w;
    size_t m = scrub(line, n, clean, sizeof clean);
    if (s_orig && !WriteFile(s_orig, clean, (DWORD)m, &w, NULL))
        s_orig = NULL;
    if (s_file == INVALID_HANDLE_VALUE)
        return;
    if (s_file_bytes > LOG_CAP && !important(clean, m)) {
        if (!s_capped) {
            static const char note[] = "[LOG] this log reached 20 MB: from here on only reports and errors are kept\n";
            s_capped = 1;
            WriteFile(s_file, note, sizeof note - 1, &w, NULL);
        }
        return;
    }
    if (WriteFile(s_file, clean, (DWORD)m, &w, NULL))
        s_file_bytes += w;
}

static DWORD WINAPI tee_thread(LPVOID p)
{
    static char buf[65536], line[8192];
    size_t ln = 0;
    DWORD idle = 0;
    (void)p;
    for (;;) {
        DWORD avail = 0, got = 0, i;
        if (!PeekNamedPipe(s_rd, NULL, 0, NULL, &avail, NULL))
            return 0;
        if (!avail) {
            if (ln && ++idle >= 10) { emit(line, ln); ln = 0; }
            if (!ln) InterlockedExchange(&s_tee_busy, 0);
            Sleep(20);
            continue;
        }
        idle = 0;
        InterlockedExchange(&s_tee_busy, 1);
        if (!ReadFile(s_rd, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL))
            return 0;
        for (i = 0; i < got; i++) {
            line[ln++] = buf[i];
            if (buf[i] == '\n' || ln == sizeof line) { emit(line, ln); ln = 0; }
        }
    }
}

static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void prune_logs(const char *dir)
{
    static char names[64][MAX_PATH];
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;
    snprintf(pat, sizeof pat, "%s\\halo2-*.log", dir);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (n < 64) snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, n, MAX_PATH, by_name);
    for (i = 0; i < n - (LOG_KEEP - 1); i++) {
        snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        DeleteFileA(path);
    }
}

static void flush_log(void)
{
    int i;
    fflush(stdout);
    fflush(stderr);
    if (!s_rd) return;
    for (i = 0; i < 100; i++) {
        DWORD avail = 0;
        if (!PeekNamedPipe(s_rd, NULL, 0, NULL, &avail, NULL) || (!avail && !s_tee_busy))
            break;
        Sleep(10);
    }
    if (s_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(s_file);
}

/* ---- [BUILD] ------------------------------------------------------------------------------------------------ */
static void build_line(void)
{
    char ver[32] = "dev", pdb[64] = "none", cpu[96] = "?", gpu[128] = "?", os[64] = "?", disp[16] = "", exe[MAX_PATH];
    const char *name;
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    MEMORYSTATUSEX ms;
    DISPLAY_DEVICEA dev;
    HRSRC r;
    DWORD i, sz;

    if ((r = FindResourceA(NULL, MAKEINTRESOURCEA(1), RT_VERSION)) != NULL) {
        const BYTE *v = (const BYTE *)LockResource(LoadResource(NULL, r));
        DWORD n = SizeofResource(NULL, r), k;
        for (k = 0; v && k + 52 <= n; k += 4)
            if (*(const DWORD *)(v + k) == 0xFEEF04BDu) {
                const VS_FIXEDFILEINFO *f = (const VS_FIXEDFILEINFO *)(v + k);
                snprintf(ver, sizeof ver, "%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS),
                         HIWORD(f->dwFileVersionLS));
                break;
            }
    }
    for (i = 0; dd.VirtualAddress && i < dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i++) {
        const IMAGE_DEBUG_DIRECTORY *d = (const IMAGE_DEBUG_DIRECTORY *)(base + dd.VirtualAddress) + i;
        if (d->Type == IMAGE_DEBUG_TYPE_CODEVIEW && d->AddressOfRawData) {
            const BYTE *cv = base + d->AddressOfRawData;
            if (*(const DWORD *)cv == 0x53445352u) {
                const GUID *g = (const GUID *)(cv + 4);
                snprintf(pdb, sizeof pdb, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX", g->Data1, g->Data2,
                         g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
                         g->Data4[6], g->Data4[7], *(const DWORD *)(cv + 20));
            }
        }
    }
    { HKEY k;
      if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k)) {
          sz = sizeof cpu; RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (BYTE *)cpu, &sz); RegCloseKey(k);
          for (sz = (DWORD)strlen(cpu); sz && cpu[sz - 1] == ' '; ) cpu[--sz] = 0; }
      if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k)) {
          char build[16] = "?", pn[64] = "Windows"; DWORD ubr = 0;
          sz = sizeof build; RegQueryValueExA(k, "CurrentBuild", NULL, NULL, (BYTE *)build, &sz);
          sz = sizeof pn; RegQueryValueExA(k, "ProductName", NULL, NULL, (BYTE *)pn, &sz);
          sz = sizeof disp; RegQueryValueExA(k, "DisplayVersion", NULL, NULL, (BYTE *)disp, &sz);
          sz = sizeof ubr; RegQueryValueExA(k, "UBR", NULL, NULL, (BYTE *)&ubr, &sz);
          if (atoi(build) >= 22000 && !strncmp(pn, "Windows 10", 10)) pn[9] = '1';
          snprintf(os, sizeof os, "%s %s (%s.%lu)", pn, disp, build, (unsigned long)ubr);
          RegCloseKey(k); } }
    memset(&dev, 0, sizeof dev);
    dev.cb = sizeof dev;
    for (i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++)
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { snprintf(gpu, sizeof gpu, "%s", dev.DeviceString); break; }
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    GetModuleFileNameA(NULL, exe, sizeof exe);
    name = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
    { SYSTEM_INFO si; GetSystemInfo(&si);
      putf("[BUILD] %s %s, symbols %s | %s | %s, %lu logical CPUs | display %s | %llu GB\n", name, ver, pdb, os, cpu,
           (unsigned long)si.dwNumberOfProcessors, gpu, (unsigned long long)((ms.ullTotalPhys + (1ull << 29)) >> 30)); }
}

/* ---- [VIDEO] ------------------------------------------------------------------------------------------------ */
void h2_diag_video(const char *why)
{
    static const char *const wide[4] = { "off", "auto", "16:9", "16:10" };
    Halo2Settings s;
    SYSTEMTIME st;
    DEVMODEA dm;
    memset(&s, 0, sizeof s);
    halo2_settings_load(&s);
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    GetLocalTime(&st);
    putf("[VIDEO] %02d:%02d:%02d %s: scale %dx, fullscreen %d, vsync %d, anisotropy %dx, fxaa %d, msaa %dx, "
         "widescreen %s, mouse aim %d | display %lux%lu at %lu Hz\n",
         st.wHour, st.wMinute, st.wSecond, why, s.resolution_scale, s.fullscreen, s.vsync, s.anisotropy, s.fxaa, s.msaa,
         wide[s.widescreen & 3], s.mouse_aim, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
}

/* ---- [FREEZE] ----------------------------------------------------------------------------------------------- */
static ULONGLONG s_t0;
static DWORD s_watch_tid;

static void dump_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    BOOL ok;
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    for (ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        DWORD64 pcs[MAX_FRAMES];
        CONTEXT ctx;
        HANDLE th;
        int n = 0;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || te.th32ThreadID == s_tee_tid) continue;
        th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx))
                n = unwind(&ctx, pcs, MAX_FRAMES);
            ResumeThread(th);
        }
        CloseHandle(th);
        if (n) put_stack("[FREEZE]", te.th32ThreadID, pcs, n);
    }
    CloseHandle(snap);
}

static void freeze_report(unsigned secs, LONG frames)
{
    putf("[FREEZE] no new frame for %u s (frames so far %ld, %lu s since start)\n", secs, (long)frames,
         (unsigned long)((GetTickCount64() - s_t0) / 1000));
    build_line();
    h2_diag_video("at the freeze");
    dump_threads();
    put("[FREEZE] end of report\n");
    flush_log();
}

static DWORD WINAPI watch_thread(LPVOID p)
{
    LONG last = g_gpu_present_count;
    unsigned still = 0, reports = 0, total = 0;
    int video_logged = 0;
    (void)p;
    for (;;) {
        LONG now;
        Sleep(1000);
        now = g_gpu_present_count;
        if (now && !video_logged) { h2_diag_video("start"); video_logged = 1; }
        if (now != last || now == 0 || halo2_overlay_active()) {
            if (reports && now != last) putf("[FREEZE] frames are moving again after %u s\n", still);
            last = now; still = 0; reports = 0;
            continue;
        }
        still++;
        if ((still == FREEZE_SECS || still == FREEZE_SECS + 20) && total < 6) {
            freeze_report(still, now);
            reports++; total++;
        }
    }
}

/* ---- [CRASH] ------------------------------------------------------------------------------------------------ */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    static volatile LONG once;
    DWORD64 pcs[MAX_FRAMES];
    CONTEXT ctx;
    char w[200];
    int n;
    if (InterlockedExchange(&once, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    where(ep->ExceptionRecord->ExceptionAddress, w, sizeof w);
    putf("[CRASH] unhandled exception 0x%08lX at %s\n", (unsigned long)ep->ExceptionRecord->ExceptionCode, w);
    ctx = *ep->ContextRecord;
    n = unwind(&ctx, pcs, MAX_FRAMES);
    put_stack("[CRASH]", GetCurrentThreadId(), pcs, n);
    build_line();
    h2_diag_video("at the crash");
    flush_log();
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The process the player starts takes the log (its game and downloader processes inherit the pipe); every
 * process gets the crash report, and the game process the freeze watch. */
void h2_diag_start_log(void)
{
    char root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    SYSTEMTIME st;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    int fd;

    s_t0 = GetTickCount64();
    SetUnhandledExceptionFilter(crash_filter);
    if (getenv("H2_FRAME_HWND")) {           /* a game or downloader process: the log is the frame's */
#ifndef H2_DOWNLOADER
        CloseHandle(CreateThread(NULL, 0, watch_thread, NULL, 0, &s_watch_tid));
#endif
        build_line();
        return;
    }
    if (!GetEnvironmentVariableA("USERPROFILE", s_profile, sizeof s_profile)) s_profile[0] = 0;
    if (!GetEnvironmentVariableA("USERNAME", s_user, sizeof s_user)) s_user[0] = 0;
    s_profile_len = strlen(s_profile);
    s_user_len = strlen(s_user);
    if (s_profile_len < 4) s_profile_len = 0;
    if (s_user_len < 2) s_user_len = 0;

    halo2_install_root(root, sizeof root);
    h2_data_path(root, "", dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    h2_data_path(root, "logs", dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    prune_logs(dir);
    GetLocalTime(&st);
    snprintf(path, sizeof path, "%s\\halo2-%04d%02d%02d-%02d%02d%02d.log", dir, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    s_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);

    s_orig = GetStdHandle(STD_ERROR_HANDLE);
    if (s_orig == INVALID_HANDLE_VALUE || !s_orig || GetFileType(s_orig) == FILE_TYPE_UNKNOWN)
        s_orig = GetStdHandle(STD_OUTPUT_HANDLE);
    if (s_orig == INVALID_HANDLE_VALUE || !s_orig || GetFileType(s_orig) == FILE_TYPE_UNKNOWN)
        s_orig = NULL;
    if (s_orig && !DuplicateHandle(GetCurrentProcess(), s_orig, GetCurrentProcess(), &s_orig, 0, FALSE,
                                   DUPLICATE_SAME_ACCESS))
        s_orig = NULL;
    /* The write end is inheritable: the game and downloader processes write into the same log. */
    if (!CreatePipe(&s_rd, &s_wr, &sa, 1 << 20)) { build_line(); return; }
    SetHandleInformation(s_rd, HANDLE_FLAG_INHERIT, 0);
    if (_fileno(stdout) < 0) freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0) freopen("NUL", "w", stderr);
    fd = _open_osfhandle((intptr_t)s_wr, _O_WRONLY | _O_BINARY);
    if (fd >= 0) {
        _dup2(fd, _fileno(stdout));
        _dup2(fd, _fileno(stderr));
    }
    SetStdHandle(STD_OUTPUT_HANDLE, s_wr);
    SetStdHandle(STD_ERROR_HANDLE, s_wr);
    CloseHandle(CreateThread(NULL, 0, tee_thread, NULL, 0, &s_tee_tid));
    atexit(flush_log);
    build_line();
}

void h2_diag_frame(void) { }
