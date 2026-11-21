#include "overlay.h"
#include "settings.h"
#include "keybinds.h"
#include "halo_ui.h"
#include "h2_mouse.h"
#include "update_check.h"
#include "version.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xinput.h>

/* The F1 menu: a layered window over the game, drawn like Config.exe
 * (halo_ui.c) with two pages, VIDEO and CONTROLS. The game is paused while
 * it is open. Keyboard: arrows, Enter, Esc or F1 to close. Pad: D-pad or
 * stick, A, B or Start. Mouse: the pointer is freed while a menu is open
 * (mouse aim gives it up); pointing selects, left click changes or presses,
 * right click steps a setting back or clears a key, the wheel moves the
 * selection. The overlay window lets the mouse through, so the clicks arrive
 * at the game's window (subclass_proc) in the same coordinates. */

extern HWND xbox_FramebufferWindowHandle(void);
extern int  xbox_FramebufferKeyDown(int vk);
extern void xbox_FramebufferHideCursor(int hide);
extern void xbox_time_pause_begin(void);
extern void xbox_time_pause_end(void);
extern volatile int g_audio_muted;

/* Existing helper in the 1.5 SDL3 pad override: pad 1 only, no keyboard merge. */
extern int hitz_PadPeek(DWORD port, WORD *buttons, BYTE *lt, BYTE *rt,
                        SHORT *lx, SHORT *ly, SHORT *rx, SHORT *ry);

enum { IT_SCALE, IT_FS, IT_VSYNC, IT_ANISO, IT_FXAA, IT_MSAA, IT_WIDE, IT_CLOSE, IT_QUIT, IT_COUNT };
enum { MODE_NONE, MODE_QUIT, MODE_SET };
enum { PAGE_VIDEO, PAGE_CONTROLS };
enum { M_AIM, M_SENS, M_INV };

static const char *const ITEM_NAME[IT_COUNT] = {
    "Render scale", "Fullscreen", "VSync", "Anisotropic filter", "FXAA", "MSAA",
    "Widescreen (at next start)", "Close menu", "Quit game"
};
static const char *const TABS[2] = { "VIDEO", "CONTROLS" };
static const char *const M_LABEL[UI_MOUSE_ROWS] = { "Mouse aim (off in Xbox Live)", "Sensitivity", "Invert" };

static HWND s_game, s_hwnd;
static WNDPROC s_oldproc;
static HANDLE s_thread;
static volatile LONG s_mode;
static volatile LONG s_running;
static int s_page;
static int s_sel;               /* -1: the tab row; else a row of the page */
static int s_col;               /* controls page: key slot 0..KB_SLOTS-1 */
static volatile LONG s_listen;  /* waiting for a key for the selected slot */
static int s_set_armed;         /* "Quit game" asks once more */
static int s_quit_yes;          /* quit prompt: Yes selected (No is the default) */
static char s_status[128];
static Halo2Settings s_edit;
static KbMap s_map;
static int s_unblock;
static WORD s_pad_prev;
static DWORD s_repeat_at;
static int s_repeat_dir;
static unsigned char s_mouse_prev[16];
static RECT s_upd_r;            /* the UPDATE button, in window coordinates */

/* Where the last paint put things, for the mouse (window coordinates). */
static RECT s_tab_r[2];
static RECT s_row_r[IT_COUNT], s_prev_r[IT_COUNT], s_next_r[IT_COUNT];   /* VIDEO page */
static UiHit s_hits[160];                                              /* CONTROLS page */
static int s_nhits;
static RECT s_quit_r[2];                                               /* NO, YES */

#ifdef RECOMP_DEBUG_TOOLS
/* Debug only (RECOMP_DEBUG_TOOLS builds, RECOMP_DEBUG_FPS=1, set by the dev
 * launchers): the game's frame rate in the top-left corner of the picture,
 * updated twice a second -- green at 55+, yellow at 28+, red below. A small
 * layered window of its own, under the F1 menu, letting the mouse through. */
extern uint32_t halo2_gpu_flip_count(void);
static HWND s_fps_hwnd;

