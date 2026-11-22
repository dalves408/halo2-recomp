/*
 * h2_frame.c -- one game window for the whole session.
 *
 * Adapted from CounterStrikeXB's cs_frame.c (same author).
 *
 * Halo 2 hands over to its disc's content downloader (Downloader.xbe) and the
 * downloader back to the game with XLaunchNewImage, a quick reboot that is
 * modelled as a fresh process (kernel_bridge.c, RECOMP_LAUNCH_MAP): guest
 * memory, threads and devices start clean, as on the console. Each process
 * used to open its own window, so the player saw the window close and another
 * open at every hand-over.
 *
 * Now the program the player starts is only the frame: it opens the window
 * (title, fullscreen, the taskbar button), starts the game as a second copy of
 * itself with H2_FRAME_HWND=<this window>, and stays until the last process
 * has ended. The game's window (the toolkit's, RECOMP_FB_PARENT, set by main.c
 * from H2_FRAME_HWND) is a child filling the frame; the downloader and every
 * later process inherit H2_FRAME_HWND and draw into the same frame. Between two
 * processes the frame shows "Loading...".
 *
 * The frame:
 *   - keeps the child the size of its client area and gives it the keyboard;
 *   - hides the mouse pointer over the game;
 *   - does fullscreen (F11 / Alt+Enter and the F1 menu in the game ask it with
 *     the toolkit's WM_APP + 0x46; settings.ini's fullscreen at start);
 *   - on close, asks the game window to close (its own exit path writes the end
 *     of the log), then ends whatever is left;
 *   - holds every process in a job (closing the frame, or the frame being
 *     killed, ends the game) and returns the last process's exit code, which is
 *     what a launcher or Recover-Insignia-Account.ps1 sees.
 *
 * H2_NO_FRAME=1 runs the game as one top-level window per process, as before.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h2_frame.h"
#include "settings.h"

#define WM_FB_TOGGLE_FULLSCREEN (WM_APP + 0x46)   /* the toolkit's (fb_present.c) */
#define WM_FRAME_CHAIN_DONE     (WM_APP + 0x47)
#define TIMER_WATCH 1
#define TIMER_CLOSE 2

static HWND s_frame;
static float s_frame_aspect = 4.0f / 3.0f;   /* the picture shape the frame opens at */
static HANDLE s_job, s_port;
static volatile LONG s_exit_code;
static int s_full, s_closing, s_children = -1;
static RECT s_saved;
static LONG_PTR s_saved_style;

static BOOL CALLBACK first_child(HWND h, LPARAM l)
{
    if (GetParent(h) == s_frame) { *(HWND *)l = h; return FALSE; }
    return TRUE;
}

static HWND game_window(void)
{
    HWND h = NULL;
    EnumChildWindows(s_frame, first_child, (LPARAM)&h);
    return h;
}

