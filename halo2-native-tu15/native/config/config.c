/* Halo 2 Native 1.5 settings editor. Reads and writes the same
 * SaveData\settings.ini file (in the install folder) as the in-game F1 menu. */
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mouse_binds.h"

#define ID_SCALE  101
#define ID_FS     102
#define ID_VSYNC  103
#define ID_ANISO  104
#define ID_FXAA   105
#define ID_MSAA   106
#define ID_MAIM   107
#define ID_MSENS  108
#define ID_MINV   109
#define ID_MB0    110   /* .. ID_MB0 + MB_COUNT - 1 */
#define ID_RESET  201
#define ID_SAVE   202
#define ID_CANCEL 203
#define ID_NOTE   204

static const char *SCALE_TXT[] = { "1x", "2x", "3x", "4x", "5x" };
static const char *OFFON[] = { "Off", "On" };
static const char *ANISO_TXT[] = { "Off", "2x", "4x", "8x", "16x" };
static const int   ANISO_VAL[] = { 1, 2, 4, 8, 16 };
static const char *MSAA_TXT[] = { "Off", "2x", "4x", "8x" };
static const int   MSAA_VAL[] = { 1, 2, 4, 8 };

static HWND s_scale, s_fs, s_vsync, s_aniso, s_fxaa, s_msaa, s_note;
static HWND s_maim, s_msens, s_minv, s_mb[MB_COUNT];
static const char *SENS_TXT[] = { "25%", "50%", "75%", "100%", "125%", "150%", "200%", "250%", "300%", "400%", "500%" };
static const int   SENS_VAL[] = { 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500 };
#define SENS_N 11

/* <install>\SaveData\settings.ini, the install being the folder that holds
 * build-ninja\ (portable: nothing in the user profile). An older
 * %LOCALAPPDATA%\Halo2Native\settings.ini is copied there the first time. */
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

static int sens_ix(int v)
{
    int i, best = 0;
    for (i = 1; i < SENS_N; i++)
        if (abs(SENS_VAL[i] - v) < abs(SENS_VAL[best] - v)) best = i;
    return best;
}

static void ini_path(char *buf, size_t n)
{
    char root[MAX_PATH], dir[MAX_PATH], old[MAX_PATH], *sl;
    buf[0] = 0;
    if (!GetModuleFileNameA(NULL, root, sizeof root))
        return;
    if ((sl = strrchr(root, '\\')) != NULL) *sl = 0;
    if ((sl = strrchr(root, '\\')) != NULL && _stricmp(sl + 1, "build-ninja") == 0) *sl = 0;
    snprintf(dir, sizeof dir, "%s\\SaveData", root);
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

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int aniso_ix(int a)
{
    int i;
    for (i = 4; i >= 0; i--)
        if (a >= ANISO_VAL[i])
            return i;
    return 0;
}

static int msaa_ix(int a)
{
    int i;
    for (i = 3; i >= 0; i--)
        if (a >= MSAA_VAL[i])
            return i;
    return 0;
}

static void fill_combo(HWND h, const char **text, int n, int sel)
{
    int i;
    SendMessageA(h, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++)
        SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)text[i]);
    SendMessageA(h, CB_SETCURSEL, (WPARAM)clampi(sel, 0, n - 1), 0);
}

static void load_into_ui(int use_defaults)
{
    char path[MAX_PATH];
    int scale = 1, fs = 0, vsync = 1, aniso = 1, fxaa = 0, msaa = 1;
    ini_path(path, sizeof path);
    if (!use_defaults && path[0]) {
        scale = clampi((int)GetPrivateProfileIntA("display", "resolution_scale", 1, path), 1, 5);
        fs = GetPrivateProfileIntA("display", "fullscreen", 0, path) != 0;
        vsync = GetPrivateProfileIntA("display", "vsync", 1, path) != 0;
        aniso = (int)GetPrivateProfileIntA("display", "anisotropy", 1, path);
        fxaa = GetPrivateProfileIntA("display", "fxaa", 0, path) != 0;
        msaa = (int)GetPrivateProfileIntA("display", "msaa", 1, path);
    }
    fill_combo(s_scale, SCALE_TXT, 5, scale - 1);
    fill_combo(s_fs, OFFON, 2, fs);
    fill_combo(s_vsync, OFFON, 2, vsync);
    fill_combo(s_aniso, ANISO_TXT, 5, aniso_ix(aniso));
    fill_combo(s_fxaa, OFFON, 2, fxaa);
    fill_combo(s_msaa, MSAA_TXT, 4, msaa_ix(msaa));
    {
        char root[MAX_PATH];
        int maim = 0, msens = 100, minv = 0, mb[MB_COUNT], i;
        if (!use_defaults && path[0]) {
            maim = GetPrivateProfileIntA("input", "mouse_aim", 0, path) != 0;
            msens = (int)GetPrivateProfileIntA("input", "mouse_sens", 100, path);
            minv = GetPrivateProfileIntA("input", "mouse_invert", 0, path) != 0;
        }
        fill_combo(s_maim, OFFON, 2, maim);
        fill_combo(s_msens, SENS_TXT, SENS_N, sens_ix(msens));
        fill_combo(s_minv, OFFON, 2, minv);
        install_root(root, sizeof root);
        if (use_defaults) mb_load_defaults(root, mb); else mb_load(root, mb);
        for (i = 0; i < MB_COUNT; i++)
            fill_combo(s_mb[i], (const char **)mb_target_label, mb_target_count, mb[i]);
    }
}