static void fps_paint(int fps)
{
    RECT cr;
    POINT o = { 0, 0 }, src = { 0, 0 }, dst;
    SIZE size;
    BITMAPINFO bi;
    BLENDFUNCTION blend;
    unsigned int *px = NULL;
    HDC sdc, mem;
    HBITMAP bm, old;
    HFONT font, oldf;
    RECT tr;
    char t[24];
    int fh, w, h, i;
    COLORREF tc = fps >= 55 ? RGB(90, 230, 110) : fps >= 28 ? RGB(250, 210, 60) : RGB(250, 80, 70);

    if (!s_game || !GetClientRect(s_game, &cr) || cr.bottom <= 0 || !ClientToScreen(s_game, &o))
        return;
    fh = cr.bottom / 36 < 14 ? 14 : cr.bottom / 36;
    w = fh * 5;
    h = fh + fh / 2;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    sdc = GetDC(NULL);
    mem = CreateCompatibleDC(sdc);
    bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bm || !px) {
        if (bm) DeleteObject(bm);
        DeleteDC(mem);
        ReleaseDC(NULL, sdc);
        return;
    }
    old = SelectObject(mem, bm);
    memset(px, 0, (size_t)w * h * 4);
    font = CreateFontA(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH, "Consolas");
    oldf = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    snprintf(t, sizeof t, "%d FPS", fps);
    SetRect(&tr, 0, 0, w, h);
    DrawTextA(mem, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();
    /* white text on black -> the text colour over a 60 % black box, premultiplied */
    for (i = 0; i < w * h; i++) {
        unsigned v = px[i] & 0xFF;                              /* text coverage */
        unsigned a = 150 + (105 * v) / 255;
        unsigned r = (GetRValue(tc) * v / 255) * a / 255;
        unsigned g = (GetGValue(tc) * v / 255) * a / 255;
        unsigned b = (GetBValue(tc) * v / 255) * a / 255;
        px[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    size.cx = w;
    size.cy = h;
    dst.x = o.x + 8;
    dst.y = o.y + 8;
    memset(&blend, 0, sizeof blend);
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(s_fps_hwnd, sdc, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    if (!IsWindowVisible(s_fps_hwnd))
        ShowWindow(s_fps_hwnd, SW_SHOWNOACTIVATE);
    SelectObject(mem, oldf);
    DeleteObject(font);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(NULL, sdc);
}

/* Called from the overlay's 16 ms timer: count frames over half a second. */
static void fps_tick(void)
{
    static DWORD t0;
    static uint32_t f0;
    DWORD now = GetTickCount();
    uint32_t f = halo2_gpu_flip_count();
    HWND top;
    if (!s_fps_hwnd)
        return;
    top = s_game ? GetAncestor(s_game, GA_ROOT) : NULL;
    if (!s_game || !IsWindowVisible(s_game) || (top && IsIconic(top))) {
        if (IsWindowVisible(s_fps_hwnd)) ShowWindow(s_fps_hwnd, SW_HIDE);
        return;
    }
    if (!t0) { t0 = now; f0 = f; return; }
    if (now - t0 < 500)
        return;
    fps_paint((int)((f - f0) * 1000u / (now - t0)));
    t0 = now;
    f0 = f;
}

static void fps_create(HINSTANCE inst)
{
    const char *e = getenv("RECOMP_DEBUG_FPS");
    WNDCLASSA wc;
    if (!(e && e[0] == '1'))
        return;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = "Halo2NativeFps";
    RegisterClassA(&wc);
    s_fps_hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                                 "Halo2NativeFps", "FPS", WS_POPUP, 0, 0, 16, 16, s_game, NULL, inst, NULL);
    fprintf(stderr, "[OVERLAY] debug FPS counter %s\n", s_fps_hwnd ? "on" : "could not be created");
}
#endif

/* A menu opened: mouse aim lets go of the pointer, which is unpinned and
 * shown at once (the pad poll that would do it may be paused with the game). */
static void pointer_to_menu(void)
{
    h2_mouse_overlay_changed();
    ClipCursor(NULL);
    xbox_FramebufferHideCursor(0);
}

static int update_available(void)
{
    UpdInfo u;
    upd_get(&u);
    return u.state == UPD_AVAILABLE;
}

/* UPDATE: the updater waits for the game to close, installs the release and
 * starts the same launcher again (H2_LAUNCHER, set by the launchers). */
static void start_update(void)
{
    char root[MAX_PATH], self[MAX_PATH];
    const char *l = getenv("H2_LAUNCHER");
    halo2_install_root(root, sizeof root);
    GetModuleFileNameA(NULL, self, sizeof self);
    if (upd_launch(root, l && *l ? l : self)) {
        fprintf(stderr, "[OVERLAY] update started\n");
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

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
    InterlockedExchange(&s_listen, 0);
    InterlockedExchange(&s_mode, MODE_NONE);
    s_set_armed = 0;
    s_unblock = 1;
    if (s_hwnd)
        ShowWindow(s_hwnd, SW_HIDE);
    pause_off();
    h2_mouse_overlay_changed();         /* mouse aim takes the pointer back */
    fprintf(stderr, "[OVERLAY] close\n");
}

static void open_quit(void)
{
    if (s_mode == MODE_NONE)
        pause_on();
    s_quit_yes = 0;
    InterlockedExchange(&s_listen, 0);
    InterlockedExchange(&s_mode, MODE_QUIT);
    pointer_to_menu();
    layout_overlay(s_game);
    fprintf(stderr, "[OVERLAY] quit prompt\n");
}

static void open_settings(void)
{
    char root[MAX_PATH];
    if (s_mode == MODE_NONE)
        pause_on();
    halo2_settings_copy(&s_edit);
    halo2_install_root(root, sizeof root);
    kb_load(root, &s_map);
    s_sel = 0;
    s_col = 0;
    s_set_armed = 0;
    s_status[0] = 0;
    InterlockedExchange(&s_listen, 0);
    InterlockedExchange(&s_mode, MODE_SET);
    pointer_to_menu();
    layout_overlay(s_game);
    fprintf(stderr, "[OVERLAY] settings\n");
}

static int clamp_to(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int aniso_ix(int a) { return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0; }
static int msaa_ix(int a) { return a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0; }

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

/* The VIDEO page's last row is the UPDATE button while a new version is out. */
#define IT_UPDATE IT_COUNT
static int page_rows(void) { return s_page == PAGE_VIDEO ? IT_COUNT + update_available() : ui_controls_rows(); }

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
    case IT_CLOSE: snprintf(out, n, "F1 / Esc"); break;
    case IT_QUIT: snprintf(out, n, "%s", s_set_armed ? "Press again" : "Exit to Windows"); break;
    default: out[0] = 0; break;
    }
}

static void mouse_text(int m, char *out, size_t n)
{
    if (m == M_AIM) snprintf(out, n, "%s", OFFON[s_edit.mouse_aim ? 1 : 0]);
    else if (m == M_SENS) snprintf(out, n, "%d%%", SENS_VAL[sens_ix(s_edit.mouse_sens)]);
    else snprintf(out, n, "%s", OFFON[s_edit.mouse_invert ? 1 : 0]);
}

static void save_binds(void)
{
    char root[MAX_PATH];
    halo2_install_root(root, sizeof root);
    if (kb_save(root, &s_map))
        h2_mouse_binds_changed();          /* the game reloads SaveData\keymap.ini */
    else
        snprintf(s_status, sizeof s_status, "Could not save keymap.ini.");
}

static void nudge(int item, int dir)
{
    int v;
    if (dir == 0)
        return;
    if (s_page == PAGE_CONTROLS) {
        int m = item - kb_action_count;
        if (m == M_AIM) s_edit.mouse_aim = !s_edit.mouse_aim;
        else if (m == M_SENS) s_edit.mouse_sens = SENS_VAL[clamp_to(sens_ix(s_edit.mouse_sens) + dir, 0, SENS_N - 1)];
        else if (m == M_INV) s_edit.mouse_invert = !s_edit.mouse_invert;
        else return;
        halo2_settings_apply(&s_edit, 1);
        return;
    }
    switch (item) {
    case IT_SCALE: s_edit.resolution_scale = clamp_to(s_edit.resolution_scale + dir, 1, 5); break;
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
    default: return;
    }
    halo2_settings_apply(&s_edit, 1);
}

static void bind_key(const char *name)
{
    char show[32];
    int from;
    if (!InterlockedExchange(&s_listen, 0))
        return;
    if (!name) {
        snprintf(s_status, sizeof s_status, "That key is kept for the game (Esc, F1, F9, Delete).");
        return;
    }
    from = kb_bind(&s_map, s_sel, s_col, name);
    kb_display(name, show, sizeof show);
    if (from >= 0 && from != s_sel)
        snprintf(s_status, sizeof s_status, "%s moved here from %s.", show, kb_actions[from].label);
    else
        snprintf(s_status, sizeof s_status, "%s is now %s.", show, kb_actions[s_sel].label);
    save_binds();
}

static void activate(int item)
{
    if (s_page == PAGE_CONTROLS) {
        if (item >= 0 && item < kb_action_count) {
            s_status[0] = 0;
            memset(s_mouse_prev, 1, sizeof s_mouse_prev);   /* only a new press counts */
            InterlockedExchange(&s_listen, 1);
        } else
            nudge(item, 1);
        return;
    }
    if (item == IT_UPDATE)
        start_update();
    else if (item == IT_CLOSE)
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

/* ---- drawing -------------------------------------------------------------- */

static void draw_settings(HDC hdc, int w, int h)
{
    UiFonts f;
    RECT all, band, box, foot;
    float sc = (float)w / 960.0f;
    int bw, bh, x, y, i;
    const char *hint;

    if ((float)h / 640.0f < sc) sc = (float)h / 640.0f;
    ui_fonts_make(&f, sc);
    bw = ui_px(&f, 960);
    bh = ui_px(&f, 640);
    x = (w - bw) / 2;
    y = (h - bh) / 2;
    SetRect(&all, 0, 0, w, h);
    ui_background(hdc, &all);
    SetRect(&band, x + ui_px(&f, 16), y + ui_px(&f, 16), x + bw - ui_px(&f, 16), y + ui_px(&f, 84));
    ui_header(hdc, &f, &band, "SETTINGS", TABS, 2, s_page, s_sel < 0, s_page == PAGE_CONTROLS, s_tab_r);

    if (s_page == PAGE_CONTROLS) {
        UiControls c;
        char vals[UI_MOUSE_ROWS][24];
        memset(&c, 0, sizeof c);
        c.map = &s_map;
        c.sel_row = s_sel;
        c.sel_col = s_col;
        c.listening = s_listen;
        c.hover_row = c.hover_col = -1;
        for (i = 0; i < UI_MOUSE_ROWS; i++) {
            mouse_text(i, vals[i], sizeof vals[i]);
            c.opt_label[i] = M_LABEL[i];
            c.opt_val[i] = vals[i];
        }
        SetRect(&box, x + ui_px(&f, 32), y + ui_px(&f, 100), x + bw - ui_px(&f, 32), y + bh - ui_px(&f, 64));
        s_nhits = ui_controls(hdc, &f, &box, &c, s_hits, (int)(sizeof s_hits / sizeof s_hits[0]));
        hint = s_listen ? "Press a key, mouse button or the wheel. Esc cancels, Delete clears."
             : s_status[0] ? s_status
             : "Click a key (or Enter) to change it, right-click or Delete clears it; changes apply at once. F1 or Esc closes.";
    } else {
        RECT sec;
        int rx0 = x + ui_px(&f, 200), rx1 = x + ui_px(&f, 760), ry = y + ui_px(&f, 130);
        SetRect(&sec, rx0, y + ui_px(&f, 96), rx1, y + ui_px(&f, 126));
        ui_section(hdc, &f, &sec, "Display");
        for (i = 0; i < IT_COUNT; i++) {
            RECT row;
            char val[40];
            int top = ry + i * ui_px(&f, 40) + (i >= IT_CLOSE ? ui_px(&f, 20) : 0);
            SetRect(&row, rx0, top, rx1, top + ui_px(&f, 34));
            s_row_r[i] = row;
            SetRect(&s_prev_r[i], 0, 0, 0, 0);
            SetRect(&s_next_r[i], 0, 0, 0, 0);
            item_text(i, val, sizeof val);
            if (i >= IT_CLOSE)
                ui_button(hdc, &f, &row, i == IT_QUIT && s_set_armed ? "QUIT GAME - PRESS AGAIN"
                                                                     : (i == IT_CLOSE ? "CLOSE MENU" : "QUIT GAME"),
                          i == s_sel, 0);
            else
                ui_option_row(hdc, &f, &row, ITEM_NAME[i], val, i == s_sel, 0, &s_prev_r[i], &s_next_r[i]);
        }
        hint = "Click or use the arrows to change a setting (widescreen: next start). Tabs or Page Down: "
               "CONTROLS. F1 or Esc closes.";
    }
    SetRect(&foot, x + ui_px(&f, 36), y + bh - ui_px(&f, 52), x + bw - ui_px(&f, 36), y + bh - ui_px(&f, 16));
    SetRect(&s_upd_r, 0, 0, 0, 0);
    if (!s_listen && update_available()) {
        UpdInfo u;
        char t[128];
        upd_get(&u);
        snprintf(t, sizeof t, "Version %s is available (you have %s). Select UPDATE, press U or click it.",
                 u.latest, H2N_VERSION);
        ui_update_banner(hdc, &f, &foot, t, s_page == PAGE_VIDEO && s_sel == IT_UPDATE, 0, &s_upd_r);
    } else
        ui_text(hdc, f.fine, s_listen ? RGB(247, 148, 29) : RGB(150, 178, 210), &foot, hint,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ui_fonts_free(&f);
}

/* The quit prompt: a small panel in the middle; the rest of the window is
 * left to dim the game (paint()). */
static void draw_quit(HDC hdc, int w, int h, RECT *box)
{
    UiFonts f;
    RECT band, msg, b;
    float sc = (float)w / 960.0f;
    int bw, bh, i;
    static const char *const BTN[2] = { "NO", "YES" };

    if ((float)h / 640.0f < sc) sc = (float)h / 640.0f;
    ui_fonts_make(&f, sc);
    bw = ui_px(&f, 440);
    bh = ui_px(&f, 210);
    SetRect(box, (w - bw) / 2, (h - bh) / 2, (w - bw) / 2 + bw, (h - bh) / 2 + bh);
    ui_chamfer(hdc, box, ui_px(&f, 22), RGB(6, 24, 54), RGB(70, 140, 214));
    SetRect(&band, box->left + ui_px(&f, 10), box->top + ui_px(&f, 10),
            box->right - ui_px(&f, 10), box->top + ui_px(&f, 62));
    ui_header(hdc, &f, &band, "QUIT GAME", NULL, 0, 0, 0, 0, NULL);
    SetRect(&msg, box->left, band.bottom + ui_px(&f, 10), box->right, band.bottom + ui_px(&f, 54));
    ui_text(hdc, f.body, RGB(242, 246, 251), &msg, "Do you want to quit Halo 2?",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (i = 0; i < 2; i++) {
        int cx = (box->left + box->right) / 2 + (i ? ui_px(&f, 12) : -ui_px(&f, 12) - ui_px(&f, 130));
        SetRect(&b, cx, box->bottom - ui_px(&f, 66), cx + ui_px(&f, 130), box->bottom - ui_px(&f, 26));
        s_quit_r[i] = b;
        ui_button(hdc, &f, &b, BTN[i], i == s_quit_yes, 0);
    }
    ui_fonts_free(&f);
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

    if (!s_hwnd || s_mode == MODE_NONE)
        return;
    GetClientRect(s_hwnd, &cr);
    w = cr.right;
    h = cr.bottom;
    if (w <= 0 || h <= 0)
        return;
    hdc = GetDC(s_hwnd);
    mem = CreateCompatibleDC(hdc);
    if (s_mode == MODE_QUIT) {
        /* Per-pixel alpha: the panel solid, the game around it dimmed. */
        BITMAPINFO bi;
        unsigned int *px = NULL;
        RECT box;
        int x, y;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        if (!bm || !px) {
            if (bm) DeleteObject(bm);
            DeleteDC(mem);
            ReleaseDC(s_hwnd, hdc);
            return;
        }
        old = SelectObject(mem, bm);
        memset(px, 0, (size_t)w * h * 4);
        draw_quit(mem, w, h, &box);
        GdiFlush();
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                unsigned int *p = &px[(size_t)y * w + x];
                /* inside the panel (the cut corners stay untouched, 0) */
                if (*p && x >= box.left && x < box.right && y >= box.top && y < box.bottom)
                    *p |= 0xFF000000u;          /* GDI leaves alpha at 0 */
                else
                    *p = 0xA0000000u;           /* black at 63% */
            }
        size.cx = w;
        size.cy = h;
        src.x = src.y = 0;
        { RECT wr; GetWindowRect(s_hwnd, &wr); dst.x = wr.left; dst.y = wr.top; }
        memset(&blend, 0, sizeof blend);
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(s_hwnd, hdc, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteDC(mem);
        ReleaseDC(s_hwnd, hdc);
        return;
    }
    bm = CreateCompatibleBitmap(hdc, w, h);
    old = SelectObject(mem, bm);
    draw_settings(mem, w, h);
    size.cx = w;
    size.cy = h;
    src.x = src.y = 0;
    { RECT wr; GetWindowRect(s_hwnd, &wr); dst.x = wr.left; dst.y = wr.top; }
    memset(&blend, 0, sizeof blend);
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 242;
    UpdateLayeredWindow(s_hwnd, hdc, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(s_hwnd, hdc);
}

/* ---- input ---------------------------------------------------------------- */

/* Rows of the controls page in screen order: the left column, then the right. */
static void move_row(int dir)
{
    int n = page_rows();
    if (s_sel < 0) {
        if (dir > 0) s_sel = 0;
        return;
    }
    s_sel += dir;
    if (s_sel < 0) s_sel = -1;                 /* up from the first row: the tabs */
    if (s_sel >= n) s_sel = n - 1;
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
        if (w == VK_LEFT || w == VK_RIGHT || w == VK_TAB)
            s_quit_yes = !s_quit_yes;
        else if (w == 'Y')
            TerminateProcess(GetCurrentProcess(), 0);
        else if (w == 'N')
            close_overlay();
        else if (w == VK_RETURN || w == VK_SPACE) {
            if (s_quit_yes) {
                fprintf(stderr, "[OVERLAY] quit confirmed\n");
                TerminateProcess(GetCurrentProcess(), 0);
            }
            close_overlay();
        }
        return;
    }
    if (mode != MODE_SET)
        return;
    if (w == VK_UP) move_row(-1);
    else if (w == VK_DOWN) move_row(1);
    else if (w == VK_PRIOR || w == VK_NEXT || ((w == VK_LEFT || w == VK_RIGHT) && s_sel < 0)) {
        s_page = !s_page;
        s_col = 0;
        s_status[0] = 0;
        if (s_sel >= page_rows()) s_sel = page_rows() - 1;
    } else if (w == VK_LEFT || w == VK_RIGHT) {
        int dir = w == VK_RIGHT ? 1 : -1;
        if (s_page == PAGE_CONTROLS && s_sel < kb_action_count)
            s_col = clamp_to(s_col + dir, 0, KB_SLOTS - 1);
        else
            nudge(s_sel, dir);
    } else if (w == VK_RETURN || w == VK_SPACE) {
        if (s_sel < 0) s_sel = 0;
        else activate(s_sel);
    } else if (w == 'U' && update_available()) {
        start_update();
    } else if (w == VK_DELETE || w == VK_BACK) {
        if (s_page == PAGE_CONTROLS && s_sel >= 0 && s_sel < kb_action_count) {
            kb_clear(&s_map, s_sel, s_col);
            save_binds();
        }
    }
    if (s_sel != IT_QUIT || s_page != PAGE_VIDEO)
        s_set_armed = 0;
}

static int side_vk(WPARAM vk, LPARAM l)
{
    int ext = (int)((l >> 24) & 1);
    if (vk == VK_SHIFT)
        return MapVirtualKeyA((UINT)((l >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL) return ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return ext ? VK_RMENU : VK_LMENU;
    return (int)vk;
}

/* While a slot waits for a key, a mouse button or a wheel notch counts too:
 * read from the game's key table, which has them with or without mouse aim. */
static void poll_listen_mouse(void)
{
    static const int vks[7] = { 0x01, 0x02, 0x04, 0x05, 0x06, 0x0E, 0x0F };
    int i;
    for (i = 0; i < 7; i++) {
        int down = xbox_FramebufferKeyDown(vks[i]) != 0;
        if (down && !s_mouse_prev[i] && s_listen) {
            bind_key(kb_name_from_vk(vks[i]));
            memset(s_mouse_prev, 1, sizeof s_mouse_prev);
            return;
        }
        s_mouse_prev[i] = (unsigned char)down;
    }
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
    if (mode == MODE_NONE || s_listen)
        return;

    if (press & 0x0001) on_key(VK_UP);
    if (press & 0x0002) on_key(VK_DOWN);
    if (press & 0x0004) on_key(VK_LEFT);
    if (press & 0x0008) on_key(VK_RIGHT);
    if (press & 0x1000) on_key(VK_RETURN);
    if (press & (0x2000 | 0x0010)) on_key(VK_ESCAPE);

    /* Repeat left/right while held, after a short delay (settings only). */
    if (mode == MODE_SET && (b & 0x000C) && !(s_page == PAGE_CONTROLS && s_sel < kb_action_count)) {
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

/* The CONTROLS page's hit under p: a key slot (col 0..), or a setting's
 * "<" / ">" (UI_HIT_PREV / UI_HIT_NEXT). */
static int controls_hit(POINT p, int *row, int *col)
{
    int i;
    for (i = 0; i < s_nhits; i++)
        if (PtInRect(&s_hits[i].r, p)) { *row = s_hits[i].row; *col = s_hits[i].col; return 1; }
    return 0;
}

/* A mouse message while a menu is open (not while a key slot waits for a
 * key: then the buttons and wheel reach the game's key table, which
 * poll_listen_mouse reads). Returns 1 when the menu took it. */
static int on_mouse(UINT m, WPARAM w, LPARAM l)
{
    int mode = (int)s_mode;
    int left = m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK;
    int right = m == WM_RBUTTONDOWN || m == WM_RBUTTONDBLCLK;
    int i, row, col;
    POINT p;

    if (mode == MODE_NONE || s_listen)
        return 0;
    if (m == WM_SETCURSOR) {
        if (LOWORD(l) != HTCLIENT)
            return 0;
        SetCursor(LoadCursorA(NULL, IDC_ARROW));
        return 1;
    }
    if (m == WM_MOUSEWHEEL) {
        if (mode == MODE_SET)
            move_row(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1);
        return 1;
    }
    if (m != WM_MOUSEMOVE && !left && !right && m != WM_MBUTTONDOWN && m != WM_XBUTTONDOWN
        && m != WM_LBUTTONUP && m != WM_RBUTTONUP && m != WM_MBUTTONUP && m != WM_XBUTTONUP)
        return 0;
    p.x = (short)LOWORD(l);
    p.y = (short)HIWORD(l);

    if (mode == MODE_QUIT) {
        for (i = 0; i < 2; i++)
            if (PtInRect(&s_quit_r[i], p)) {
                s_quit_yes = i;
                if (left) {
                    if (i) {
                        fprintf(stderr, "[OVERLAY] quit confirmed\n");
                        TerminateProcess(GetCurrentProcess(), 0);
                    }
                    close_overlay();
                }
            }
        return 1;
    }

    /* the settings */
    if (left && PtInRect(&s_upd_r, p)) {
        start_update();
        return 1;
    }
    if (left)
        for (i = 0; i < 2; i++)
            if (PtInRect(&s_tab_r[i], p)) {
                if (s_page != i) {
                    s_page = i;
                    s_sel = 0;
                    s_col = 0;
                    s_status[0] = 0;
                }
                s_set_armed = 0;
                return 1;
            }
    if (s_page == PAGE_VIDEO) {
        if (update_available() && PtInRect(&s_upd_r, p))
            s_sel = IT_UPDATE;                       /* pointing at UPDATE selects it */
        for (i = 0; i < IT_COUNT; i++) {
            if (!PtInRect(&s_row_r[i], p))
                continue;
            if (s_sel != i)
                s_set_armed = 0;
            s_sel = i;
            if (left) {
                if (PtInRect(&s_prev_r[i], p)) nudge(i, -1);
                else if (PtInRect(&s_next_r[i], p)) nudge(i, 1);
                else activate(i);                    /* a setting steps on; CLOSE / QUIT press */
            } else if (right && i < IT_CLOSE)
                nudge(i, -1);
            break;
        }
    } else if (controls_hit(p, &row, &col)) {
        s_sel = row;
        if (col >= 0) {
            s_col = col;
            if (left)
                activate(row);                       /* the slot waits for a key */
            else if (right) {
                kb_clear(&s_map, row, col);
                s_status[0] = 0;
                save_binds();
            }
        } else if (left)
            nudge(row, col == UI_HIT_NEXT ? 1 : -1);
    }
    return 1;
}

static LRESULT CALLBACK subclass_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1L << 30));
    if (s_listen && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_SYSCHAR)) {
        if (down) {
            if (w == VK_ESCAPE) { InterlockedExchange(&s_listen, 0); s_status[0] = 0; }
            else if (w == VK_DELETE) {
                InterlockedExchange(&s_listen, 0);
                kb_clear(&s_map, s_sel, s_col);
                save_binds();
            } else if (w != VK_LWIN && w != VK_RWIN && w != VK_APPS)
                bind_key(kb_name_from_vk(side_vk(w, l)));
            if (s_hwnd) paint();
        }
        return 0;
    }
    if (s_listen && m == WM_MOUSEWHEEL) {
        bind_key(kb_name_from_vk(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0x0E : 0x0F));
        if (s_hwnd) paint();
        return 0;
    }
    if (down && (w == VK_F1 || w == VK_ESCAPE)) {
        on_key(w);
        if (s_hwnd) paint();
        return 0;
    }
    if (on_mouse(m, w, l)) {
        /* a click shows at once; pointing waits for the 60 Hz timer's paint */
        if (s_hwnd && s_mode != MODE_NONE && m != WM_SETCURSOR && m != WM_MOUSEMOVE) paint();
        return m == WM_SETCURSOR || m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;
    }
    /* With the menu open the keys are its own; Alt+F4 still closes the game. */
    if (s_mode != MODE_NONE && (m == WM_KEYDOWN || (m == WM_SYSKEYDOWN && w != VK_F4))) {
        if (down) {
            on_key(w);
            if (s_hwnd) paint();
        }
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
        if (s_listen)
            poll_listen_mouse();
        poll_pad();
#ifdef RECOMP_DEBUG_TOOLS
        fps_tick();
#endif
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
#ifdef RECOMP_DEBUG_TOOLS
    fps_create(wc.hInstance);
#endif
    SetTimer(s_hwnd, 1, 16, NULL);
    {   /* the quiet update check, once at launch (at most once every 2 hours) */
        char root[MAX_PATH];
        halo2_install_root(root, sizeof root);
        upd_start(root);
    }
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
