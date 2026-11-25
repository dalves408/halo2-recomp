#include "h2_layout.h"
#include "settings.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gpu/d3d11_backend.h"
#include "gpu/fxaa.h"
#include "gpu/render_scale.h"
#include "gpu/tex_pack.h"
#include "video/gpu_present.h"

extern void xbox_FramebufferWindowSetFullscreen(int on);
extern void h2_mouse_apply(void);       /* h2_mouse.c */

static Halo2Settings s_cur;
static int s_loaded;
static CRITICAL_SECTION s_pending_lock;
static int s_pending_inited;
static Halo2Settings s_pending;
static volatile LONG s_pending_live;
static char s_last_apply[256];

static void pending_init(void)
{
    if (s_pending_inited)
        return;
    InitializeCriticalSection(&s_pending_lock);
    s_pending_inited = 1;
}

/* resolution_scale: 0 = Native (the output's own resolution), 1..5 = 640x480 times n */
static int clamp_scale(int v)
{
    if (v < 0) return 1;
    if (v > 5) return 5;
    return v;
}

static int clamp_aniso(int v)
{
    if (v >= 16) return 16;
    if (v >= 8) return 8;
    if (v >= 4) return 4;
    if (v >= 2) return 2;
    return 1;
}

static int clamp_msaa(int v)
{
    if (v >= 8) return 8;
    if (v >= 4) return 4;
    if (v >= 2) return 2;
    return 1;
}

void halo2_settings_defaults(Halo2Settings *s)
{
    if (!s)
        return;
    s->resolution_scale = 1;
    s->fullscreen = 0;
    s->vsync = 1;
    s->anisotropy = 1;
    s->fxaa = 0;
    s->msaa = 1;
    s->widescreen = 2;     /* 16:9 by default */
    s->hires_textures = 1;
    s->mouse_aim = 0;
    s->mouse_sens = 100;
    s->mouse_invert = 0;
    s->frame_rate = 0;     /* Auto */
}

static int clamp_sens(int v) { return v < 10 ? 10 : (v > 500 ? 500 : v); }

/* The install folder: the exe's folder, or its parent when that is build-ninja. */
void halo2_install_root(char *buf, size_t n)
{
    char *sl;
    if (!buf || !n)
        return;
    buf[0] = 0;
    if (!GetModuleFileNameA(NULL, buf, (DWORD)n))
        return;
    if ((sl = strrchr(buf, '\\')) != NULL) *sl = 0;
    if ((sl = strrchr(buf, '\\')) != NULL && _stricmp(sl + 1, "build-ninja") == 0) *sl = 0;
}

void halo2_settings_ini_path(char *buf, size_t n)
{
    char root[MAX_PATH], old[MAX_PATH], dir[MAX_PATH];

    if (!buf || !n)
        return;
    buf[0] = 0;
    halo2_install_root(root, sizeof root);
    if (!root[0])
        return;
    h2_data_path(root, "", dir, sizeof dir);
    snprintf(buf, n, "%s\\settings.ini", dir);
    /* Portable install: an older build kept the file in %LOCALAPPDATA%. */
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES &&
        SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, old))) {
        strncat(old, "\\Halo2Native\\settings.ini", sizeof old - strlen(old) - 1);
        if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryA(dir, NULL);
            CopyFileA(old, buf, TRUE);
        }
    }
}

static int cfg_get(const char *key, int def, const char *path)
{
    return (int)GetPrivateProfileIntA("display", key, def, path);
}

static int cfg_get_input(const char *key, int def, const char *path)
{
    return (int)GetPrivateProfileIntA("input", key, def, path);
}

int halo2_settings_load(Halo2Settings *s)
{
    char path[MAX_PATH];

    halo2_settings_defaults(s);
    halo2_settings_ini_path(path, sizeof path);
    if (!path[0])
        return 0;
    s->resolution_scale = clamp_scale(cfg_get("resolution_scale", 1, path));
    s->fullscreen = cfg_get("fullscreen", 0, path) != 0;
    s->vsync = cfg_get("vsync", 1, path) != 0;
    s->anisotropy = clamp_aniso(cfg_get("anisotropy", 1, path));
    s->fxaa = cfg_get("fxaa", cfg_get("aa_mode", 0, path) == 1, path) != 0;
    s->msaa = clamp_msaa(cfg_get("msaa", 1, path));
    s->widescreen = cfg_get("widescreen", 2, path);
    if (s->widescreen < 0 || s->widescreen > 3) s->widescreen = 2;
    s->hires_textures = cfg_get("hires_textures", 1, path) != 0;
    s->mouse_aim = cfg_get_input("mouse_aim", 0, path) != 0;
    s->mouse_sens = clamp_sens(cfg_get_input("mouse_sens", 100, path));
    s->mouse_invert = cfg_get_input("mouse_invert", 0, path) != 0;
    s->frame_rate = cfg_get("frame_rate", 0, path);
    if (s->frame_rate < 0 || s->frame_rate > 2) s->frame_rate = 0;
    return 1;
}