static int save_from_ui(HWND owner)
{
    char path[MAX_PATH], dir[MAX_PATH], *slash, buf[16];
    int scale, fs, vsync, aniso, fxaa, msaa;
    ini_path(path, sizeof path);
    if (!path[0]) {
        MessageBoxA(owner, "Could not find the install folder.", "Halo 2 Native", MB_ICONERROR);
        return 0;
    }
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '\\');
    if (slash) {
        *slash = 0;
        CreateDirectoryA(dir, NULL);
    }
    scale = (int)SendMessageA(s_scale, CB_GETCURSEL, 0, 0) + 1;
    fs = (int)SendMessageA(s_fs, CB_GETCURSEL, 0, 0) > 0;
    vsync = (int)SendMessageA(s_vsync, CB_GETCURSEL, 0, 0) > 0;
    aniso = ANISO_VAL[clampi((int)SendMessageA(s_aniso, CB_GETCURSEL, 0, 0), 0, 4)];
    fxaa = (int)SendMessageA(s_fxaa, CB_GETCURSEL, 0, 0) > 0;
    msaa = MSAA_VAL[clampi((int)SendMessageA(s_msaa, CB_GETCURSEL, 0, 0), 0, 3)];
    snprintf(buf, sizeof buf, "%d", clampi(scale, 1, 5));
    WritePrivateProfileStringA("display", "resolution_scale", buf, path);
    WritePrivateProfileStringA("display", "fullscreen", fs ? "1" : "0", path);
    WritePrivateProfileStringA("display", "vsync", vsync ? "1" : "0", path);
    snprintf(buf, sizeof buf, "%d", aniso);
    WritePrivateProfileStringA("display", "anisotropy", buf, path);
    WritePrivateProfileStringA("display", "fxaa", fxaa ? "1" : "0", path);
    snprintf(buf, sizeof buf, "%d", msaa);
    WritePrivateProfileStringA("display", "msaa", buf, path);
    WritePrivateProfileStringA("input", "mouse_aim", SendMessageA(s_maim, CB_GETCURSEL, 0, 0) > 0 ? "1" : "0", path);
    snprintf(buf, sizeof buf, "%d", SENS_VAL[clampi((int)SendMessageA(s_msens, CB_GETCURSEL, 0, 0), 0, SENS_N - 1)]);
    WritePrivateProfileStringA("input", "mouse_sens", buf, path);
    WritePrivateProfileStringA("input", "mouse_invert", SendMessageA(s_minv, CB_GETCURSEL, 0, 0) > 0 ? "1" : "0", path);
    {
        char root[MAX_PATH];
        int mb[MB_COUNT], i;
        for (i = 0; i < MB_COUNT; i++)
            mb[i] = clampi((int)SendMessageA(s_mb[i], CB_GETCURSEL, 0, 0), 0, mb_target_count - 1);
        install_root(root, sizeof root);
        if (!mb_save(root, mb)) {
            MessageBoxA(owner, "The mouse buttons could not be saved to SaveData\\keymap.ini.",
                        "Halo 2 Native", MB_ICONERROR);
            return 0;
        }
    }
    return 1;
}

