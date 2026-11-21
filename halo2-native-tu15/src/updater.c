/*
 * Halo2 Updater.exe -- installs a newer release, started by the F1 menu or
 * Config.exe (update_check.c's upd_launch) from data\Update\ (SaveData\Update\
 * in a development tree):
 *
 *   "Halo2 Updater.exe" --pid N --root DIR --version V --url URL [--relaunch PATH]
 *
 * It waits for the program that started it to close, downloads the release
 * zip (a GitHub release asset only), unpacks it with Windows' own tar.exe,
 * copies it over the install (the data folder and game files are never
 * touched: saves, settings, key map and console identity stay), and starts
 * PATH again. A small window
 * in the menus' style shows the progress; on a failure it says so and the
 * install is left to be updated by hand.
 */
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_ui.h"

#define WM_APP_STATUS (WM_APP + 1)

static char s_root[MAX_PATH], s_url[1024], s_relaunch[MAX_PATH], s_version[32];
static DWORD s_pid;
static HWND s_win;
static volatile LONG s_pct = -1;            /* download progress, -1 none */
static char s_msg[256] = "Waiting for Halo 2 to close...";
static volatile LONG s_failed, s_done;
static RECT s_close_r;

static void status(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    if (s_win) PostMessageA(s_win, WM_APP_STATUS, 0, 0);
}

static int fail(const char *m)
{
    status(m);
    InterlockedExchange(&s_failed, 1);
    return 0;
}

static int run_hidden(const char *cmd, const char *dir, DWORD *code)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[4096];
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    snprintf(buf, sizeof buf, "%s", cmd);
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static int download(const char *url, const char *path)
{
    wchar_t wurl[1024], host[256], obj[1024];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status_code = 0, sl = sizeof status_code, total = 0, tl = sizeof total, n, got;
    unsigned long long have = 0;
    static char chunk[64 * 1024];
    FILE *f = NULL;
    int ok = 0;

    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = obj; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return 0;
    ses = WinHttpOpen(L"Halo2Native-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto done;
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
    if (!(con = WinHttpConnect(ses, host, uc.nPort, 0))) goto done;
    if (!(req = WinHttpOpenRequest(con, L"GET", obj, NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))) goto done;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) goto done;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status_code != 200) goto done;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &total, &tl, WINHTTP_NO_HEADER_INDEX);
    if (fopen_s(&f, path, "wb") || !f) goto done;
    for (;;) {
        if (!WinHttpQueryDataAvailable(req, &n)) goto done;
        if (!n) break;
        if (n > sizeof chunk) n = sizeof chunk;
        if (!WinHttpReadData(req, chunk, n, &got) || !got) goto done;
        if (fwrite(chunk, 1, got, f) != got) goto done;
        have += got;
        if (total) InterlockedExchange(&s_pct, (LONG)(have * 100 / total));
        if (s_win) PostMessageA(s_win, WM_APP_STATUS, 0, 0);
    }
    ok = have > 0 && (!total || have == total);
done:
    if (f) fclose(f);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

/* An install (halo2_recomp.exe at the top) or a development tree (in build-ninja\). */
static int dev_tree(const char *dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\build-ninja\\halo2_recomp.exe", dir);
    return exists(p);
}

static int is_install(const char *dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\halo2_recomp.exe", dir);
    return exists(p) || dev_tree(dir);
}

