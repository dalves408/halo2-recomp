#include "overlay.h"
#include "settings.h"
#include "mouse_binds.h"
#include "h2_mouse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xinput.h>

extern HWND xbox_FramebufferWindowHandle(void);
extern void xbox_time_pause_begin(void);
extern void xbox_time_pause_end(void);
extern volatile int g_audio_muted;

/* Existing helper in the 1.5 SDL3 pad override: pad 1 only, no keyboard merge. */
extern int hitz_PadPeek(DWORD port, WORD *buttons, BYTE *lt, BYTE *rt,
                        SHORT *lx, SHORT *ly, SHORT *rx, SHORT *ry);

enum { IT_SCALE, IT_FS, IT_VSYNC, IT_ANISO, IT_FXAA, IT_MSAA, IT_WIDE, IT_MOUSE, IT_MSENS, IT_MINV,
       IT_MB0, IT_MB_LAST = IT_MB0 + MB_COUNT - 1,
       IT_CLOSE, IT_QUIT, IT_COUNT };
enum { MODE_NONE, MODE_QUIT, MODE_SET };

static const char *const ITEM_NAME[IT_COUNT] = {
    "Render Scale", "Fullscreen", "VSync", "Anisotropic filter",
    "FXAA", "MSAA", "Widescreen (at next start)", "Mouse aim (off in Xbox Live)", "Mouse sensitivity", "Invert mouse",
    "In play: left click", "In play: right click", "In play: middle click",
    "In play: mouse button 4", "In play: mouse button 5", "In play: wheel up", "In play: wheel down",
    "Close menu", "Quit game"
};
static int s_mb[MB_COUNT];      /* mouse bindings being edited (mouse_binds.c) */

static HWND s_game, s_hwnd;
static WNDPROC s_oldproc;
static HANDLE s_thread;
static volatile LONG s_mode;
static volatile LONG s_running;
static int s_sel, s_quit_sel, s_set_armed;
static Halo2Settings s_edit;
static int s_unblock;
static WORD s_pad_prev;
static DWORD s_repeat_at;
static int s_repeat_dir;

static void pause_on(void)
{
    xbox_time_pause_begin();
    g_audio_muted = 1;
}

static void pause_off(void)
{
    g_audio_muted = 0;
    xbox_time_pause_end();
}

int halo2_overlay_active(void)
{
    return InterlockedCompareExchange(&s_mode, 0, 0) != MODE_NONE;
}

