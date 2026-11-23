/* Halo 2 Native 1.5 settings editor. Reads and writes the same files as the
 * in-game F1 menu: SaveData\settings.ini and SaveData\keymap.ini, in the
 * install folder. Drawn in the look of the Halo 2 manuals (src/halo_ui.c). */
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h2_layout.h"
#include "keybinds.h"
#include "halo_ui.h"
#include "update_check.h"
#include "version.h"

static const char *const SCALE_TXT[] = { "1x", "2x", "3x", "4x", "5x" };
static const char *const OFFON[] = { "Off", "On" };
static const char *const ANISO_TXT[] = { "Off", "2x", "4x", "8x", "16x" };
static const int ANISO_VAL[] = { 1, 2, 4, 8, 16 };
static const char *const MSAA_TXT[] = { "Off", "2x", "4x", "8x" };
static const int MSAA_VAL[] = { 1, 2, 4, 8 };
static const char *const WIDE_TXT[] = { "Off (4:3)", "Auto (monitor)", "16:9", "16:10" };
static const char *const SENS_TXT[] = { "25%", "50%", "75%", "100%", "125%", "150%", "200%",
                                        "250%", "300%", "400%", "500%" };
static const int SENS_VAL[] = { 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500 };
#define SENS_N 11

/* The settings as indexes into the lists above. */
static const char *const FPS_TXT[] = { "Auto", "60 fps", "30 fps" };
enum { V_SCALE, V_FS, V_VSYNC, V_FPS, V_ANISO, V_FXAA, V_MSAA, V_WIDE, V_HIRES, V_COUNT };
static const char *const V_LABEL[V_COUNT] = {
    "Render scale", "Fullscreen", "VSync", "Frame rate", "Anisotropic filter", "FXAA", "MSAA",
    "Widescreen (at next start)", "Hi-res textures (at next start)",
};
static const char *const *const V_TXT[V_COUNT] = { SCALE_TXT, OFFON, OFFON, FPS_TXT, ANISO_TXT, OFFON, MSAA_TXT, WIDE_TXT, OFFON };
static const int V_N[V_COUNT] = { 5, 2, 2, 3, 5, 2, 4, 4, 2 };
static int s_v[V_COUNT];
enum { M_AIM, M_SENS, M_INV };
static const char *const M_LABEL[UI_MOUSE_ROWS] = { "Mouse aim (off in Xbox Live)", "Sensitivity", "Invert" };
static const char *const *const M_TXT[UI_MOUSE_ROWS] = { OFFON, SENS_TXT, OFFON };
static const int M_N[UI_MOUSE_ROWS] = { 2, SENS_N, 2 };
static int s_m[UI_MOUSE_ROWS];
static KbMap s_map;

/* The window. */
enum { PAGE_VIDEO, PAGE_CONTROLS };
enum { B_RESET, B_SAVE, B_CANCEL, B_COUNT };
static const char *const B_TXT[B_COUNT] = { "RESET DEFAULTS", "SAVE", "CANCEL" };
static const char *const TABS[2] = { "VIDEO", "CONTROLS" };
#define BASE_W 960
#define BASE_H 640
static float s_scale = 1.0f;
static int s_page = PAGE_CONTROLS;
static int s_sel_row = -1, s_sel_col = -1, s_listen;
static int s_hover_row = -1, s_hover_col = -1, s_hover_btn = -1, s_hover_tab = -1;
static char s_status[160];
static UiHit s_hits[160];
static int s_nhits;
static RECT s_tab_r[2], s_btn_r[B_COUNT], s_upd_r, s_report_r;
static int s_hover_upd, s_hover_report, s_upd_state = -1;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* The install folder: the one holding build-ninja\ (this program lives there). */
static void install_root(char *root, size_t n)
{
    char *sl;
    root[0] = 0;
    if (!GetModuleFileNameA(NULL, root, (DWORD)n))
        return;
    if ((sl = strrchr(root, '\\')) != NULL) *sl = 0;
    if ((sl = strrchr(root, '\\')) != NULL && _stricmp(sl + 1, "build-ninja") == 0) *sl = 0;
}

