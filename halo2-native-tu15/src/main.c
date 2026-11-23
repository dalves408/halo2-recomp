/**
 * Halo 2 - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       Halo 2
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include <kernel/nv2a_backend.h>
#include <usb/ohci.h>
#include <kernel/nvnet.h>
#include "h2_layout.h"
#include "gpu/d3d11_backend.h"
#include "settings.h"
#include "overlay.h"
#include "perf_counters.h"
#include "perf_sampler.h"
#include "kernel/recomp_perf.h"
#include "h2_frame.h"
#include "h2_mouse.h"

/* Hybrid laptops: ask the NVIDIA (Optimus) and AMD (PowerXpress) drivers to run
 * this process on the discrete GPU. The D3D11 backend also asks DXGI for the
 * high-performance adapter; RECOMP_GPU_ADAPTER overrides both. */
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#ifdef H2_DOWNLOADER
/* The disc's content downloader (Downloader.xbe, the same title id), built as
 * its own program; the game hands over to it with XLaunchNewImage. */
#define YOUR_GAME_ENTRY_POINT   0x0001BEFF  /* downloader.xbe entry point */
#define YOUR_GAME_XBE_PATH      "game/downloader.xbe"
#else
#define YOUR_GAME_ENTRY_POINT   0x002DA9DE  /* XBE entry point VA (TU 1.5 / cert 0x803; base 1.0 was 0x002D0AEE) */
#define YOUR_GAME_XBE_PATH      "game/default.xbe"
#endif
#define YOUR_GAME_DIR           "game"

/* Halo 2's XDK Direct3D device and its completion fields. */
#ifdef H2_DOWNLOADER
#define HALO2_D3D_DEVICE_PTR    0x0004F4C8u /* downloader.xbe: matched to the game's 0x00411568 by code bytes */
#else
#define HALO2_D3D_DEVICE_PTR    0x00411568u /* TU 1.5 (base 1.0: 0x00407488; D3D section moved +0xA0E0) */
#endif
#define HALO2_D3D_FENCE_GET_PTR 0x30u
#define HALO2_D3D_FENCE_EVENT  0x1DCCu
#define HALO2_D3D_FENCE_NOTIFY 5u

static int halo2_gpu_semaphore(uint32_t value)
{
    uint32_t device = *(const volatile uint32_t *)
        ((uintptr_t)g_xbox_mem_offset + HALO2_D3D_DEVICE_PTR);
    if (!device)
        return 0;
    uint32_t address = *(const volatile uint32_t *)
        ((uintptr_t)g_xbox_mem_offset + device + HALO2_D3D_FENCE_GET_PTR);
    if (address < 0x80000000u || address > 0x83FFFFFCu || (address & 3u))
        return 0;
    bruteforce_d3d11_release(address, value);
    return 1;
}

static int halo2_gpu_software_method(uint32_t value)
{
    if (value != HALO2_D3D_FENCE_NOTIFY)
        return 0;
    uint32_t device = *(const volatile uint32_t *)
        ((uintptr_t)g_xbox_mem_offset + HALO2_D3D_DEVICE_PTR);
    bruteforce_d3d11_finish();
    return device && xbox_KeSignalGuestEvent(device + HALO2_D3D_FENCE_EVENT);
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static volatile LONG s_crash_report_written;

static void halo2_write_crash_report(PEXCEPTION_POINTERS ep)
{
    char exe[MAX_PATH], dmp[MAX_PATH], logp[MAX_PATH];
    char *slash;
    HANDLE file;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    FILE *f;
    const Halo2Settings *s;

    if (InterlockedExchange(&s_crash_report_written, 1) != 0)
        return;
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
        return;
    slash = strrchr(exe, '\\');
    if (slash)
        slash[1] = 0;
    else
        exe[0] = 0;
    snprintf(dmp, sizeof dmp, "%shalo2_crash.dmp", exe);
    snprintf(logp, sizeof logp, "%shalo2_crash.log", exe);
    f = fopen(logp, "w");
    if (f) {
        fprintf(f, "Halo 2 Native crash report\n");
        if (ep && ep->ExceptionRecord) {
            fprintf(f, "Exception code=0x%08lX address=0x%p\n",
                    (unsigned long)ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress);
        }
        s = halo2_settings_get();
        if (s)
            fprintf(f, "Settings scale=%dx fullscreen=%d vsync=%d aniso=%d fxaa=%d msaa=%d\n",
                    s->resolution_scale, s->fullscreen, s->vsync,
                    s->anisotropy, s->fxaa, s->msaa);
        if (halo2_settings_last_apply() && halo2_settings_last_apply()[0])
            fprintf(f, "Last live apply: %s\n", halo2_settings_last_apply());
        fclose(f);
    }
    file = CreateFileA(dmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        memset(&mei, 0, sizeof mei);
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MiniDumpNormal, ep ? &mei : NULL, NULL, NULL);
        CloseHandle(file);
        fprintf(stderr, "[CRASH] wrote %s and %s\n", dmp, logp);
        fflush(stderr);
    }
}

