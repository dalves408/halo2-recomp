#ifndef HALO2_SCREENSHOT_H
#define HALO2_SCREENSHOT_H

/*
 * Debug-only DEL-key screenshot of the presented D3D11 frame, plus a
 * matching .txt sidecar. Compiled in only when RECOMP_DEBUG_TOOLS is on,
 * and inert at run time unless RECOMP_DEBUG_SCREENSHOT=1.
 *
 * Call try_copy on the render thread, ideally while the producer still
 * holds the shared-present mutex, then finalize after releasing it.
 */

#ifdef RECOMP_DEBUG_TOOLS

#include <d3d11.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void halo2_screenshot_runtime_init(void);

/* Cached RECOMP_DEBUG_SCREENSHOT=1 check. Cheap to call every frame. */
int halo2_screenshot_enabled(void);

/* Edge-triggered. If DEL was newly pressed in the focused game window,
 * copies src into a staging texture. Returns 1 if a copy was queued. */
int halo2_screenshot_try_copy(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                              ID3D11Texture2D *src, uint32_t width, uint32_t height,
                              DXGI_FORMAT format);

/* Map a queued staging copy, write PNG (WIC) or BMP fallback, a sidecar,
 * and log the saved path. */
void halo2_screenshot_finalize(ID3D11DeviceContext *ctx);

/* GPU/stats section of the sidecar; implemented in d3d11_backend.c. */
void halo2_debug_sidecar_gpu(FILE *f);

/* Settings and per-category counters for the last two frames into the
 * sidecar, plus the full per-draw list to <png>.draws.csv (d3d11_backend.c). */
void halo2_debug_drawlist_write(const char *png_path, FILE *sidecar);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_DEBUG_TOOLS */

#endif