/* <install>\SaveData\settings.ini (portable: nothing in the user profile). An
 * older %LOCALAPPDATA%\Halo2Native\settings.ini is copied there the first time. */
static void ini_path(char *buf, size_t n)
{
    char root[MAX_PATH], dir[MAX_PATH], old[MAX_PATH];
    buf[0] = 0;
    install_root(root, sizeof root);
    if (!root[0])
        return;
    h2_data_path(root, "", dir, sizeof dir);
    snprintf(buf, n, "%s\\settings.ini", dir);
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES &&
        SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, old))) {
        strncat(old, "\\Halo2Native\\settings.ini", sizeof old - strlen(old) - 1);
        if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryA(dir, NULL);
            CopyFileA(old, buf, TRUE);
        }
    }
}

static int index_of(const int *vals, int n, int v)
{
    int i;
    for (i = n - 1; i >= 0; i--)
        if (v >= vals[i]) return i;
    return 0;
}

static int sens_ix(int v)
{
    int i, best = 0;
    for (i = 1; i < SENS_N; i++)
        if (abs(SENS_VAL[i] - v) < abs(SENS_VAL[best] - v)) best = i;
    return best;
}

static void load_all(int defaults)
{
    char path[MAX_PATH], root[MAX_PATH];
    int on;
    ini_path(path, sizeof path);
    on = !defaults && path[0];
    s_v[V_SCALE] = clampi(on ? (int)GetPrivateProfileIntA("display", "resolution_scale", 1, path) : 1, 1, 5) - 1;
    s_v[V_FS] = on && GetPrivateProfileIntA("display", "fullscreen", 0, path) != 0;
    s_v[V_VSYNC] = on ? GetPrivateProfileIntA("display", "vsync", 1, path) != 0 : 1;
    s_v[V_FPS] = clampi(on ? (int)GetPrivateProfileIntA("display", "frame_rate", 0, path) : 0, 0, 2);
    s_v[V_ANISO] = index_of(ANISO_VAL, 5, on ? (int)GetPrivateProfileIntA("display", "anisotropy", 1, path) : 1);
    s_v[V_FXAA] = on && GetPrivateProfileIntA("display", "fxaa", 0, path) != 0;
    s_v[V_MSAA] = index_of(MSAA_VAL, 4, on ? (int)GetPrivateProfileIntA("display", "msaa", 1, path) : 1);
    s_v[V_WIDE] = clampi(on ? (int)GetPrivateProfileIntA("display", "widescreen", 2, path) : 2, 0, 3);
    s_v[V_HIRES] = on ? GetPrivateProfileIntA("display", "hires_textures", 1, path) != 0 : 1;
    s_m[M_AIM] = on && GetPrivateProfileIntA("input", "mouse_aim", 0, path) != 0;
    s_m[M_SENS] = sens_ix(on ? (int)GetPrivateProfileIntA("input", "mouse_sens", 100, path) : 100);
    s_m[M_INV] = on && GetPrivateProfileIntA("input", "mouse_invert", 0, path) != 0;
    install_root(root, sizeof root);
    if (defaults) kb_load_defaults(root, &s_map); else kb_load(root, &s_map);
}