/* The unpacked folder that holds the install: the zip's one top folder, or the zip itself. */
static int find_top(const char *x, char *out, size_t n)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MAX_PATH];
    if (is_install(x)) { snprintf(out, n, "%s", x); return 1; }
    snprintf(pat, sizeof pat, "%s\\*", x);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            snprintf(out, n, "%s\\%s", x, fd.cFileName);
            if (is_install(out)) { FindClose(h); return 1; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

static DWORD WINAPI work(LPVOID unused)
{
    char dir[MAX_PATH], zip[MAX_PATH], x[MAX_PATH], top[MAX_PATH], cmd[4096], tar[MAX_PATH], sys[MAX_PATH];
    DWORD code = 1;
    HANDLE p;
    (void)unused;

    /* the program that started this */
    if (s_pid && (p = OpenProcess(SYNCHRONIZE, FALSE, s_pid)) != NULL) {
        WaitForSingleObject(p, 60000);
        CloseHandle(p);
    }
    Sleep(1500);                                 /* its helpers close too */

    snprintf(dir, sizeof dir, "%s\\%s\\Update", s_root, dev_tree(s_root) ? "SaveData" : "data");
    snprintf(zip, sizeof zip, "%s\\update.zip", dir);
    snprintf(x, sizeof x, "%s\\unpacked", dir);
    snprintf(cmd, sizeof cmd, "Downloading version %s...", s_version);
    status(cmd);
    if (!download(s_url, zip)) return fail("The download failed. Check your connection and try again later.");

    InterlockedExchange(&s_pct, -1);
    status("Unpacking...");
    GetSystemDirectoryA(sys, sizeof sys);
    snprintf(tar, sizeof tar, "%s\\tar.exe", sys);
    snprintf(cmd, sizeof cmd, "cmd.exe /c rd /s /q \"%s\"", x);
    run_hidden(cmd, dir, &code);
    CreateDirectoryA(x, NULL);
    snprintf(cmd, sizeof cmd, "\"%s\" -xf \"%s\" -C \"%s\"", tar, zip, x);
    if (!run_hidden(cmd, dir, &code) || code != 0) return fail("The update could not be unpacked.");
    if (!find_top(x, top, sizeof top)) return fail("The download is not a Halo 2 Native update.");

    status("Installing...");
    /* the data folder (saves, settings, key map, identity, logs) and the game's
     * own files are never copied over */
    snprintf(cmd, sizeof cmd, "robocopy \"%s\" \"%s\" /E /XD data SaveData game_files game /R:20 /W:1 /NFL /NDL /NJH /NJS /NP",
             top, s_root);
    if (!run_hidden(cmd, s_root, &code) || code >= 8)
        return fail("Some files could not be replaced. Close Halo 2 and Config.exe and try again.");

    snprintf(cmd, sizeof cmd, "cmd.exe /c rd /s /q \"%s\"", x);
    run_hidden(cmd, dir, &code);
    DeleteFileA(zip);
    snprintf(cmd, sizeof cmd, "Updated to version %s.", s_version);
    status(cmd);
    if (s_relaunch[0])
        ShellExecuteA(NULL, "open", s_relaunch, NULL, s_root, SW_SHOWNORMAL);
    Sleep(1200);
    InterlockedExchange(&s_done, 1);
    if (s_win) PostMessageA(s_win, WM_CLOSE, 0, 0);
    return 0;
}

static void draw(HDC dc, int w, int h)
{
    UiFonts f;
    RECT r, band, msg, bar, fill;
    ui_fonts_make(&f, 1.0f);
    SetRect(&r, 0, 0, w, h);
    ui_background(dc, &r);
    SetRect(&band, 12, 12, w - 12, 72);
    ui_header(dc, &f, &band, "UPDATING HALO 2", NULL, 0, 0, 0, 0, NULL);
    SetRect(&msg, 28, 88, w - 28, 120);
    ui_text(dc, f.body, s_failed ? RGB(247, 148, 29) : RGB(242, 246, 251), &msg, s_msg,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetRect(&bar, 28, 130, w - 28, 152);
    ui_chamfer(dc, &bar, 6, RGB(6, 24, 54), RGB(38, 92, 156));
    if (s_pct >= 0 || s_done) {
        int p = s_done ? 100 : s_pct;
        SetRect(&fill, bar.left + 3, bar.top + 3, bar.left + 3 + (bar.right - bar.left - 6) * p / 100, bar.bottom - 3);
        if (fill.right > fill.left) {
            HBRUSH b = CreateSolidBrush(RGB(130, 206, 255));
            FillRect(dc, &fill, b);
            DeleteObject(b);
        }
    }
    SetRect(&s_close_r, 0, 0, 0, 0);
    if (s_failed) {
        SetRect(&s_close_r, w - 28 - 120, h - 52, w - 28, h - 16);
        ui_button(dc, &f, &s_close_r, "CLOSE", 1, 0);
    }
    ui_fonts_free(&f);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT cr;
        HDC dc = BeginPaint(h, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP bm;
        HGDIOBJ old;
        GetClientRect(h, &cr);
        bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        old = SelectObject(mem, bm);
        draw(mem, cr.right, cr.bottom);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_APP_STATUS: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT p;
        p.x = (short)LOWORD(l); p.y = (short)HIWORD(l);
        if (s_failed && PtInRect(&s_close_r, p)) DestroyWindow(h);
        return 0;
    }
    case WM_CLOSE:
        if (s_done || s_failed) DestroyWindow(h);       /* not half way through */
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static int arg(const char *name, char *out, size_t n)
{
    int i;
    for (i = 1; i + 1 < __argc; i++)
        if (!strcmp(__argv[i], name)) { snprintf(out, n, "%s", __argv[i + 1]); return 1; }
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r = { 0, 0, 520, 230 };
    char pid[32] = "";
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    (void)prev; (void)cmdline;

    arg("--root", s_root, sizeof s_root);
    arg("--url", s_url, sizeof s_url);
    arg("--version", s_version, sizeof s_version);
    arg("--relaunch", s_relaunch, sizeof s_relaunch);
    arg("--pid", pid, sizeof pid);
    s_pid = (DWORD)strtoul(pid, NULL, 10);
    /* only a GitHub release asset, over HTTPS, into an install folder */
    if (!s_root[0] || !is_install(s_root) || strncmp(s_url, "https://github.com/", 19) ||
        !strstr(s_url, "/releases/download/")) {
        MessageBoxA(NULL, "This updater is started by Halo 2's F1 menu or Config.exe.", "Halo 2 Updater", MB_ICONINFORMATION);
        return 2;
    }
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "Halo2NativeUpdater";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, style, FALSE);
    s_win = CreateWindowExA(0, wc.lpszClassName, "Halo 2 Native - Update", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(s_win, show);
    CloseHandle(CreateThread(NULL, 0, work, NULL, 0, NULL));
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_failed ? 1 : 0;
}
