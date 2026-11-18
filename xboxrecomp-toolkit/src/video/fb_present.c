/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "gpu_present.h"
#include "display_awake.h"
#include "present_capture.h"
#include "recomp_exit_hook.h"   /* src/platform */
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"
#include "../kernel/kernel.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static HWND          s_fb_hwnd;
static DWORD         s_fb_tid;
static int           s_fb_fullscreen;
static RECT          s_fb_saved;
static LONG_PTR      s_fb_saved_style;
#define WM_RECOMP_SET_FULLSCREEN (WM_APP + 40)
static __declspec(thread) RecompDisplayAwake s_display_awake;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * The lock also protects the window's copy: two flips can otherwise reuse
 * the buffer the window is still reading. */
static uint32_t     *s_present[2];
static LONG          s_present_idx = -1;   /* -1 until the first flip */
static uint64_t      s_present_id;
static SRWLOCK       s_present_lock = SRWLOCK_INIT;

typedef struct FbPresentInfo {
    uint64_t id;
    uint32_t pitch, bpp, aa_sy;
    RecompPresentTag tag;
} FbPresentInfo;

static FbPresentInfo s_present_info;
static RecompPresenterCapture s_capture;
static SRWLOCK s_fb_config_lock = SRWLOCK_INIT;

static int fb_capture_requested(void)
{
    const char *path = getenv("RECOMP_PRESENTER_CAPTURE");
    return path && *path;
}

static void fb_capture_fail(const char *message)
{
    fprintf(stderr, "  [PRESCAP] %s; capture incomplete, terminating\n", message);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    abort();
}

static int fb_readable_surface(uint32_t va, uint32_t pitch, uint32_t bpp,
                               uint32_t aa_sy)
{
    uintptr_t begin, end, at;
    uint64_t bytes, row_stride, row_bytes, guest_end;

    if (!va || !pitch || !s_fb_width || !s_fb_height)
        return 0;
    if (bpp != 2 && bpp != 4 && bpp != 8)
        return 1;                       /* the presenter fills black */
    row_stride = (uint64_t)pitch * (bpp == 8 ? aa_sy : 1);
    row_bytes = (uint64_t)s_fb_width * bpp;
    if (s_fb_height > 1 && row_stride > (UINT64_MAX - row_bytes) / (s_fb_height - 1))
        return 0;
    bytes = (uint64_t)(s_fb_height - 1) * row_stride + row_bytes;
    guest_end = (uint64_t)va + bytes;
    if (guest_end < bytes ||
        !(guest_end <= XBOX_CONTIG_SIZE ||
          ((uint64_t)va >= XBOX_CONTIG_BASE &&
           guest_end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)))
        return 0;
    begin = (uintptr_t)va + (uintptr_t)xbox_GetMemoryOffset();
    if (!bytes || bytes > UINTPTR_MAX - begin)
        return 0;
    end = begin + (uintptr_t)bytes;
    for (at = begin; at < end;) {
        MEMORY_BASIC_INFORMATION m;
        uintptr_t next;
        DWORD access;
        if (!VirtualQuery((const void *)at, &m, sizeof m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return 0;
        access = m.Protect & 0xffu;
        if (access != PAGE_READONLY && access != PAGE_READWRITE &&
            access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READ &&
            access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
            return 0;
        next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= at)
            return 0;
        at = next;
    }
    return 1;
}

/* The last PVIDEO frame shown. A frame stays on screen until the next one or
 * STOP, but once its bank is consumed the title may reuse the memory, so the
 * pixels are kept here rather than read again. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

/* Draw the video overlay over s_rgb: a newly submitted bank if there is one
 * (copied, drawn, then handed back to the title), else the last frame. */
static int fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;

    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return 0;
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return 0;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & NV_PVIDEO_FORMAT_PITCH;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT ||
            (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            bytes > mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress))
            continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        if (!pvideo_scanout(regs, snapshot, (size_t)bytes, s_rgb,
                            s_fb_width, s_fb_height, bank)) {
            free(snapshot);
            continue;
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        presented = true;
        nv2a_hook_pvideo_consume(bank);
    }
    if (!presented && s_pvideo_source)
        presented = pvideo_scanout(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                                   s_fb_width, s_fb_height, s_pvideo_bank) != 0;
    return presented;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");
    int diagnostic = fb_capture_requested();

    if (diagnostic)
        AcquireSRWLockExclusive(&s_fb_config_lock);
    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
    if (diagnostic)
        ReleaseSRWLockExclusive(&s_fb_config_lock);
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    int diagnostic = fb_capture_requested();
    if (diagnostic)
        AcquireSRWLockExclusive(&s_fb_config_lock);
    s_fb_aa_sy = sy ? sy : 1;
    if (diagnostic)
        ReleaseSRWLockExclusive(&s_fb_config_lock);
}