static void fit_child(void)
{
    RECT c;
    HWND g = game_window();
    if (!g) return;
    GetClientRect(s_frame, &c);
    SetWindowPos(g, NULL, 0, 0, c.right, c.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void toggle_fullscreen(void)
{
    if (!s_full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(s_frame, MONITOR_DEFAULTTONEAREST), &mi)) return;
        GetWindowRect(s_frame, &s_saved);
        s_saved_style = GetWindowLongPtrA(s_frame, GWL_STYLE);
        SetWindowLongPtrA(s_frame, GWL_STYLE, (s_saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(s_frame, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        s_full = 1;
    } else {
        SetWindowLongPtrA(s_frame, GWL_STYLE, s_saved_style);
        SetWindowPos(s_frame, NULL, s_saved.left, s_saved.top, s_saved.right - s_saved.left,
                     s_saved.bottom - s_saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        s_full = 0;
    }
    fit_child();
}

/* F11, Alt+Enter and the F1 menu: the choice is kept in settings.ini. */
static void keep_fullscreen(void)
{
    Halo2Settings set;
    memset(&set, 0, sizeof set);
    halo2_settings_load(&set);
    if ((set.fullscreen != 0) != (s_full != 0)) {
        set.fullscreen = s_full;
        halo2_settings_save(&set);
    }
    h2_diag_video(s_full ? "fullscreen on" : "fullscreen off");
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    RECT c;
    HDC dc = BeginPaint(h, &ps);
    GetClientRect(h, &c);
    FillRect(dc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (!game_window()) {          /* between two processes: the next one is starting */
        int px = c.bottom / 24 < 14 ? 14 : c.bottom / 24;
        HFONT f = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              "Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 190));
        DrawTextA(dc, "Loading...", -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(f);
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK frame_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_FB_TOGGLE_FULLSCREEN:
        toggle_fullscreen();
        keep_fullscreen();
        return 0;
    case WM_KEYDOWN:            /* only while no game window has the keyboard */
        if (w == VK_F11 && !(l & (1 << 30))) { toggle_fullscreen(); keep_fullscreen(); }
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN) {
            if ((l & (1 << 29)) && !(l & (1 << 30))) { toggle_fullscreen(); keep_fullscreen(); }
            return 0;
        }
        break;
    case WM_SIZE:
        fit_child();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) {
            HWND g = game_window();
            if (g) SetFocus(g);
            return 0;
        }
        break;
    case WM_SETFOCUS: {
        HWND g = game_window();
        if (g) SetFocus(g);
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_WATCH) {        /* a process came or went */
            HWND g = game_window();
            int n = g != NULL;
            if (n != s_children) {
                s_children = n;
                if (g) {
                    fit_child();
                    if (GetForegroundWindow() == h) SetFocus(g);
                }
                InvalidateRect(h, NULL, TRUE);
            }
        } else if (w == TIMER_CLOSE) { /* the game did not close by itself */
            KillTimer(h, TIMER_CLOSE);
            TerminateJobObject(s_job, 0);
        }
        return 0;
    case WM_SETCURSOR:
        /* No mouse pointer over the game, as on the console. The game's
         * window is our child: Windows asks the parent first, and TRUE here
         * keeps its arrow from being set. Title bar and borders keep theirs. */
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_CLOSE: {
        HWND g = game_window();
        if (s_closing++) return 0;
        if (g) {
            PostMessageA(g, WM_CLOSE, 0, 0);     /* the game's own exit: the end of its log is written */
            SetTimer(h, TIMER_CLOSE, 3000, NULL);
        } else {
            TerminateJobObject(s_job, 0);
        }
        return 0;
    }
    case WM_FRAME_CHAIN_DONE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The job's messages: every process, its exit code, and the end. */
static DWORD WINAPI watch_job(LPVOID unused)
{
    DWORD msg;
    ULONG_PTR key;
    LPOVERLAPPED ov;
    (void)unused;
    while (GetQueuedCompletionStatus(s_port, &msg, &key, &ov, INFINITE)) {
        DWORD pid = (DWORD)(ULONG_PTR)ov;
        if (msg == JOB_OBJECT_MSG_EXIT_PROCESS || msg == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) {
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            DWORD code;
            if (p) {
                if (GetExitCodeProcess(p, &code)) InterlockedExchange(&s_exit_code, (LONG)code);
                CloseHandle(p);
            }
        } else if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
            PostMessageA(s_frame, WM_FRAME_CHAIN_DONE, 0, 0);
            break;
        }
    }
    return 0;
}

static int start_game(void)
{
    char exe[MAX_PATH], hv[32];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp;
    BOOL ok;

    s_job = CreateJobObjectA(NULL, NULL);
    s_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!s_job || !s_port) return 0;
    memset(&lim, 0, sizeof lim);
    /* BREAKAWAY_OK: the updater the F1 menu starts leaves the job (update_check.c), so the frame can close and
     * its exe be replaced. */
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(s_job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    acp.CompletionKey = s_job;
    acp.CompletionPort = s_port;
    SetInformationJobObject(s_job, JobObjectAssociateCompletionPortInformation, &acp, sizeof acp);

    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) return 0;
    snprintf(hv, sizeof hv, "%llu", (unsigned long long)(ULONG_PTR)s_frame);
    SetEnvironmentVariableA("H2_FRAME_HWND", hv);      /* inherited by every hand-over too */
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    ok = CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("H2_FRAME_HWND", NULL);
    if (!ok) return 0;
    if (!AssignProcessToJobObject(s_job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(CreateThread(NULL, 0, watch_job, NULL, 0, NULL));
    return 1;
}

int h2_frame_main(int *exit_code)
{
    const char *no = getenv("H2_NO_FRAME");
    WNDCLASSA wc;
    RECT r, work;
    MSG msg;
    Halo2Settings set;
    HINSTANCE inst = GetModuleHandleA(NULL);
    int scale;

    if (getenv("H2_FRAME_HWND") || (no && *no && *no != '0'))
        return 0;

    /* The window the toolkit would have made: the 640x480 frame at 2x, smaller
     * until it fits the work area, centred (fb_present.c, RECOMP_WINDOW_SCALE). */
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = frame_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));     /* res/halo2.ico */
    if (!wc.hIcon) wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.lpszClassName = "Halo2NativeFrame";
    RegisterClassA(&wc);
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
    }
    {   /* the picture's shape: 4:3, or the widescreen setting's (settings.c) */
        Halo2Settings set;
        halo2_settings_load(&set);
        s_frame_aspect = halo2_widescreen_aspect(set.widescreen);
    }
    for (scale = 2;; scale--) {
        r.left = r.top = 0; r.bottom = 480 * scale;
        r.right = (LONG)(480.0f * scale * s_frame_aspect + 0.5f);
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (scale <= 1 || (r.right - r.left <= work.right - work.left && r.bottom - r.top <= work.bottom - work.top))
            break;
    }
    s_frame = CreateWindowExA(0, wc.lpszClassName, "Halo 2", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                              work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!s_frame)
        return 0;                      /* no frame: the game makes its own window */
    memset(&set, 0, sizeof set);
    halo2_settings_load(&set);
    ShowWindow(s_frame, SW_SHOWNORMAL);
    if (h2_start_fullscreen(set.fullscreen)) toggle_fullscreen();
    UpdateWindow(s_frame);
    if (!start_game()) {
        MessageBoxA(s_frame, "The game could not be started.", "Halo 2", MB_ICONERROR);
        *exit_code = 1;
        return 1;
    }
    SetTimer(s_frame, TIMER_WATCH, 100, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    *exit_code = (int)s_exit_code;
    return 1;
}
