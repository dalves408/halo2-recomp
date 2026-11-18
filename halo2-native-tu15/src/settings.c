#include "settings.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gpu/d3d11_backend.h"
#include "gpu/fxaa.h"
#include "gpu/render_scale.h"
#include "video/gpu_present.h"

extern void xbox_FramebufferWindowSetFullscreen(int on);

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

static int clamp_scale(int v)
{
    if (v < 1) return 1;
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
}

void halo2_settings_ini_path(char *buf, size_t n)
{
    char base[MAX_PATH];

    if (!buf || !n)
        return;
    buf[0] = 0;
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        return;
    snprintf(buf, n, "%s\\Halo2Native\\settings.ini", base);
}

static int cfg_get(const char *key, int def, const char *path)
{
    return (int)GetPrivateProfileIntA("display", key, def, path);
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
    return 1;
}

static void cfg_put(const char *key, int v, const char *path)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", v);
    WritePrivateProfileStringA("display", key, buf, path);
}

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
    return 1;
}

static void env_if_unset(const char *name, const char *value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

static void boot_env(const Halo2Settings *s)
{
    char buf[16];

    snprintf(buf, sizeof buf, "%d", clamp_scale(s->resolution_scale));
    env_if_unset("RECOMP_GPU_SCALE", buf);
    env_if_unset("RECOMP_FULLSCREEN", s->fullscreen ? "1" : "0");
    env_if_unset("RECOMP_PRESENT_VSYNC", s->vsync ? "1" : "0");
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
}

void halo2_settings_boot(void)
{
    pending_init();
    halo2_settings_load(&s_cur);
    boot_env(&s_cur);
    s_loaded = 1;
    fprintf(stderr,
            "[SETTINGS] display scale=%dx fullscreen=%d vsync=%d aniso=%d fxaa=%d msaa=%d\n",
            s_cur.resolution_scale, s_cur.fullscreen, s_cur.vsync,
            s_cur.anisotropy, s_cur.fxaa, s_cur.msaa);
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

    snprintf(buf, sizeof buf, "%d", scale);
    _putenv_s("RECOMP_GPU_SCALE", buf);
    gpu_scale_set_integer(scale);

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
    pending_init();
    EnterCriticalSection(&s_pending_lock);
    s_cur = t;
    s_pending = t;
    s_loaded = 1;
    InterlockedExchange(&s_pending_live, 1);
    LeaveCriticalSection(&s_pending_lock);
    if (save)
        return halo2_settings_save(&s_cur);
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
             "scale=%dx fullscreen=%d vsync=%d aniso=%d fxaa=%d msaa=%d",
             t.resolution_scale, t.fullscreen, t.vsync,
             t.anisotropy, t.fxaa, t.msaa);
    fprintf(stderr, "[SETTINGS] live apply %s\n", s_last_apply);
    fflush(stderr);
    halo2_gpu_apply_live(&t);
}