/* Called by the pushbuffer executor when the title flips. */
static void fb_publish(uint32_t fb_va, uint32_t pitch, const RecompPresentTag *tag)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y, aa_sy = 0;
    int diagnostic = tag != NULL || fb_capture_requested();

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    bpp = pitch / s_fb_width;
    if (diagnostic) {
        AcquireSRWLockShared(&s_fb_config_lock);
        aa_sy = s_fb_aa_sy;
        ReleaseSRWLockShared(&s_fb_config_lock);
    }
    if (diagnostic && !fb_readable_surface(fb_va, pitch, bpp, aa_sy))
        fb_capture_fail("published framebuffer input is not readable");
    AcquireSRWLockExclusive(&s_present_lock);
    if (diagnostic && s_present_id == UINT64_MAX) {
        ReleaseSRWLockExclusive(&s_present_lock);
        fb_capture_fail("publication ID exhausted");
    }
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next]) {
            ReleaseSRWLockExclusive(&s_present_lock);
            if (diagnostic)
                fb_capture_fail("cannot allocate a published frame");
            fprintf(stderr, "  [FBWIN] cannot allocate a presentation frame\n");
            return;
        }
    }
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch
                           * (bpp == 8 ? (diagnostic ? aa_sy : s_fb_aa_sy) : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    s_present_idx = next;
    s_present_id++;
    s_present_info.id = s_present_id;
    s_present_info.pitch = pitch;
    s_present_info.bpp = bpp;
    s_present_info.aa_sy = diagnostic ? aa_sy : s_fb_aa_sy;
    if (tag)
        s_present_info.tag = *tag;
    else
        memset(&s_present_info.tag, 0, sizeof s_present_info.tag);
    ReleaseSRWLockExclusive(&s_present_lock);
}

void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    fb_publish(fb_va, pitch, NULL);
}

void xbox_FramebufferWindowPresentTagged(uint32_t fb_va, uint32_t pitch,
                                         const RecompPresentTag *tag)
{
    if (!tag)
        fb_capture_fail("missing flip provenance");
    fb_publish(fb_va, pitch, tag);
}

uint64_t xbox_FramebufferWindowNextFrameId(void)
{
    uint64_t id;
    AcquireSRWLockShared(&s_present_lock);
    id = s_present_id + 1;
    ReleaseSRWLockShared(&s_present_lock);
    return id;
}

static int fb_copy_presented_frame_info(uint32_t *dst, FbPresentInfo *info)
{
    int copied = 0;

    AcquireSRWLockShared(&s_present_lock);
    if (s_present_idx >= 0) {
        memcpy(dst, s_present[s_present_idx],
               (size_t)s_fb_width * s_fb_height * 4);
        if (info)
            *info = s_present_info;
        copied = 1;
    }
    ReleaseSRWLockShared(&s_present_lock);
    return copied;
}

static int fb_copy_presented_frame_id(uint32_t *dst, uint64_t *frame_id)
{
    FbPresentInfo info;
    int copied = fb_copy_presented_frame_info(dst, frame_id ? &info : NULL);
    if (copied && frame_id)
        *frame_id = info.id;
    return copied;
}