static int halo2_is_fatal_exception(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case 0xC0000374: /* STATUS_HEAP_CORRUPTION */
    case 0xC0000409: /* STATUS_STACK_BUFFER_OVERRUN / fail-fast */
        return 1;
    default:
        return 0;
    }
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Unwind native frames even when a fault overwrote the guest stack. */
        {
            CONTEXT ctx = *ep->ContextRecord;
            fprintf(stderr, "  Native stack:\n");
            for (unsigned i = 0; i < 10 && ctx.Rip && ctx.Rsp; ++i) {
                DWORD64 image = 0, displacement = 0, frame = 0;
                PVOID handler_data = NULL;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
                union { SYMBOL_INFO info; char buffer[sizeof(SYMBOL_INFO) + 256]; } symbol = {0};
                symbol.info.SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol.info.MaxNameLen = 255;
                if (SymFromAddr(GetCurrentProcess(), ctx.Rip, &displacement, &symbol.info))
                    fprintf(stderr, "    %s+0x%llX\n", symbol.info.Name, displacement);
                else
                    fprintf(stderr, "    0x%llX\n", ctx.Rip);
                if (function) {
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, function, &ctx,
                                     &handler_data, &frame, NULL);
                } else {
                    SIZE_T bytes;
                    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)ctx.Rsp,
                                           &ctx.Rip, sizeof ctx.Rip, &bytes))
                        break;
                    ctx.Rsp += sizeof ctx.Rip;
                }
            }
        }
        fflush(stderr);
        halo2_write_crash_report(ep);
    } else if (halo2_is_fatal_exception(ep->ExceptionRecord->ExceptionCode)) {
        fprintf(stderr, "[CRASH] exception 0x%08lX at RIP=0x%llX\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                (unsigned long long)ep->ContextRecord->Rip);
        fflush(stderr);
        halo2_write_crash_report(ep);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}


/* RECOMP_HALO2_DSP_PROBE=1 (analysis/perf): DirectSound's work routine
 * (0x00388CCB in the 1.5 XBE) resets the GP/EP DSPs -- with a 10 ms
 * KeStallExecutionProcessor -- whenever the dword at *(0x3911E8) is not
 * 0x00CCCCCC. Print the pointer and the word every 2 s. */
static DWORD WINAPI halo2_dsp_probe(LPVOID arg)
{
    (void)arg;
    for (;;) {
        uint32_t ptr = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x3911E8u);
        uint32_t val = 0xDEADBEEFu;
        __try {
            if (ptr) val = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + ptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { val = 0xBADBADu; }
        fprintf(stderr, "[DSP-PROBE] *0x3911E8 = 0x%08X -> 0x%08X\n", ptr, val);
        Sleep(2000);
    }
    return 0;
}

int g_halo2_single_vblank;

#ifndef H2_DOWNLOADER
/* The frame rate: 60 or 30 fps (g_halo2_single_vblank: a frame targets the
 * next vblank instead of every second one, recomp_0029.c).
 *
 * The setting (settings.h frame_rate, F1 menu and Config.exe):
 *   - 60: always 60.
 *   - 30: always 30, the console's own cadence.
 *   - Auto (default): 60, until this PC shows it cannot hold 60 in play; then
 *     30 for the rest of the session. A PC that can't keep up gives an uneven
 *     40-55 fps, which reads worse than an even 30 (one frame per simulation
 *     tick) and asks twice the GPU work. Auto watches 5-second windows and
 *     counts only those that draw a scene (100+ draws a frame: not the intro
 *     video or loading screens, which run near 30 on any PC). Time spent
 *     building shaders is taken out of a window before its rate is worked out,
 *     and a window that is mostly building is skipped: a first run on a slow
 *     CPU builds shaders in nearly every window (180 at 85-235 ms each on an
 *     i5-8250U), so skipping every window with builds never decided. A fast PC
 *     whose only dips are shader builds stays at 60. When all of the last 4
 *     such windows are under 52 fps and they average under 45, it goes to 30
 *     (3 of 4 under 50 dropped the test laptop's GTX 1650, which holds 60, to
 *     30 while a level streamed in: 48.8, 59.0, 48.1 fps). It
 *     never goes back up by itself: switching back and forth would look worse
 *     than either rate. Measured 2026-11-22 on the test laptop's Intel UHD at
 *     1x: 38-55 fps in play, 5-13 ms a frame waiting for the GPU; its GTX 1650
 *     held 60 with 1-3 ms. Choosing a setting again in the F1 menu starts over.
 *
 * Over everything: 30 fps in an Xbox Live match. Halo 2 simulates 30 ticks a
 * second whatever the frame rate (0x004F2C5C counts them). At 60 fps a frame
 * is not tied to a tick: measured in an Insignia match, a quarter of the
 * ticks were shown for 1 frame and a quarter for 3 (2026-11-19), which reads
 * as jitter. Only a match counts: the router seeing game traffic on UDP 3074
 * (8 or more packets in a second, nvnet_nat.c), not a signed-in console's
 * keep-alives with the security gateway. Campaign and system link (which
 * never goes through the router) follow the setting; 10 s after the match
 * traffic stops the setting applies again. RECOMP_HALO2_LIVE_60=1 keeps the
 * setting in Live matches too; RECOMP_HALO2_SINGLE_VBLANK=0 forces 30. */
int nat_live_match_recent(unsigned ms);   /* toolkit nvnet_nat.c */
uint32_t halo2_gpu_flip_count(void);      /* gpu/d3d11_backend.c */
uint32_t halo2_gpu_draw_count(void);
double halo2_gpu_compile_ms(void);

static volatile LONG s_fps_rate = 60, s_fps_auto_low;
static int s_fps_forced30;

int halo2_fps_current(int *auto_low)
{
    if (auto_low)
        *auto_low = (int)s_fps_auto_low;
    return (int)s_fps_rate;
}

static int fps_setting(void)
{
    const Halo2Settings *st = halo2_settings_get();
    return st && st->frame_rate >= 0 && st->frame_rate <= 2 ? st->frame_rate : 0;
}

static void fps_apply(int setting, int in_match)
{
    int want60 = !s_fps_forced30 && !in_match &&
                 (setting == 1 || (setting == 0 && !s_fps_auto_low));
    g_halo2_single_vblank = want60;
    InterlockedExchange(&s_fps_rate, want60 ? 60 : 30);
}

#define FPS_WINDOW_MS 5000
#define FPS_WINDOWS 4
static DWORD WINAPI halo2_fps_policy(LPVOID unused)
{
    const char *live60 = getenv("RECOMP_HALO2_LIVE_60");
    int live_rule = getenv("RECOMP_NET") && !(live60 && *live60 == '1');
    int setting_was = fps_setting(), in_match_was = 0, n = 0;
    double hist[FPS_WINDOWS];
    uint32_t f0 = halo2_gpu_flip_count(), d0 = halo2_gpu_draw_count();
    double c0 = halo2_gpu_compile_ms();
    DWORD t0 = GetTickCount();
    (void)unused;
    for (;;) {
        int setting, in_match;
        DWORD now;
        Sleep(500);
        setting = fps_setting();
        in_match = live_rule && nat_live_match_recent(10000);
        now = GetTickCount();
        if (halo2_overlay_active()) {            /* F1 or the shader build: paused, not slow */
            f0 = halo2_gpu_flip_count(); d0 = halo2_gpu_draw_count(); c0 = halo2_gpu_compile_ms(); t0 = now;
            fps_apply(setting, in_match);
            continue;
        }
        if (setting != setting_was || in_match != in_match_was) {
            if (setting != setting_was) {
                InterlockedExchange(&s_fps_auto_low, 0);
                fprintf(stderr, "[FPS] frame rate set to %s\n",
                        setting == 1 ? "60" : setting == 2 ? "30" : "Auto");
            }
            if (in_match != in_match_was)
                fprintf(stderr, "[FPS] %s\n", in_match ? "Xbox Live match: 30 fps (one frame per simulation tick)"
                                                       : "no Xbox Live match: the frame rate setting applies again");
            setting_was = setting;
            in_match_was = in_match;
            n = 0;                               /* judge afresh */
            f0 = halo2_gpu_flip_count(); d0 = halo2_gpu_draw_count(); c0 = halo2_gpu_compile_ms(); t0 = now;
        } else if (now - t0 >= FPS_WINDOW_MS) {
            uint32_t f = halo2_gpu_flip_count(), d = halo2_gpu_draw_count();
            double c = halo2_gpu_compile_ms(), secs = (now - t0) / 1000.0;
            double play = secs - (c - c0) / 1000.0;     /* the window less its shader builds */
            if (setting == 0 && !s_fps_auto_low && !in_match && !s_fps_forced30 && f != f0 &&
                (double)(d - d0) / (f - f0) >= 100.0 && play >= secs / 2.0) {
                int i, slow = 0;
                double sum = 0.0;
                hist[n % FPS_WINDOWS] = (f - f0) / play;
                n++;
                for (i = 0; i < FPS_WINDOWS && i < n; i++) {
                    sum += hist[i];
                    slow += hist[i] < 52.0;
                }
                if (n >= FPS_WINDOWS && slow == FPS_WINDOWS && sum / FPS_WINDOWS < 45.0) {
                    InterlockedExchange(&s_fps_auto_low, 1);
                    fprintf(stderr, "[FPS] Auto: %.1f fps on average over the last %d s of play;"
                                    " 30 fps for the rest of this session\n",
                            sum / FPS_WINDOWS, FPS_WINDOWS * FPS_WINDOW_MS / 1000);
                }
            }
            f0 = f; d0 = d; c0 = c; t0 = now;
        }
        fps_apply(setting, in_match);
    }
}
#else
/* The downloader draws its own screens at its own rate; its F1 menu shows 60. */
int halo2_fps_current(int *auto_low)
{
    if (auto_low)
        *auto_low = 0;
    return 60;
}
#endif

int main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    h2_release_startup();          /* an installed game started from its exe (src/h2_release.c) */
    h2_diag_start_log();           /* data\logs\halo2-<date>-<time>.log, [BUILD] (src/h2_diag.c) */

#ifndef H2_DOWNLOADER
    {   /* The process the player started is only the window (h2_frame.c): the
         * game and its downloader run inside it as their own processes. */
        int frame_exit = 0;
        if (h2_frame_main(&frame_exit))
            return frame_exit;
    }
#endif
    {   /* Inside the frame: draw into it (toolkit fb_present.c, RECOMP_FB_PARENT). */
        const char *frame = getenv("H2_FRAME_HWND");
        if (frame && *frame && !getenv("RECOMP_FB_PARENT"))
            _putenv_s("RECOMP_FB_PARENT", frame);
    }
    {   /* Windows 11 throttles a process with no visible top-level window of
         * its own, which a process drawing inside the frame is: it ignores the
         * 1 ms timer resolution and runs on efficiency settings. CounterStrikeXB
         * froze within a minute that way; opt out of both. */
        PROCESS_POWER_THROTTLING_STATE pt;
        memset(&pt, 0, sizeof pt);
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    }

    const char *single_vblank = getenv("RECOMP_HALO2_SINGLE_VBLANK");
    /* 60 FPS by default (frame target one vblank instead of two); game speed
     * checked at normal in Cairo 2026-11-18. RECOMP_HALO2_SINGLE_VBLANK=0
     * restores the retail 30 FPS cadence. */
    g_halo2_single_vblank = !(single_vblank && strcmp(single_vblank, "0") == 0);

    if (!getenv("RECOMP_VBLANK")) _putenv_s("RECOMP_VBLANK", "paced");
    if (!getenv("RECOMP_PB_EXEC")) _putenv_s("RECOMP_PB_EXEC", "1");
    if (!getenv("RECOMP_PB_WALK")) _putenv_s("RECOMP_PB_WALK", "dma");
    if (!getenv("RECOMP_AC97_READY")) _putenv_s("RECOMP_AC97_READY", "1");
    if (!getenv("RECOMP_TITLE_KEVENTS")) _putenv_s("RECOMP_TITLE_KEVENTS", "1");
    if (!getenv("RECOMP_DISPATCH_GATE")) _putenv_s("RECOMP_DISPATCH_GATE", "1");
    if (!getenv("RECOMP_USB")) _putenv_s("RECOMP_USB", "1");
#ifdef H2_DOWNLOADER
    /* The disc downloader's XAPI takes its controllers from the bring-up scan
     * (ohci.c, RECOMP_USB_PLUG). Marked as the downloader's own, because the
     * environment goes with the hand-over back to the game. */
    if (!getenv("RECOMP_USB_PLUG")) {
        _putenv_s("RECOMP_USB_PLUG", "boot");
        _putenv_s("H2_USB_PLUG_SET_BY_DOWNLOADER", "1");
    }
    /* One pad in the downloader, as before (the game's four come with the hand-over in the environment). */
    if (getenv("H2_USB_PADS_SET_BY_GAME"))
        _putenv_s("RECOMP_USB_PADS", "1");
#else
    if (getenv("H2_USB_PLUG_SET_BY_DOWNLOADER")) {   /* back from the downloader */
        _putenv_s("RECOMP_USB_PLUG", "");
        _putenv_s("H2_USB_PLUG_SET_BY_DOWNLOADER", "");
    }
    /* Four controller ports, as on the console: split screen and co-op need a pad on each player's port
     * (ohci.c, RECOMP_USB_PADS; the default was one). A port with no host pad reads as a pad at rest; host pads
     * take ports in order (overrides/xinput_device.c). */
    if (!getenv("RECOMP_USB_PADS") || getenv("H2_USB_PADS_SET_BY_GAME")) {
        _putenv_s("RECOMP_USB_PADS", "4");
        _putenv_s("H2_USB_PADS_SET_BY_GAME", "1");
    }
#endif
    /* fix3: 8 x 512-sample device buffers (pacing 2048..4096 queued, 43-85 ms)
     * instead of 4. The sequencer hold (apu_core.c, APU_SEQ_HOLD_US) renders
     * nothing for 5-10 ms at each SSL interrupt; with the default 1024-sample
     * low mark the device ran dry ~0.3/s in menus and ~1/s in Outskirts. */
    if (!getenv("RECOMP_AUDIO_BUF_COUNT")) _putenv_s("RECOMP_AUDIO_BUF_COUNT", "8");
    /* Reuse the temporary D3D boot buffers before allocating visibility reports. */
    if (!getenv("RECOMP_HEAP_RECLAIM")) _putenv_s("RECOMP_HEAP_RECLAIM", "1");
    /* Keep OHCI descriptors away from physical addresses that alias the XBE. */
    if (!getenv("RECOMP_CONTIG_AVOID_IMAGE")) _putenv_s("RECOMP_CONTIG_AVOID_IMAGE", "1");

    halo2_settings_boot();
    {   /* The player's keys: SaveData\keymap.ini, else the install's keymap.ini. */
        char root[MAX_PATH], km[MAX_PATH];
        halo2_install_root(root, sizeof root);
        h2_data_path(root, "keymap.ini", km, sizeof km);
        if (GetFileAttributesA(km) == INVALID_FILE_ATTRIBUTES)
            snprintf(km, sizeof km, "%s\\keymap.ini", root);
        if (!getenv("RECOMP_KEYMAP") && GetFileAttributesA(km) != INVALID_FILE_ATTRIBUTES)
            _putenv_s("RECOMP_KEYMAP", km);
    }
#ifndef H2_DOWNLOADER
    h2_mouse_init();          /* mouse aim, off on Xbox Live (src/h2_mouse.c) */
#endif
    halo2_overlay_start();

    const char *renderer = getenv("RECOMP_GPU");
    if (renderer && strcmp(renderer, "d3d11") == 0) {
        if (!getenv("RECOMP_GPU_PHYSICAL_TEXTURE_READS"))
            _putenv_s("RECOMP_GPU_PHYSICAL_TEXTURE_READS", "1");
        if (!bruteforce_perf_counters_init() || !bruteforce_d3d11_install())
            return 1;
    } else if (renderer && strcmp(renderer, "cpu") != 0) {
        fprintf(stderr, "RECOMP_GPU must be d3d11 or cpu.\n");
        return 1;
    }

    printf("=== Halo 2 - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    perf_sampler_start();   /* RECOMP_PROF=<report> (analysis/perf) */
    rpz_init_main_thread(); /* RECOMP_PERF_CSV=<csv> (analysis/perf) */

    /* Step 1: Load XBE: game_files\ in an install, game\ in a development tree (h2_layout.h) */
    {
        char xbe_path[64];
        const char *xbe_name = strrchr(YOUR_GAME_XBE_PATH, '/') + 1;
        snprintf(xbe_path, sizeof xbe_path, "%s/%s", h2_game_name(), xbe_name);
        if (!load_xbe(xbe_path, &xbe_data, &xbe_size)) {
            return 1;
        }
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fputs("Failed to initialize Xbox memory layout.\n", stderr);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    nv2a_pb_set_semaphore_handler(halo2_gpu_semaphore);
    nv2a_pb_set_software_method_handler(halo2_gpu_software_method);
    {
        /* Toolkit opt-in (off for 1.0): pack INLINE_ARRAY attributes at
         * their element size. Halo 2's particle quads are 8-byte S32K
         * vertices; the dword-padded layout dropped every one. */
        extern void nv2a_pb_set_inline_packed_layout(int enable);
        nv2a_pb_set_inline_packed_layout(getenv("RECOMP_INLINE_PACKED")
                                         ? atoi(getenv("RECOMP_INLINE_PACKED")) != 0 : 1);
    }

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        extern void xbox_path_set_ephemeral_cache_maps(int enable);
        extern void xbox_path_set_in_memory_partitions(int enable);
        /* Cache maps (cache000.map..) stay in a per-session temp directory
         * (ephemeral): leaving them in SaveData dropped the intro to ~8-9 FPS.
         * Partition devices are emulated in memory: no PartitionN.img is
         * written. Only DiskConfig.bin (8 KB of console config sectors) plus
         * fonts, preferences, savegames, UDATA and TDATA persist in SaveData. */
        xbox_path_set_ephemeral_cache_maps(1);
        xbox_path_set_in_memory_partitions(1);
        xbox_path_init(h2_game_name(), h2_data_name());
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();
    xbox_OhciInit();
    /* The MCPX Ethernet controller: System Link, and Xbox Live through
     * Insignia with the player's own console identity (RECOMP_NET=1). */
    xbox_NvnetInit();
#ifndef H2_DOWNLOADER
    s_fps_forced30 = !g_halo2_single_vblank;     /* RECOMP_HALO2_SINGLE_VBLANK=0 */
    fps_apply(fps_setting(), 0);
    CloseHandle(CreateThread(NULL, 0, halo2_fps_policy, NULL, 0, NULL));
#endif

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    if (getenv("RECOMP_HALO2_DSP_PROBE"))
        CloseHandle(CreateThread(NULL, 0, halo2_dsp_probe, NULL, 0, NULL));

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}
