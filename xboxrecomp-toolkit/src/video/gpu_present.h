/* D3D11 presentation for the framebuffer window (see gpu_present.c). */
#ifndef XBOX_GPU_PRESENT_H
#define XBOX_GPU_PRESENT_H
#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

int  gpu_present_enabled(void);                 /* RECOMP_D3D_PRESENT=1 */
int  gpu_present_init(HWND hwnd, uint32_t width, uint32_t height);
/* width*height B8G8R8A8; allow_shared must be 0 for a CPU-composited overlay
 * or a pinned framebuffer. Returns 1 only when the present waited for vsync. */
int  gpu_present_frame(const uint32_t *pixels, int allow_shared, uint64_t frame_id);
enum GpuPresentRoute {
    GPU_PRESENT_NO_INPUT,
    GPU_PRESENT_CPU_UPLOAD,
    GPU_PRESENT_SHARED_REJECTED
};
enum GpuPresentStage {
    GPU_PRESENT_NO_SURFACE,
    GPU_PRESENT_RESIZE_FAILED,
    GPU_PRESENT_TARGET_FAILED,
    GPU_PRESENT_CALLED,
    GPU_PRESENT_INCOMPATIBLE
};
typedef struct GpuPresentTrace {
    enum GpuPresentRoute route;
    enum GpuPresentStage stage;
    HRESULT result;
    HRESULT shared_release;
} GpuPresentTrace;
/* Diagnostic variant: shared input is rejected, never substituted with a
 * CPU frame. Any acquired shared mutex is returned before it reports that. */
int  gpu_present_frame_capture(const uint32_t *pixels, int allow_shared,
                               uint64_t frame_id, GpuPresentTrace *trace);
/* One keyed-mutex texture may be submitted at a time. The producer owns key
 * 0, copies the finished frame and releases key 1 before submitting. The
 * presenter acquires key 1 and releases key 0 after using or discarding it. */
int  gpu_present_shared_ready(uint32_t width, uint32_t height);
/* Current 4:3 picture rectangle inside the framebuffer window. */
int  gpu_present_output_size(uint32_t *width, uint32_t *height);
/* The shape the picture is shown at (width / height): 4:3 by default; a
 * title rendering widescreen sets 16:9 or 16:10. */
void  gpu_present_set_picture_aspect(float aspect);
float gpu_present_picture_aspect(void);
uint64_t xbox_FramebufferWindowNextFrameId(void);
int  gpu_present_submit_shared(HANDLE handle, uint32_t width, uint32_t height,
                               uint64_t frame_id);
int  gpu_present_active(void);
void gpu_present_shutdown(void);
/* Live VSync for the Halo 2 1.5 settings menu. No effect until the next Present. */
void gpu_present_set_vsync(int enabled);
int  gpu_present_get_vsync(void);
#endif
#endif