static int fb_copy_presented_frame(uint32_t *dst)
{
    return fb_copy_presented_frame_id(dst, NULL);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    if (!s_fb_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &s_fb_saved);
        s_fb_saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (s_fb_saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        s_fb_fullscreen = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, s_fb_saved_style);
        SetWindowPos(h, NULL, s_fb_saved.left, s_fb_saved.top, s_fb_saved.right - s_fb_saved.left,
                     s_fb_saved.bottom - s_fb_saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        s_fb_fullscreen = 0;
    }
}

static void fb_apply_fullscreen(HWND h, int on)
{
    if ((on != 0) != (s_fb_fullscreen != 0))
        fb_toggle_fullscreen(h);
}

HWND xbox_FramebufferWindowHandle(void)
{
    return s_fb_hwnd;
}

int xbox_FramebufferWindowIsFullscreen(void)
{
    return s_fb_fullscreen;
}

void xbox_FramebufferWindowSetFullscreen(int on)
{
    HWND h = s_fb_hwnd;
    if (!h)
        return;
    if (GetCurrentThreadId() == s_fb_tid)
        fb_apply_fullscreen(h, on != 0);
    else
        PostMessageA(h, WM_RECOMP_SET_FULLSCREEN, on ? 1 : 0, 0);
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_RECOMP_SET_FULLSCREEN) {
        fb_apply_fullscreen(h, w ? 1 : 0);
        return 0;
    }
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    switch (m) {
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SIZE:
    case WM_SHOWWINDOW:
        recomp_display_awake_update(&s_display_awake, h);
        break;
    case WM_CLOSE:
    case WM_DESTROY:
        recomp_display_awake_shutdown(&s_display_awake);
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            if (s_capture.active && !recomp_capture_close(&s_capture, "window_closed"))
                fb_capture_fail("cannot close capture");
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. Autorepeat
             * sets a Shift still down again. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Mouse buttons go in the same table, for bindings that name them.
     * Capturing the mouse while one is down delivers its release even
     * outside the window. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert_mode(const uint8_t *src, uint32_t bpp, int fixed,
                            uint32_t pitch, uint32_t aa_sy)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * (fixed ? pitch : s_fb_pitch)
                           * (bpp == 8 ? (fixed ? aa_sy : s_fb_aa_sy) : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    fb_convert_mode(src, bpp, 0, 0, 0);
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* GDI: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
typedef struct FbGdiTrace {
    int called, result;
    DWORD error;
} FbGdiTrace;

static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi, FbGdiTrace *trace)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    BOOL have_client;
    int result;
    if (trace)
        memset(trace, 0, sizeof *trace);
    have_client = GetClientRect(hwnd, &c);
    if (trace && !have_client) {
        trace->error = GetLastError();
        return;
    }
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);     /* HALFTONE wants it after the mode */
    result = StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                           0, 0, (int)s_fb_width, (int)s_fb_height,
                           s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
    if (trace) {
        trace->called = 1;
        trace->result = result;
        if (!result || result == (int)GDI_ERROR)
            trace->error = GetLastError();
    }
}

/* The converted frame in s_rgb, on screen: D3D11 when it is up (which waits
 * for vsync, so returns 1 when the loop is already paced), else GDI. */