static int save_all(HWND owner)
{
    char path[MAX_PATH], dir[MAX_PATH], root[MAX_PATH], buf[16], *slash;
    ini_path(path, sizeof path);
    if (!path[0]) {
        MessageBoxA(owner, "Could not find the install folder.", "Halo 2 Native", MB_ICONERROR);
        return 0;
    }
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '\\')) != NULL) { *slash = 0; CreateDirectoryA(dir, NULL); }
    snprintf(buf, sizeof buf, "%d", s_v[V_SCALE] + 1);
    WritePrivateProfileStringA("display", "resolution_scale", buf, path);
    WritePrivateProfileStringA("display", "fullscreen", s_v[V_FS] ? "1" : "0", path);
    WritePrivateProfileStringA("display", "vsync", s_v[V_VSYNC] ? "1" : "0", path);
    snprintf(buf, sizeof buf, "%d", s_v[V_FPS]);
    WritePrivateProfileStringA("display", "frame_rate", buf, path);
    snprintf(buf, sizeof buf, "%d", ANISO_VAL[s_v[V_ANISO]]);
    WritePrivateProfileStringA("display", "anisotropy", buf, path);
    WritePrivateProfileStringA("display", "fxaa", s_v[V_FXAA] ? "1" : "0", path);
    snprintf(buf, sizeof buf, "%d", MSAA_VAL[s_v[V_MSAA]]);
    WritePrivateProfileStringA("display", "msaa", buf, path);
    snprintf(buf, sizeof buf, "%d", s_v[V_WIDE]);
    WritePrivateProfileStringA("display", "widescreen", buf, path);
    WritePrivateProfileStringA("display", "hires_textures", s_v[V_HIRES] ? "1" : "0", path);
    WritePrivateProfileStringA("input", "mouse_aim", s_m[M_AIM] ? "1" : "0", path);
    snprintf(buf, sizeof buf, "%d", SENS_VAL[s_m[M_SENS]]);
    WritePrivateProfileStringA("input", "mouse_sens", buf, path);
    WritePrivateProfileStringA("input", "mouse_invert", s_m[M_INV] ? "1" : "0", path);
    install_root(root, sizeof root);
    if (!kb_save(root, &s_map)) {
        MessageBoxA(owner, "The controls could not be saved to keymap.ini in the data folder.",
                    "Halo 2 Native", MB_ICONERROR);
        return 0;
    }
    return 1;
}

/* ---- drawing -------------------------------------------------------------- */

static int px(int v) { return (int)(v * s_scale + 0.5f); }

static void video_row_rect(int i, RECT *r)
{
    int top = px(130);
    SetRect(r, px(200), top + i * px(40), px(760), top + i * px(40) + px(34));
}