static HWND labeled_combo_at(HWND p, HFONT font, const char *label, int x, int y, int id)
{
    HWND h, c;
    h = CreateWindowExA(0, "STATIC", label, WS_CHILD | WS_VISIBLE,
                        x, y + 4, 170, 22, p, NULL, NULL, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    c = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", "",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                        x + 175, y, 200, 300, p, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static HWND labeled_combo(HWND p, HFONT font, const char *label, int y, int id)
{
    HWND h, c;
    h = CreateWindowExA(0, "STATIC", label, WS_CHILD | WS_VISIBLE,
                        24, y + 4, 200, 22, p, NULL, NULL, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    c = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", "",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                        240, y, 180, 200, p, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND t, note, reset, save, cancel;
        t = CreateWindowExA(0, "STATIC", "Video",
                            WS_CHILD | WS_VISIBLE,
                            24, 16, 200, 22, h, NULL, NULL, NULL);
        SendMessageA(t, WM_SETFONT, (WPARAM)font, TRUE);
        s_scale = labeled_combo(h, font, "Render scale", 48, ID_SCALE);
        s_fs = labeled_combo(h, font, "Fullscreen", 84, ID_FS);
        s_vsync = labeled_combo(h, font, "VSync", 120, ID_VSYNC);
        s_aniso = labeled_combo(h, font, "Anisotropic filter", 156, ID_ANISO);
        s_fxaa = labeled_combo(h, font, "FXAA", 192, ID_FXAA);
        s_msaa = labeled_combo(h, font, "MSAA", 228, ID_MSAA);
        {
            HWND mt = CreateWindowExA(0, "STATIC", "Mouse (also in the F1 menu in the game)",
                                      WS_CHILD | WS_VISIBLE, 460, 16, 380, 22, h, NULL, NULL, NULL);
            HWND bt;
            int i;
            SendMessageA(mt, WM_SETFONT, (WPARAM)font, TRUE);
            s_maim = labeled_combo_at(h, font, "Mouse aim (off in Xbox Live)", 460, 48, ID_MAIM);
            s_msens = labeled_combo_at(h, font, "Mouse sensitivity", 460, 84, ID_MSENS);
            s_minv = labeled_combo_at(h, font, "Invert mouse", 460, 120, ID_MINV);
            bt = CreateWindowExA(0, "STATIC", "Mouse buttons in play (in menus: left = Start, right = B)",
                                 WS_CHILD | WS_VISIBLE, 460, 162, 380, 22, h, NULL, NULL, NULL);
            SendMessageA(bt, WM_SETFONT, (WPARAM)font, TRUE);
            for (i = 0; i < MB_COUNT; i++)
                s_mb[i] = labeled_combo_at(h, font, mb_key_label[i], 460, 190 + i * 34, ID_MB0 + i);
        }
        note = CreateWindowExA(0, "STATIC",
                               "The picture stays 4:3, letterboxed in the window.\n"
                               "Mouse settings are on the right (also F1 in the game).\n"
                               "Settings are stored in SaveData\\settings.ini.\n"
                               "If the game is already running, open F1 in-game or restart it\n"
                               "to pick up changes made here. Render scale and MSAA rebuild\n"
                               "surfaces and may flicker once.",
                               WS_CHILD | WS_VISIBLE,
                               24, 272, 400, 110, h, (HMENU)ID_NOTE, NULL, NULL);
        SendMessageA(note, WM_SETFONT, (WPARAM)font, TRUE);
        s_note = note;
        reset = CreateWindowExA(0, "BUTTON", "Reset",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                24, 450, 90, 28, h, (HMENU)ID_RESET, NULL, NULL);
        save = CreateWindowExA(0, "BUTTON", "Save",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               640, 450, 90, 28, h, (HMENU)ID_SAVE, NULL, NULL);
        cancel = CreateWindowExA(0, "BUTTON", "Cancel",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 740, 450, 90, 28, h, (HMENU)ID_CANCEL, NULL, NULL);
        SendMessageA(reset, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(save, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(cancel, WM_SETFONT, (WPARAM)font, TRUE);
        load_into_ui(0);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_RESET && HIWORD(w) == BN_CLICKED)
            load_into_ui(1);
        else if (LOWORD(w) == ID_SAVE && HIWORD(w) == BN_CLICKED) {
            if (save_from_ui(h))
                DestroyWindow(h);
        } else if (LOWORD(w) == ID_CANCEL && HIWORD(w) == BN_CLICKED)
            DestroyWindow(h);
        return 0;
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
    INITCOMMONCONTROLSEX icc;
    (void)prev; (void)cmd;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "Halo2NativeConfig";
    RegisterClassA(&wc);
    w = CreateWindowExA(0, "Halo2NativeConfig", "Halo 2 Native - Settings",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, 880, 540,
                        NULL, NULL, inst, NULL);
    if (!w)
        return 1;
    ShowWindow(w, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return (int)msg.wParam;
}