static int fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi, uint64_t frame_id,
                   const FbPresentInfo *info, enum RecompCaptureOrigin origin,
                   int pinned, int overlay)
{
    RecompCaptureRecord record;
    int paced = 0, reject_shared = 0;
    if (!s_capture.active) {
        if (gpu_present_active())
            return gpu_present_frame(s_rgb, !getenv("RECOMP_FB_VA") && !s_pvideo_source,
                                     frame_id);
        gdi_blit(hwnd, hdc, bi, NULL);
        return 0;
    }
    memset(&record, 0, sizeof record);
    record.origin = origin;
    record.pinned = pinned != 0;
    record.overlay = overlay != 0;
    record.width = s_fb_width;
    record.height = s_fb_height;
    if (info) {
        record.pitch = info->pitch;
        record.bpp = info->bpp;
        record.aa_sy = info->aa_sy;
        if (origin == RECOMP_CAPTURE_FLIP) {
            record.publication_id = info->id;
            if (!pinned)
                record.tag = info->tag;
        }
    }
    if (!recomp_capture_clock(&record.start_ns))
        fb_capture_fail("cannot read the monotonic clock");
    if (gpu_present_active()) {
        GpuPresentTrace trace;
        paced = gpu_present_frame_capture(s_rgb,
                    !getenv("RECOMP_FB_VA") && !s_pvideo_source, frame_id, &trace);
        record.route = RECOMP_CAPTURE_D3D_CPU;
        if (trace.route == GPU_PRESENT_SHARED_REJECTED) {
            record.route = RECOMP_CAPTURE_D3D_SHARED_REJECTED;
            record.result = RECOMP_CAPTURE_SHARED_REJECTED;
            record.detail = (uint32_t)trace.shared_release;
            reject_shared = 1;
        } else if (trace.stage == GPU_PRESENT_CALLED &&
                   trace.route == GPU_PRESENT_CPU_UPLOAD) {
            record.image = s_rgb;
            record.detail = (uint32_t)trace.result;
            record.result = trace.result == S_OK ? RECOMP_CAPTURE_D3D_OK :
                            trace.result == DXGI_STATUS_OCCLUDED ? RECOMP_CAPTURE_D3D_OCCLUDED :
                            FAILED(trace.result) ? RECOMP_CAPTURE_D3D_FAILED :
                                                   RECOMP_CAPTURE_D3D_OTHER_STATUS;
        } else if (trace.route == GPU_PRESENT_NO_INPUT) {
            record.detail = (uint32_t)trace.result;
            record.result = trace.stage == GPU_PRESENT_RESIZE_FAILED ?
                            RECOMP_CAPTURE_D3D_RESIZE_FAILED :
                            trace.stage == GPU_PRESENT_TARGET_FAILED ?
                            RECOMP_CAPTURE_D3D_TARGET_FAILED : RECOMP_CAPTURE_D3D_NO_SURFACE;
        } else {
            fb_capture_fail("inconsistent D3D source selection");
        }
    } else {
        FbGdiTrace trace;
        gdi_blit(hwnd, hdc, bi, &trace);
        record.route = RECOMP_CAPTURE_GDI;
        if (!trace.called) {
            record.result = RECOMP_CAPTURE_GDI_NO_CLIENT;
            record.detail = trace.error;
        } else {
            record.image = s_rgb;
            record.result = trace.result && trace.result != (int)GDI_ERROR ?
                            RECOMP_CAPTURE_GDI_OK : RECOMP_CAPTURE_GDI_FAILED;
            record.detail = (uint32_t)trace.result;
        }
    }
    if (!recomp_capture_clock(&record.end_ns) || !recomp_capture_record(&s_capture, &record))
        fb_capture_fail("cannot record presenter input");
    if (reject_shared)
        fb_capture_fail("shared GPU presenter input is unsupported (not replaced by CPU input)");
    return paced;
}