/* The hi-res texture pack (the texture addon) is in <install>\textures, where the game loads it. */
static int hires_installed(void)
{
    char root[MAX_PATH], dir[MAX_PATH + 16];
    DWORD a;
    install_root(root, sizeof root);
    snprintf(dir, sizeof dir, "%s\\textures", root);
    a = GetFileAttributesA(dir);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void draw(HDC dc, int w, int h)
{
    UiFonts f;
    RECT r, band;
    int i;
    ui_fonts_make(&f, s_scale);
    SetRect(&r, 0, 0, w, h);
    ui_background(dc, &r);
    SetRect(&band, px(16), px(16), w - px(16), px(84));
    ui_header(dc, &f, &band, "SETTINGS", TABS, 2, s_page, 0, s_page == PAGE_CONTROLS, s_tab_r);
    /* Report a problem: in the title band, on both pages (left of the wield legend) */
    SetRect(&s_report_r, band.right - px(150) - px(200), band.top + px(14), band.right - px(150), band.bottom - px(14));
    ui_button(dc, &f, &s_report_r, "REPORT A PROBLEM", 0, s_hover_report);
    if (s_hover_tab >= 0 && s_hover_tab != s_page) {
        RECT t = s_tab_r[s_hover_tab];
        ui_text(dc, f.head, RGB(130, 206, 255), &t, TABS[s_hover_tab], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    s_nhits = 0;
    if (s_page == PAGE_CONTROLS) {
        UiControls c;
        RECT area;
        memset(&c, 0, sizeof c);
        c.map = &s_map;
        c.sel_row = s_sel_row; c.sel_col = s_sel_col; c.listening = s_listen;
        c.hover_row = s_hover_row; c.hover_col = s_hover_col;
        for (i = 0; i < UI_MOUSE_ROWS; i++) {
            c.opt_label[i] = M_LABEL[i];
            c.opt_val[i] = M_TXT[i][s_m[i]];
        }
        SetRect(&area, px(32), px(100), w - px(32), h - px(76));
        s_nhits = ui_controls(dc, &f, &area, &c, s_hits, (int)(sizeof s_hits / sizeof s_hits[0]));
    } else {
        RECT note, sec;
        SetRect(&sec, px(200), px(96), px(760), px(126));
        ui_section(dc, &f, &sec, "Display");
        for (i = 0; i < V_COUNT; i++) {
            RECT row, l, rr;
            video_row_rect(i, &row);
            ui_option_row(dc, &f, &row, V_LABEL[i],
                          i == V_HIRES && !hires_installed() ? "Not installed" : V_TXT[i][s_v[i]],
                          0, s_hover_row == i, &l, &rr);
            s_hits[s_nhits].row = i; s_hits[s_nhits].col = UI_HIT_PREV; s_hits[s_nhits++].r = l;
            s_hits[s_nhits].row = i; s_hits[s_nhits].col = UI_HIT_NEXT; s_hits[s_nhits++].r = rr;
        }
        SetRect(&note, px(212), px(130) + V_COUNT * px(40) + px(16), px(760), h - px(80));
        ui_text(dc, f.fine, RGB(150, 178, 210), &note,
                "Frame rate Auto plays at 60 fps and drops to 30 if this PC can't keep up.\n"
                "Also in the game's F1 menu; a running game takes changes made here at its next\n"
                "start. Saved in the data folder. Halo 2 Native version " H2N_VERSION ".",
                DT_LEFT | DT_WORDBREAK);
    }
    /* footer */
    {
        int bx = w - px(32), by = h - px(60), bh = px(40);
        static const int bw[B_COUNT] = { 170, 110, 110 };
        RECT st;
        for (i = B_COUNT - 1; i >= 0; i--) {
            SetRect(&s_btn_r[i], bx - px(bw[i]), by, bx, by + bh);
            ui_button(dc, &f, &s_btn_r[i], B_TXT[i], i == B_SAVE, s_hover_btn == i);
            bx -= px(bw[i]) + px(10);
        }
        SetRect(&st, px(36), by, bx - px(10), by + bh);
        SetRect(&s_upd_r, 0, 0, 0, 0);
        if (!s_listen && !s_status[0]) {
            UpdInfo u;
            upd_get(&u);
            if (u.state == UPD_AVAILABLE) {
                char t[96];
                snprintf(t, sizeof t, "Version %s is available (you have %s).", u.latest, H2N_VERSION);
                ui_update_banner(dc, &f, &st, t, 0, s_hover_upd, &s_upd_r);
                ui_fonts_free(&f);
                return;
            }
        }
        ui_text(dc, f.fine, s_listen ? RGB(247, 148, 29) : RGB(150, 178, 210), &st,
                s_listen ? "Press a key, mouse button or the wheel. Esc cancels, Delete clears."
                         : (s_status[0] ? s_status
                            : (s_page == PAGE_CONTROLS
                               ? "Click a box, then press a key or mouse button. Right-click a box to clear it."
                               : "Click the arrows to change a setting.")),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    ui_fonts_free(&f);
}

/* ---- input ---------------------------------------------------------------- */

static int hit(int x, int y, int *row, int *col)
{
    POINT p;
    int i;
    p.x = x; p.y = y;
    for (i = 0; i < s_nhits; i++)
        if (PtInRect(&s_hits[i].r, p)) { *row = s_hits[i].row; *col = s_hits[i].col; return 1; }
    return 0;
}

static void step_option(int row, int dir)
{
    if (s_page == PAGE_VIDEO && row >= 0 && row < V_COUNT) {
        if (row == V_HIRES && !hires_installed())
            return;                 /* nothing to switch: the texture addon is not installed */
        s_v[row] = (s_v[row] + dir + V_N[row]) % V_N[row];
    } else if (s_page == PAGE_CONTROLS && row >= kb_action_count) {
        int m = row - kb_action_count;
        if (m == M_SENS) s_m[m] = clampi(s_m[m] + dir, 0, SENS_N - 1);
        else s_m[m] = (s_m[m] + dir + M_N[m]) % M_N[m];
    }
}

static void bind_key(const char *name)
{
    char show[32];
    int from;
    if (!s_listen || s_sel_row < 0)
        return;
    s_listen = 0;
    if (!name) {
        snprintf(s_status, sizeof s_status, "That key is kept for the game (Esc, F1, F9, Delete).");
        return;
    }
    from = kb_bind(&s_map, s_sel_row, s_sel_col, name);
    kb_display(name, show, sizeof show);
    if (from >= 0 && from != s_sel_row)
        snprintf(s_status, sizeof s_status, "%s moved here from %s.", show, kb_actions[from].label);
    else
        snprintf(s_status, sizeof s_status, "%s is now %s.", show, kb_actions[s_sel_row].label);
}

static int side_vk(int vk, LPARAM l)
{
    int ext = (int)((l >> 24) & 1);
    if (vk == VK_SHIFT)
        return MapVirtualKeyA((UINT)((l >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL) return ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return ext ? VK_RMENU : VK_LMENU;
    return vk;
}

/* ---- Report a problem --------------------------------------------------------------------------------------------
 * Opens the logs folder with the newest log selected, and a new GitHub issue filled in with the game version and the
 * PC (after Marvel vs. Capcom 2's config.exe). The player describes the problem and drags the log onto the page
 * (GitHub needs a free account). RecompLauncher's own report button sends the same log without either. */
static void url_append(char *out, size_t n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = strlen(out);
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static void reg_str(HKEY root, const char *key, const char *name, char *out, DWORD n)
{
    HKEY k;
    if (!RegOpenKeyExA(root, key, 0, KEY_READ, &k)) {
        if (RegQueryValueExA(k, name, NULL, NULL, (BYTE *)out, &n)) out[0] = 0;
        RegCloseKey(k);
    }
}

static void do_report(HWND h)
{
    char root[MAX_PATH], logs[MAX_PATH], newest[MAX_PATH] = "", pat[MAX_PATH], exe[MAX_PATH];
    char ver[32] = "?", cpu[96] = "?", gpu[128] = "?", osname[64] = "Windows", disp[16] = "", build[16] = "";
    char body[2048], url[8192], arg[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    DISPLAY_DEVICEA dev;
    MEMORYSTATUSEX ms;
    DWORD i, sz, hnd;

    install_root(root, sizeof root);
    h2_data_path(root, "", logs, sizeof logs);
    CreateDirectoryA(logs, NULL);
    h2_data_path(root, "logs", logs, sizeof logs);
    CreateDirectoryA(logs, NULL);
    snprintf(pat, sizeof pat, "%s\\halo2-*.log", logs);
    if ((f = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do { if (strcmp(fd.cFileName, newest) > 0) snprintf(newest, sizeof newest, "%s", fd.cFileName); }
        while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    /* the game's version, from halo2_recomp.exe beside this program */
    h2_bin_path(root, "halo2_recomp.exe", exe, sizeof exe);
    if ((sz = GetFileVersionInfoSizeA(exe, &hnd)) != 0) {
        void *vi = malloc(sz);
        VS_FIXEDFILEINFO *ff;
        UINT len;
        if (vi && GetFileVersionInfoA(exe, 0, sz, vi) && VerQueryValueA(vi, "\\", (void **)&ff, &len) && ff)
            snprintf(ver, sizeof ver, "%u.%u.%u", HIWORD(ff->dwFileVersionMS), LOWORD(ff->dwFileVersionMS),
                     HIWORD(ff->dwFileVersionLS));
        free(vi);
    }
    reg_str(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", cpu, sizeof cpu);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", osname, sizeof osname);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", disp, sizeof disp);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild", build, sizeof build);
    if (atoi(build) >= 22000 && !strncmp(osname, "Windows 10", 10)) osname[9] = '1';   /* 11 still says "10" there */
    memset(&dev, 0, sizeof dev);
    dev.cb = sizeof dev;
    for (i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++)
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { snprintf(gpu, sizeof gpu, "%s", dev.DeviceString); break; }
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);

    snprintf(body, sizeof body,
             "**What happened?**\n\n\n"
             "**What were you doing just before?** (a level, the menus, Xbox Live, changing settings...)\n\n\n"
             "**Log:** drag the selected file from the logs folder that just opened (%s) into this box.\n\n"
             "---\nGame %s | %s %s (build %s) | %s | %s | %llu GB\n",
             newest[0] ? newest : "halo2-....log", ver, osname, disp, build, cpu, gpu,
             (unsigned long long)((ms.ullTotalPhys + (1ull << 29)) >> 30));
    strcpy(url, "https://github.com/dalves408/halo2-recomp/issues/new?title=");
    url_append(url, sizeof url, "Problem: ");
    strcat(url, "&body=");
    url_append(url, sizeof url, body);

    if (newest[0]) {
        snprintf(arg, sizeof arg, "/select,\"%s\\%s\"", logs, newest);
        ShellExecuteA(h, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
    } else {
        ShellExecuteA(h, "open", logs, NULL, NULL, SW_SHOWNORMAL);
    }
    ShellExecuteA(h, "open", url, NULL, NULL, SW_SHOWNORMAL);
    MessageBoxA(h, newest[0]
                ? "Two windows opened:\n\n"
                  "  - your logs folder, with the newest log selected\n"
                  "  - a new problem report on GitHub (it needs a free GitHub account)\n\n"
                  "Say what happened, then drag the selected log file onto the report and submit it.\n\n"
                  "The log is from the last time the game ran, so report right after the problem. It holds no personal "
                  "information: your Windows user name is blanked out.\n\n"
                  "Using RecompLauncher? Its own report button sends the log for you."
                : "Your logs folder and a new problem report on GitHub opened. There is no log yet: play until the problem "
                  "happens, then click Report a problem again.",
                "Halo 2 Recomp", MB_ICONINFORMATION);
    snprintf(s_status, sizeof s_status, "The logs folder and the problem report page opened.");
}

static void refresh(HWND h) { InvalidateRect(h, NULL, FALSE); }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        char root[MAX_PATH];
        load_all(0);
        install_root(root, sizeof root);
        upd_start(root);                  /* quiet; at most once every 2 hours */
        SetTimer(h, 1, 500, NULL);
        return 0;
    }
    case WM_TIMER: {
        UpdInfo u;
        upd_get(&u);
        if (u.state != s_upd_state) { s_upd_state = u.state; refresh(h); }
        if (u.state != UPD_CHECKING) KillTimer(h, 1);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT cr;
        HDC dc = BeginPaint(h, &ps), mem;
        HBITMAP bm;
        HGDIOBJ old;
        GetClientRect(h, &cr);
        mem = CreateCompatibleDC(dc);
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
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l), row = -1, col = -1, b = -1, t = -1, i;
        POINT p;
        TRACKMOUSEEVENT tme;
        p.x = x; p.y = y;
        hit(x, y, &row, &col);
        if (s_page == PAGE_VIDEO && row < 0)
            for (i = 0; i < V_COUNT; i++) {
                RECT r;
                video_row_rect(i, &r);
                if (PtInRect(&r, p)) row = i;
            }
        for (i = 0; i < B_COUNT; i++) if (PtInRect(&s_btn_r[i], p)) b = i;
        for (i = 0; i < 2; i++) if (PtInRect(&s_tab_r[i], p)) t = i;
        if (row != s_hover_row || col != s_hover_col || b != s_hover_btn || t != s_hover_tab ||
            PtInRect(&s_upd_r, p) != s_hover_upd || PtInRect(&s_report_r, p) != s_hover_report) {
            s_hover_row = row; s_hover_col = col; s_hover_btn = b; s_hover_tab = t;
            s_hover_upd = PtInRect(&s_upd_r, p);
            s_hover_report = PtInRect(&s_report_r, p);
            refresh(h);
        }
        memset(&tme, 0, sizeof tme);
        tme.cbSize = sizeof tme; tme.dwFlags = TME_LEAVE; tme.hwndTrack = h;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        s_hover_row = s_hover_col = s_hover_btn = s_hover_tab = -1;
        refresh(h);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l), row, col, i;
        POINT p;
        if (s_listen) {
            int vk = m == WM_LBUTTONDOWN ? 1 : m == WM_RBUTTONDOWN ? 2 : m == WM_MBUTTONDOWN ? 4
                   : (GET_XBUTTON_WPARAM(w) == XBUTTON1 ? 5 : 6);
            bind_key(kb_name_from_vk(vk));
            refresh(h);
            return m == WM_XBUTTONDOWN ? TRUE : 0;
        }
        p.x = x; p.y = y;
        if (m == WM_LBUTTONDOWN && PtInRect(&s_report_r, p)) {
            do_report(h);
            refresh(h);
            return 0;
        }
        if (m == WM_LBUTTONDOWN && PtInRect(&s_upd_r, p)) {
            char root[MAX_PATH], self[MAX_PATH];
            install_root(root, sizeof root);
            GetModuleFileNameA(NULL, self, sizeof self);
            if (upd_launch(root, self))
                DestroyWindow(h);         /* the updater takes over and opens Config.exe again */
            else {
                snprintf(s_status, sizeof s_status, "The update could not start. Try again later.");
                refresh(h);
            }
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            for (i = 0; i < 2; i++)
                if (PtInRect(&s_tab_r[i], p) && s_page != i) {
                    s_page = i; s_sel_row = s_sel_col = -1; s_status[0] = 0;
                    refresh(h);
                    return 0;
                }
            for (i = 0; i < B_COUNT; i++)
                if (PtInRect(&s_btn_r[i], p)) {
                    if (i == B_RESET) {
                        load_all(1);
                        snprintf(s_status, sizeof s_status, "Defaults restored. Save to keep them.");
                    } else if (i == B_SAVE) {
                        if (save_all(h)) DestroyWindow(h);
                        return 0;
                    } else {
                        DestroyWindow(h);
                        return 0;
                    }
                    refresh(h);
                    return 0;
                }
        }
        if (hit(x, y, &row, &col)) {
            s_status[0] = 0;
            if (col == UI_HIT_PREV || col == UI_HIT_NEXT) {
                if (m == WM_LBUTTONDOWN) step_option(row, col == UI_HIT_NEXT ? 1 : -1);
            } else if (m == WM_LBUTTONDOWN) {
                s_sel_row = row; s_sel_col = col; s_listen = 1;
                SetFocus(h);
            } else if (m == WM_RBUTTONDOWN) {
                kb_clear(&s_map, row, col);
                s_sel_row = row; s_sel_col = col;
            }
            refresh(h);
        }
        return m == WM_XBUTTONDOWN ? TRUE : 0;
    }
    case WM_MOUSEWHEEL:
        if (s_listen) {
            bind_key(kb_name_from_vk(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0x0E : 0x0F));
            refresh(h);
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (s_listen) {
            if (w == VK_ESCAPE) { s_listen = 0; s_status[0] = 0; }
            else if (w == VK_DELETE) {
                kb_clear(&s_map, s_sel_row, s_sel_col);
                s_listen = 0;
                s_status[0] = 0;
            } else if (w != VK_LWIN && w != VK_RWIN && w != VK_APPS)
                bind_key(kb_name_from_vk(side_vk((int)w, l)));
            refresh(h);
            return 0;
        }
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (m == WM_SYSKEYDOWN) break;
        return 0;
    case WM_SYSCHAR:
        if (s_listen) return 0;
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        s_scale = HIWORD(w) / 96.0f;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        refresh(h);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    HWND w;
    MSG msg;
    RECT r;
    HDC dc;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    (void)prev; (void)cmd;
    SetProcessDPIAware();
    dc = GetDC(NULL);
    s_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
    ReleaseDC(NULL, dc);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));          /* res/halo2.ico */
    wc.lpszClassName = "Halo2NativeConfig";
    RegisterClassA(&wc);
    SetRect(&r, 0, 0, px(BASE_W), px(BASE_H));
    AdjustWindowRect(&r, style, FALSE);
    w = CreateWindowExA(0, "Halo2NativeConfig", "Halo 2 Native - Settings", style,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                        NULL, NULL, inst, NULL);
    if (!w)
        return 1;
    ShowWindow(w, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