static void cfg_put_in(const char *section, const char *key, int v, const char *path)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", v);
    WritePrivateProfileStringA(section, key, buf, path);
}

static void cfg_put(const char *key, int v, const char *path) { cfg_put_in("display", key, v, path); }

int halo2_settings_save(const Halo2Settings *s)
{
    char path[MAX_PATH], dir[MAX_PATH], *slash;

    if (!s)
        return 0;
    halo2_settings_ini_path(path, sizeof path);
    if (!path[0])
        return 0;
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '\\');
    if (slash) {
        *slash = 0;
        CreateDirectoryA(dir, NULL);
    }
    cfg_put("resolution_scale", clamp_scale(s->resolution_scale), path);
    cfg_put("fullscreen", s->fullscreen ? 1 : 0, path);
    cfg_put("vsync", s->vsync ? 1 : 0, path);
    cfg_put("anisotropy", clamp_aniso(s->anisotropy), path);
    cfg_put("fxaa", s->fxaa ? 1 : 0, path);
    cfg_put("msaa", clamp_msaa(s->msaa), path);
    cfg_put("widescreen", s->widescreen >= 0 && s->widescreen <= 3 ? s->widescreen : 2, path);
    cfg_put("hires_textures", s->hires_textures ? 1 : 0, path);
    cfg_put("frame_rate", s->frame_rate >= 0 && s->frame_rate <= 2 ? s->frame_rate : 0, path);
    cfg_put_in("input", "mouse_aim", s->mouse_aim ? 1 : 0, path);
    cfg_put_in("input", "mouse_sens", clamp_sens(s->mouse_sens), path);
    cfg_put_in("input", "mouse_invert", s->mouse_invert ? 1 : 0, path);
    return 1;
}