static void fb_capture_missing_input(int pinned)
{
    RecompCaptureRecord record;
    memset(&record, 0, sizeof record);
    record.origin = RECOMP_CAPTURE_MISSING;
    record.pinned = pinned != 0;
    record.route = RECOMP_CAPTURE_NO_ROUTE;
    record.result = RECOMP_CAPTURE_NO_INPUT;
    if (!recomp_capture_clock(&record.start_ns) ||
        !recomp_capture_clock(&record.end_ns) ||
        !recomp_capture_record(&s_capture, &record))
        fb_capture_fail("cannot record missing presenter input");
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned, missing_noted = 0;
    const char *capture_path = getenv("RECOMP_PRESENTER_CAPTURE");

    (void)unused;
    recomp_display_awake_init(&s_display_awake, getenv("RECOMP_DISPLAY_AWAKE"), NULL);

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        if (capture_path && *capture_path)
            fb_capture_fail("cannot create the framebuffer window");
        return 0;
    }
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        s_fb_hwnd = hwnd;
        s_fb_tid = GetCurrentThreadId();
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    /* A window pinned to one address shows that memory and nothing else. */
    fb_pinned = getenv("RECOMP_FB_VA") != NULL;
    if (capture_path && *capture_path) {
        if (!hdc || !s_rgb)
            fb_capture_fail("cannot allocate the presenter input");
        if (!recomp_capture_open(&s_capture, capture_path))
            fb_capture_fail("capture directory must be new and writable");
        fprintf(stderr, "  [PRESCAP] enabled: synchronous capture perturbs timing; "
                        "submissions do not prove visible scanout\n");
    }

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0;
        uint64_t frame_id = 0;
        FbPresentInfo info;
        uint32_t capture_va = 0, capture_pitch = 0, capture_aa_sy = 0;
        int overlay = 0;
        memset(&info, 0, sizeof info);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!InterlockedCompareExchange(&s_fb_running, 1, 1))
            break;
        recomp_display_awake_update(&s_display_awake, hwnd);
        if (s_capture.active) {
            AcquireSRWLockShared(&s_fb_config_lock);
            capture_va = s_fb_va;
            capture_pitch = s_fb_pitch;
            capture_aa_sy = s_fb_aa_sy;
            ReleaseSRWLockShared(&s_fb_config_lock);
        }
        if (s_rgb && fb_copy_presented_frame_info(s_rgb, &info)) {
            frame_id = info.id;
            missing_noted = 0;
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            if (!fb_pinned)
                overlay = fb_overlay();
            paced = fb_show(hwnd, hdc, &bi, frame_id, &info,
                            RECOMP_CAPTURE_FLIP, fb_pinned, overlay);
        } else if ((s_capture.active ? capture_va && capture_pitch : s_fb_va && s_fb_pitch)
                   && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src;
            missing_noted = 0;
            if (s_capture.active) {
                uint32_t va = capture_va;
                info.pitch = capture_pitch;
                info.bpp = info.pitch / s_fb_width;
                info.aa_sy = capture_aa_sy;
                if (!fb_readable_surface(va, info.pitch, info.bpp, info.aa_sy))
                    fb_capture_fail("fallback framebuffer input is not readable");
                src = (const uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());
                fb_convert_mode(src, info.bpp, 1, info.pitch, info.aa_sy);
            } else {
                src = (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
                fb_convert(src, s_fb_pitch / s_fb_width);
            }
            if (!fb_pinned)
                overlay = fb_overlay();
            paced = fb_show(hwnd, hdc, &bi, 0, &info,
                            fb_pinned ? RECOMP_CAPTURE_PINNED : RECOMP_CAPTURE_FALLBACK,
                            fb_pinned, overlay);
        } else if (s_capture.active && !missing_noted) {
            fb_capture_missing_input(fb_pinned);
            missing_noted = 1;
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if (!paced)
            Sleep(16);                  /* D3D11's Present waited for vsync */
    }

    recomp_display_awake_shutdown(&s_display_awake);
    if (s_capture.active && !recomp_capture_close(&s_capture, "thread_stopped"))
        fb_capture_fail("cannot close capture");
    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;
    const char *capture = getenv("RECOMP_PRESENTER_CAPTURE");
    const char *direct = getenv("RECOMP_GPU_DIRECT_PRESENT");

    if (!getenv("RECOMP_FB_WINDOW")) {
        if (capture && *capture)
            fb_capture_fail("RECOMP_PRESENTER_CAPTURE requires RECOMP_FB_WINDOW");
        return;
    }
    if (capture && *capture && direct && *direct && strcmp(direct, "0"))
        fb_capture_fail("RECOMP_GPU_DIRECT_PRESENT is incompatible with presenter capture");
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else {
        InterlockedExchange(&s_fb_running, 0);
        if (capture && *capture)
            fb_capture_fail("cannot start the framebuffer window thread");
    }
}

#endif /* _WIN32: POSIX hosts get the SDL2 window in fb_present_sdl.c */