static void layout_overlay(HWND owner)
{
    RECT r;
    if (!s_hwnd || !owner || !GetClientRect(owner, &r))
        return;
    MapWindowPoints(owner, NULL, (POINT *)&r, 2);
    SetWindowPos(s_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void close_overlay(void)
{
    if (s_mode == MODE_NONE)
        return;
    InterlockedExchange(&s_mode, MODE_NONE);
    s_set_armed = 0;
    s_unblock = 1;
    if (s_hwnd)
        ShowWindow(s_hwnd, SW_HIDE);
    pause_off();
    fprintf(stderr, "[OVERLAY] close\n");
}

static void open_quit(void)
{
    if (s_mode == MODE_NONE)
        pause_on();
    InterlockedExchange(&s_mode, MODE_QUIT);
    s_quit_sel = 0;
    s_set_armed = 0;
    layout_overlay(s_game);
    fprintf(stderr, "[OVERLAY] quit prompt\n");
}

static void open_settings(void)
{
    if (s_mode == MODE_NONE)
        pause_on();
    InterlockedExchange(&s_mode, MODE_SET);
    halo2_settings_copy(&s_edit);
    {
        char root[MAX_PATH];
        halo2_install_root(root, sizeof root);
        mb_load(root, s_mb);
    }
    s_sel = 0;
    s_set_armed = 0;
    layout_overlay(s_game);
    fprintf(stderr, "[OVERLAY] settings\n");
}

static int clamp_to(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int aniso_ix(int a)
{
    return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0;
}

static int msaa_ix(int a)
{
    return a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0;
}

static const int ANISO_VAL[5] = { 1, 2, 4, 8, 16 };
static const int MSAA_VAL[4] = { 1, 2, 4, 8 };
static const char *const ANISO_TXT[5] = { "Off", "2x", "4x", "8x", "16x" };
static const char *const MSAA_TXT[4] = { "Off", "2x", "4x", "8x" };
static const char *const OFFON[2] = { "Off", "On" };
static const char *const WIDE_TXT[4] = { "Off (4:3)", "Auto (monitor)", "16:9", "16:10" };
static const int SENS_VAL[] = { 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500 };
#define SENS_N ((int)(sizeof SENS_VAL / sizeof SENS_VAL[0]))

static int sens_ix(int v)
{
    int i, best = 0;
    for (i = 1; i < SENS_N; i++)
        if (abs(SENS_VAL[i] - v) < abs(SENS_VAL[best] - v)) best = i;
    return best;
}

static void item_text(int item, char *out, size_t n)
{
    switch (item) {
    case IT_SCALE: snprintf(out, n, "%dx", clamp_to(s_edit.resolution_scale, 1, 5)); break;
    case IT_FS: snprintf(out, n, "%s", OFFON[s_edit.fullscreen ? 1 : 0]); break;
    case IT_VSYNC: snprintf(out, n, "%s", OFFON[s_edit.vsync ? 1 : 0]); break;
    case IT_ANISO: snprintf(out, n, "%s", ANISO_TXT[aniso_ix(s_edit.anisotropy)]); break;
    case IT_FXAA: snprintf(out, n, "%s", OFFON[s_edit.fxaa ? 1 : 0]); break;
    case IT_MSAA: snprintf(out, n, "%s", MSAA_TXT[msaa_ix(s_edit.msaa)]); break;
    case IT_WIDE: snprintf(out, n, "%s", WIDE_TXT[clamp_to(s_edit.widescreen, 0, 3)]); break;
    case IT_MOUSE: snprintf(out, n, "%s", OFFON[s_edit.mouse_aim ? 1 : 0]); break;
    case IT_MSENS: snprintf(out, n, "%d%%", SENS_VAL[sens_ix(s_edit.mouse_sens)]); break;
    case IT_MINV: snprintf(out, n, "%s", OFFON[s_edit.mouse_invert ? 1 : 0]); break;
    default:
        if (item >= IT_MB0 && item <= IT_MB_LAST)
            snprintf(out, n, "%s", mb_target_label[s_mb[item - IT_MB0]]);
        else
            out[0] = 0;
        break;
    }
}

static void nudge(int item, int dir)
{
    int v;
    if (dir == 0)
        return;
    switch (item) {
    case IT_SCALE:
        s_edit.resolution_scale = clamp_to(s_edit.resolution_scale + dir, 1, 5);
        break;
    case IT_FS: s_edit.fullscreen = !s_edit.fullscreen; break;
    case IT_VSYNC: s_edit.vsync = !s_edit.vsync; break;
    case IT_ANISO:
        v = aniso_ix(s_edit.anisotropy) + dir;
        s_edit.anisotropy = ANISO_VAL[clamp_to(v, 0, 4)];
        break;
    case IT_FXAA: s_edit.fxaa = !s_edit.fxaa; break;
    case IT_MSAA:
        v = msaa_ix(s_edit.msaa) + dir;
        s_edit.msaa = MSAA_VAL[clamp_to(v, 0, 3)];
        break;
    case IT_WIDE: s_edit.widescreen = (clamp_to(s_edit.widescreen, 0, 3) + dir + 4) % 4; break;
    case IT_MOUSE: s_edit.mouse_aim = !s_edit.mouse_aim; break;
    case IT_MSENS: s_edit.mouse_sens = SENS_VAL[clamp_to(sens_ix(s_edit.mouse_sens) + dir, 0, SENS_N - 1)]; break;
    case IT_MINV: s_edit.mouse_invert = !s_edit.mouse_invert; break;
    default:
        if (item >= IT_MB0 && item <= IT_MB_LAST) {
            char root[MAX_PATH];
            int *t = &s_mb[item - IT_MB0];
            *t = (*t + dir + mb_target_count) % mb_target_count;
            halo2_install_root(root, sizeof root);
            if (mb_save(root, s_mb))
                h2_mouse_binds_changed();
            return;
        }
        return;
    }
    halo2_settings_apply(&s_edit, 1);
}

static void activate(int item)
{
    if (item == IT_CLOSE)
        close_overlay();
    else if (item == IT_QUIT) {
        if (!s_set_armed) {
            s_set_armed = 1;
            return;
        }
        fprintf(stderr, "[OVERLAY] quit confirmed\n");
        TerminateProcess(GetCurrentProcess(), 0);
    } else
        nudge(item, 1);
}

static void draw_quit(HDC hdc, int w, int h)
{
    RECT box, r;
    char line[128];
    int bw = 280, bh = 140, x = (w - bw) / 2, y = (h - bh) / 2, i;
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    HBRUSH panel = CreateSolidBrush(RGB(24, 28, 36));
    HBRUSH lit = CreateSolidBrush(RGB(40, 90, 160));

    SetRect(&r, 0, 0, w, h);
    FillRect(hdc, &r, bg);
    SetRect(&box, x, y, x + bw, y + bh);
    FillRect(hdc, &box, panel);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240, 240, 240));
    SetRect(&r, x, y + 16, x + bw, y + 50);
    DrawTextA(hdc, "Do you want to quit?", -1, &r, DT_CENTER | DT_SINGLELINE);
    for (i = 0; i < 2; i++) {
        RECT b;
        SetRect(&b, x + 24 + i * 120, y + 80, x + 24 + i * 120 + 100, y + 116);
        FillRect(hdc, &b, i == s_quit_sel ? lit : panel);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextA(hdc, i == 0 ? "No" : "Yes", -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    (void)line;
    DeleteObject(bg);
    DeleteObject(panel);
    DeleteObject(lit);
}

static void draw_settings(HDC hdc, int w, int h)
{
    RECT r;
    int bw = 560, rh = (h - 150) / IT_COUNT, bh, x = (w - bw) / 2, y, i;
    HBRUSH bg = CreateSolidBrush(RGB(8, 10, 14));
    HBRUSH panel = CreateSolidBrush(RGB(22, 26, 34));
    HBRUSH lit = CreateSolidBrush(RGB(36, 80, 140));
    HFONT title = CreateFontA(22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT body, note = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HGDIOBJ old;

    /* Rows shrink to fit the window: 36 px at most, 18 at least. */
    if (rh > 36) rh = 36;
    if (rh < 18) rh = 18;
    bh = 56 + IT_COUNT * rh + 60;
    y = (h - bh) / 2;
    if (y < 0) y = 0;
    body = CreateFontA(rh >= 30 ? 18 : (rh >= 24 ? 16 : 14), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    SetRect(&r, 0, 0, w, h);
    FillRect(hdc, &r, bg);
    SetRect(&r, x, y, x + bw, y + bh);
    FillRect(hdc, &r, panel);
    SetBkMode(hdc, TRANSPARENT);
    old = SelectObject(hdc, title);
    SetTextColor(hdc, RGB(250, 250, 250));
    SetRect(&r, x + 20, y + 16, x + bw - 20, y + 48);
    DrawTextA(hdc, "Settings", -1, &r, DT_LEFT | DT_SINGLELINE);
    SelectObject(hdc, body);
    for (i = 0; i < IT_COUNT; i++) {
        RECT row;
        char val[40];
        SetRect(&row, x + 16, y + 56 + i * rh, x + bw - 16, y + 56 + (i + 1) * rh - 2);
        if (i == s_sel)
            FillRect(hdc, &row, lit);
        SetTextColor(hdc, RGB(235, 235, 235));
        DrawTextA(hdc, ITEM_NAME[i], -1, &row, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        item_text(i, val, sizeof val);
        if (i == IT_QUIT && s_set_armed)
            snprintf(val, sizeof val, "Confirm?");
        DrawTextA(hdc, val, -1, &row, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(hdc, note);
    SetTextColor(hdc, RGB(170, 176, 184));
    SetRect(&r, x + 16, y + bh - 52, x + bw - 16, y + bh - 8);
    DrawTextA(hdc, "Changes apply now and are saved (widescreen: at next start).\n"
                   "F1 or B closes. Arrows or the D-pad change a setting.",
              -1, &r, DT_LEFT | DT_WORDBREAK);
    SelectObject(hdc, old);
    DeleteObject(title);
    DeleteObject(body);
    DeleteObject(note);
    DeleteObject(bg);
    DeleteObject(panel);
    DeleteObject(lit);
}

static void paint(void)
{
    RECT cr;
    HDC hdc, mem;
    HBITMAP bm, old;
    BLENDFUNCTION blend;
    SIZE size;
    POINT src, dst;
    int w, h;
    int mode = (int)s_mode;

    if (!s_hwnd || mode == MODE_NONE)
        return;
    GetClientRect(s_hwnd, &cr);
    w = cr.right;
    h = cr.bottom;
    if (w <= 0 || h <= 0)
        return;
    hdc = GetDC(s_hwnd);
    mem = CreateCompatibleDC(hdc);
    bm = CreateCompatibleBitmap(hdc, w, h);
    old = SelectObject(mem, bm);
    if (mode == MODE_QUIT)
        draw_quit(mem, w, h);
    else
        draw_settings(mem, w, h);
    size.cx = w;
    size.cy = h;
    src.x = src.y = 0;
    dst.x = dst.y = 0;
    { RECT wr; GetWindowRect(s_hwnd, &wr); dst.x = wr.left; dst.y = wr.top; }
    memset(&blend, 0, sizeof blend);
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 235;
    UpdateLayeredWindow(s_hwnd, hdc, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(s_hwnd, hdc);
}

static void on_key(WPARAM w)
{
    int mode = (int)s_mode;
    if (w == VK_ESCAPE) {
        if (mode == MODE_NONE)
            open_quit();
        else
            close_overlay();
        return;
    }
    if (w == VK_F1) {
        if (mode == MODE_SET)
            close_overlay();
        else
            open_settings();
        return;
    }
    if (mode == MODE_QUIT) {
        if (w == VK_LEFT || w == VK_RIGHT)
            s_quit_sel = !s_quit_sel;
        else if (w == VK_RETURN || w == VK_SPACE) {
            if (s_quit_sel)
                TerminateProcess(GetCurrentProcess(), 0);
            else
                close_overlay();
        }
        return;
    }
    if (mode != MODE_SET)
        return;
    if (w == VK_UP)
        s_sel = (s_sel + IT_COUNT - 1) % IT_COUNT;
    else if (w == VK_DOWN)
        s_sel = (s_sel + 1) % IT_COUNT;
    else if (w == VK_LEFT)
        nudge(s_sel, -1);
    else if (w == VK_RIGHT)
        nudge(s_sel, 1);
    else if (w == VK_RETURN || w == VK_SPACE)
        activate(s_sel);
    if (s_sel != IT_QUIT)
        s_set_armed = 0;
}

static void poll_pad(void)
{
    WORD b = 0;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
    WORD press;
    DWORD now = GetTickCount();
    int mode = (int)s_mode;

    if (!hitz_PadPeek(0, &b, &lt, &rt, &lx, &ly, &rx, &ry))
        b = 0;
    if (ly > 20000) b |= 0x0001;
    if (ly < -20000) b |= 0x0002;
    if (lx < -20000) b |= 0x0004;
    if (lx > 20000) b |= 0x0008;

    if (s_unblock) {
        if (b == 0)
            s_unblock = 0;
        s_pad_prev = b;
        return;
    }
    press = (WORD)(b & ~s_pad_prev);
    s_pad_prev = b;
    if (mode == MODE_NONE)
        return;

    if (press & 0x0001) on_key(VK_UP);
    if (press & 0x0002) on_key(VK_DOWN);
    if (press & 0x0004) on_key(VK_LEFT);
    if (press & 0x0008) on_key(VK_RIGHT);
    if (press & 0x1000) on_key(VK_RETURN);
    if (press & (0x2000 | 0x0010)) on_key(VK_ESCAPE);

    /* Repeat left/right while held, after a short delay. */
    if (mode == MODE_SET && (b & 0x000C)) {
        int dir = (b & 0x0008) ? 1 : -1;
        if (s_repeat_dir != dir) {
            s_repeat_dir = dir;
            s_repeat_at = now + 350;
        } else if (now >= s_repeat_at) {
            nudge(s_sel, dir);
            s_repeat_at = now + 120;
        }
    } else
        s_repeat_dir = 0;
}

static LRESULT CALLBACK subclass_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) &&
        (w == VK_ESCAPE || w == VK_F1) && !(l & (1L << 30))) {
        on_key(w);
        if (s_hwnd)
            paint();
        return 0;
    }
    if (s_mode != MODE_NONE && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) &&
        (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT ||
         w == VK_RETURN || w == VK_SPACE) && !(l & (1L << 30))) {
        on_key(w);
        if (s_hwnd)
            paint();
        return 0;
    }
    return CallWindowProcA(s_oldproc, h, m, w, l);
}

static LRESULT CALLBACK overlay_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_TIMER) {
        HWND game = xbox_FramebufferWindowHandle();
        if (game && game != s_game) {
            if (s_game && s_oldproc) {
                SetWindowLongPtrA(s_game, GWLP_WNDPROC, (LONG_PTR)s_oldproc);
                s_oldproc = NULL;
            }
            s_game = game;
            s_oldproc = (WNDPROC)SetWindowLongPtrA(s_game, GWLP_WNDPROC, (LONG_PTR)subclass_proc);
        }
        if (s_mode != MODE_NONE && s_game)
            layout_overlay(s_game);
        poll_pad();
        if (s_mode != MODE_NONE)
            paint();
        return 0;
    }
    if (m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI overlay_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND game;
    MSG msg;
    (void)unused;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "Halo2NativeOverlay";
    RegisterClassA(&wc);

    while (s_running && !(game = xbox_FramebufferWindowHandle()))
        Sleep(50);
    if (!s_running)
        return 0;
    s_game = game;
    s_oldproc = (WNDPROC)SetWindowLongPtrA(s_game, GWLP_WNDPROC, (LONG_PTR)subclass_proc);

    s_hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                             "Halo2NativeOverlay", "Halo 2 Settings",
                             WS_POPUP, 0, 0, 64, 64, s_game, NULL, wc.hInstance, NULL);
    if (!s_hwnd) {
        fprintf(stderr, "[OVERLAY] cannot create overlay window (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }
    ShowWindow(s_hwnd, SW_HIDE);
    SetTimer(s_hwnd, 1, 16, NULL);
    fprintf(stderr, "[OVERLAY] ready (ESC quit, F1 settings)\n");
    while (s_running && GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

void halo2_overlay_start(void)
{
    if (s_thread)
        return;
    InterlockedExchange(&s_running, 1);
    s_thread = CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL);
}