static void env_if_unset(const char *name, const char *value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

/* Widescreen, with nothing stretched. Halo 2's own Xbox widescreen mode is
 * anamorphic (the HUD and menus stretched across, split screen side by
 * side), so it stays off: the game runs in its 4:3 mode, and
 *   - every surface gets more pixels across (render_scale.c), the picture
 *     is shown at the wide shape (gpu_present.c);
 *   - the 3D camera sees wider: the projection's aspect (width / height,
 *     sub_0002FB50; analysis/apply-gen-patches.py patch 7) is multiplied by
 *     g_h2_wide_fov, so the vertical view stays and more shows at the sides;
 *   - the 2D layer (HUD, menus, text) is squeezed back into the middle 4:3
 *     at its own proportions (vsh_hlsl.c's geometry shader).
 * Wider monitors get 16:9 with bars at the sides. */
float g_h2_wide_fov = 1.0f;                  /* the 3D view's widening (1.333 for 16:9) */
float g_h2_ws_aspect_mul = 4.0f / 3.0f;      /* the game's own widescreen mode (unused) */
static float s_picture_aspect = 4.0f / 3.0f;
static float s_wide_fov_full = 1.0f;         /* g_h2_wide_fov outside Xbox Live */
static float s_squeeze_full = 1.0f;          /* the 2D squeeze outside Xbox Live */

/* Xbox Live matches are played anamorphic: a wider view would see more than
 * players at 4:3 do. While `live` (h2_mouse.c: from the XBOX LIVE menu choice
 * until Campaign / Split Screen / System Link, or a Live match's own traffic,
 * nvnet_nat.c nat_live_match_recent), the camera keeps the 4:3 view and the 2D layer is not
 * squeezed, so the game's 4:3 picture is stretched across the same wide
 * screen -- what Halo 2's own Xbox widescreen mode shows. Both values are
 * read every frame, so this switches at once in either direction. */
void halo2_widescreen_live(int live)
{
    static int cur = -1;
    live = live != 0;
    if (live == cur)
        return;
    cur = live;
    if (s_wide_fov_full <= 1.0f)
        return;                               /* playing at 4:3: nothing to change */
    g_h2_wide_fov = live ? 1.0f : s_wide_fov_full;
    halo2_gpu_set_ui_squeeze(live ? 1.0f : s_squeeze_full);
    fprintf(stderr, "[SETTINGS] widescreen: %s\n",
            live ? "Xbox Live, 4:3 view stretched across (anamorphic)" : "full widescreen view");
}

float halo2_picture_aspect(void) { return s_picture_aspect; }

static float monitor_aspect(void)
{
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    return w > 0 && h > 0 ? (float)w / (float)h : 4.0f / 3.0f;
}

float halo2_widescreen_aspect(int mode)
{
    float aspect = 4.0f / 3.0f;
    switch (mode) {
    case 2: aspect = 16.0f / 9.0f; break;
    case 3: aspect = 16.0f / 10.0f; break;
    case 1:
        aspect = monitor_aspect();
        if (aspect < 1.45f) aspect = 4.0f / 3.0f;            /* 4:3, 5:4: as the game was */
        else if (aspect < 1.70f) aspect = 16.0f / 10.0f;     /* 16:10 (and 3:2) */
        else aspect = 16.0f / 9.0f;                          /* 16:9 and wider */
        break;
    }
    return aspect;
}

static void widescreen_boot(int mode)
{
    float aspect = halo2_widescreen_aspect(mode);
    if (getenv("RECOMP_VIDEO_WIDESCREEN") && getenv("RECOMP_VIDEO_WIDESCREEN")[0] == '0')
        aspect = 4.0f / 3.0f;
    s_picture_aspect = aspect;
    if (aspect > 1.34f) {
        g_h2_wide_fov = aspect * 0.75f;                      /* 1.333 for 16:9, 1.2 for 16:10 */
        halo2_gpu_set_ui_squeeze(4.0f / 3.0f / aspect);      /* 0.75 for 16:9 */
        s_wide_fov_full = g_h2_wide_fov;
        s_squeeze_full = 4.0f / 3.0f / aspect;
    }
    gpu_scale_set_widescreen(aspect);
    gpu_present_set_picture_aspect(aspect);
    fprintf(stderr, "[SETTINGS] widescreen %s: picture %.3f (%s)\n",
            mode == 0 ? "off" : mode == 1 ? "auto" : mode == 2 ? "16:9" : "16:10", aspect,
            aspect > 1.7f ? "16:9" : aspect > 1.34f ? "16:10" : "4:3");
}

static void boot_env(const Halo2Settings *s)
{
    char buf[16];

    if (clamp_scale(s->resolution_scale) == 0)
        snprintf(buf, sizeof buf, "native");
    else
        snprintf(buf, sizeof buf, "%d", clamp_scale(s->resolution_scale));
    env_if_unset("RECOMP_GPU_SCALE", buf);
    env_if_unset("RECOMP_FULLSCREEN", s->fullscreen ? "1" : "0");
    env_if_unset("RECOMP_PRESENT_VSYNC", s->vsync ? "1" : "0");
    /* Frames reach the screen through the D3D11 presenter (gpu_present.c):
     * vsync-paced and, above 1x or with FXAA, straight from the GPU. The GDI
     * window path showed about 40 of 60 frames a second on a 1080p laptop
     * (a 16 ms sleep after each 9 ms blit), which read as hitching. GDI stays
     * the fallback when the presenter cannot start; RECOMP_D3D_PRESENT=0
     * forces it (development builds). */
    env_if_unset("RECOMP_D3D_PRESENT", "1");
    /* Ini anisotropy=1 means off (same as NHL). Env 0 is off; 1 still means
     * legacy "on" (8x) for older launchers. */
    snprintf(buf, sizeof buf, "%d", clamp_aniso(s->anisotropy) == 1 ? 0 : clamp_aniso(s->anisotropy));
    env_if_unset("RECOMP_GPU_ANISO", buf);
    env_if_unset("RECOMP_GPU_FXAA", s->fxaa ? "1" : "0");
    snprintf(buf, sizeof buf, "%d", clamp_msaa(s->msaa));
    env_if_unset("RECOMP_GPU_MSAA", buf);
    /* Aniso only works on mip chains. */
    if (clamp_aniso(s->anisotropy) >= 2)
        env_if_unset("RECOMP_GPU_MIPS", "1");
    /* Hi-res textures (the pack in <game folder>\textures): switchable while
     * the game runs (halo2_gpu_apply_live), so it is not an env setting. */
    texpack_set_active(s->hires_textures);
}

void halo2_settings_boot(void)
{
    pending_init();
    halo2_settings_load(&s_cur);
    boot_env(&s_cur);
    widescreen_boot(s_cur.widescreen);
    s_loaded = 1;
    fprintf(stderr,
            "[SETTINGS] display scale=%dx fullscreen=%d vsync=%d aniso=%d fxaa=%d msaa=%d hires_textures=%d"
            " frame_rate=%s; mouse aim=%d sens=%d%% invert=%d\n",
            s_cur.resolution_scale, s_cur.fullscreen, s_cur.vsync,
            s_cur.anisotropy, s_cur.fxaa, s_cur.msaa, s_cur.hires_textures,
            s_cur.frame_rate == 1 ? "60" : s_cur.frame_rate == 2 ? "30" : "auto",
            s_cur.mouse_aim, s_cur.mouse_sens, s_cur.mouse_invert);
}

const Halo2Settings *halo2_settings_get(void)
{
    if (!s_loaded) {
        halo2_settings_defaults(&s_cur);
        s_loaded = 1;
    }
    return &s_cur;
}

void halo2_settings_copy(Halo2Settings *s)
{
    if (s)
        *s = *halo2_settings_get();
}

void halo2_gpu_apply_live(const Halo2Settings *s)
{
    char buf[16];
    int scale, aniso, msaa;

    if (!s)
        return;
    scale = clamp_scale(s->resolution_scale);
    aniso = clamp_aniso(s->anisotropy);
    msaa = clamp_msaa(s->msaa);

    if (scale == 0) {
        _putenv_s("RECOMP_GPU_SCALE", "native");
        gpu_scale_set_native();
    } else {
        snprintf(buf, sizeof buf, "%d", scale);
        _putenv_s("RECOMP_GPU_SCALE", buf);
        gpu_scale_set_integer(scale);
    }

    _putenv_s("RECOMP_FULLSCREEN", s->fullscreen ? "1" : "0");
    xbox_FramebufferWindowSetFullscreen(s->fullscreen ? 1 : 0);

    _putenv_s("RECOMP_PRESENT_VSYNC", s->vsync ? "1" : "0");
    gpu_present_set_vsync(s->vsync ? 1 : 0);

    snprintf(buf, sizeof buf, "%d", aniso == 1 ? 0 : aniso);
    _putenv_s("RECOMP_GPU_ANISO", buf);
    if (aniso >= 2)
        _putenv_s("RECOMP_GPU_MIPS", "1");
    halo2_gpu_set_aniso(aniso);

    _putenv_s("RECOMP_GPU_FXAA", s->fxaa ? "1" : "0");
    halo2_gpu_set_fxaa(s->fxaa ? 1 : 0);

    snprintf(buf, sizeof buf, "%d", msaa);
    _putenv_s("RECOMP_GPU_MSAA", buf);
    halo2_gpu_set_msaa(msaa);

    halo2_gpu_set_hires(s->hires_textures);
}

int halo2_settings_apply(const Halo2Settings *s, int save)
{
    Halo2Settings t;

    if (!s)
        return 0;
    t = *s;
    t.resolution_scale = clamp_scale(t.resolution_scale);
    t.fullscreen = t.fullscreen ? 1 : 0;
    t.vsync = t.vsync ? 1 : 0;
    t.anisotropy = clamp_aniso(t.anisotropy);
    t.fxaa = t.fxaa ? 1 : 0;
    t.msaa = clamp_msaa(t.msaa);
    if (t.widescreen < 0 || t.widescreen > 3) t.widescreen = 2;
    t.hires_textures = t.hires_textures ? 1 : 0;
    t.mouse_aim = t.mouse_aim ? 1 : 0;
    t.mouse_sens = clamp_sens(t.mouse_sens);
    t.mouse_invert = t.mouse_invert ? 1 : 0;
    if (t.frame_rate < 0 || t.frame_rate > 2) t.frame_rate = 0;
    pending_init();
    EnterCriticalSection(&s_pending_lock);
    s_cur = t;
    s_pending = t;
    s_loaded = 1;
    InterlockedExchange(&s_pending_live, 1);
    LeaveCriticalSection(&s_pending_lock);
    h2_mouse_apply();                    /* mouse settings take effect at once */
    if (save) {
        int ok = halo2_settings_save(&s_cur);
        h2_diag_video("changed in the F1 menu");
        return ok;
    }
    return 1;
}

const char *halo2_settings_last_apply(void)
{
    return s_last_apply;
}

void halo2_settings_pump(void)
{
    Halo2Settings t;

    pending_init();
    if (!s_pending_live)
        return;
    EnterCriticalSection(&s_pending_lock);
    if (!s_pending_live) {
        LeaveCriticalSection(&s_pending_lock);
        return;
    }
    t = s_pending;
    InterlockedExchange(&s_pending_live, 0);
    LeaveCriticalSection(&s_pending_lock);

    snprintf(s_last_apply, sizeof s_last_apply,
             "scale=%dx fullscreen=%d vsync=%d aniso=%d fxaa=%d msaa=%d hires=%d",
             t.resolution_scale, t.fullscreen, t.vsync,
             t.anisotropy, t.fxaa, t.msaa, t.hires_textures);
    fprintf(stderr, "[SETTINGS] live apply %s\n", s_last_apply);
    fflush(stderr);
    halo2_gpu_apply_live(&t);
}
