/* Shared video settings for Halo 2 Native 1.5 (F1 overlay and Config.exe). */
#ifndef HALO2_SETTINGS_H
#define HALO2_SETTINGS_H

#include <stddef.h>

typedef struct Halo2Settings {
    int resolution_scale; /* 1..5 */
    int fullscreen;       /* 0/1 */
    int vsync;            /* 0/1 */
    int anisotropy;       /* 1=off, or 2/4/8/16 */
    int fxaa;             /* 0/1 */
    int msaa;             /* 1=off, or 2/4/8 */
    int widescreen;       /* 0 off (4:3), 1 auto (the monitor's shape), 2 16:9 (default), 3 16:10; at start */
    int hires_textures;   /* 0/1: load the hi-res pack in <game folder>\textures (default on); live */
    int mouse_aim;        /* 0/1: mouse turns the view (h2_mouse.c); off in Xbox Live */
    int mouse_sens;       /* percent, 10..500 */
    int mouse_invert;     /* 0/1: mouse up looks down */
} Halo2Settings;

/* Settings live in the install folder: <install>\SaveData\settings.ini
 * (the folder that holds build-ninja\). An older %LOCALAPPDATA% copy is
 * moved there the first time. */
void halo2_install_root(char *buf, size_t n);
void h2_release_startup(void);                 /* h2_release.c */
int  h2_start_fullscreen(int saved);
void h2_diag_start_log(void);                  /* h2_diag.c */
void h2_diag_video(const char *why);
void h2_diag_frame(void);

/* The picture's shape this run (width / height): 4/3, or the widescreen
 * shape chosen at start. Widescreen is fixed for the run (the game reads its
 * TV type once, at boot). */
float halo2_picture_aspect(void);
/* The shape a widescreen setting (0-3) gives on this monitor: 4/3, 1.6 or 16/9. */
float halo2_widescreen_aspect(int mode);
/* In Xbox Live: the 4:3 view stretched across (anamorphic), so a wide screen
 * sees no more than 4:3 does; off again: the full widescreen view. */
void halo2_widescreen_live(int live);

void halo2_settings_defaults(Halo2Settings *s);
void halo2_settings_ini_path(char *buf, size_t n);
int  halo2_settings_load(Halo2Settings *s);
int  halo2_settings_save(const Halo2Settings *s);

/* Apply environment overrides from the ini (only keys not already in the
 * environment) and remember the loaded values. Call before the D3D11 back
 * end is installed. */
void halo2_settings_boot(void);

const Halo2Settings *halo2_settings_get(void);
void halo2_settings_copy(Halo2Settings *s);

/* Write the struct, save the ini, and queue a live GPU apply. The GPU
 * thread picks the change up through halo2_settings_pump(). */
int  halo2_settings_apply(const Halo2Settings *s, int save);

void halo2_gpu_apply_live(const Halo2Settings *s);
void halo2_settings_pump(void);
const char *halo2_settings_last_apply(void);

#endif
