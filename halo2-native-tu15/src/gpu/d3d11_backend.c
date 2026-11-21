/*
 * Direct3D 11 render back end (RECOMP_GPU=d3d11).
 *
 * Generic parts adapted from Blood Wake's bw_d3d11.c (bloodwake-recomp at
 * e631edddfe56d22344e9f696923aeb6d78a9e7ba, MIT licence; see
 * THIRD_PARTY_NOTICES.md): surfaces, depth buffers, the texture cache, state
 * objects, the vertex ring and the combiner pixel shaders. The generic
 * physical-resolution sizing and resolve passes are adapted separately in
 * render_scale.c. Its edition, widescreen, other visual effects,
 * texture-restoration and title-specific code are not used.
 *
 * The executor (nv2a_pb_exec.c) decodes the pushbuffer and hands this back
 * end clears, screen-space batches (nv2a_backend.h) and transformed batches
 * with the full pixel-pipeline state (nv2a_backend_ext.h). Rendering is
 * GPU-authoritative:
 *   - one render target per guest colour surface and one D24S8 buffer per
 *     zeta surface; colour and eligible experimental depth buffers are
 *     seeded once from guest memory;
 *   - a texture whose address is a colour surface drawn here samples that
 *     render target directly (render to texture without readback);
 *   - other textures are decoded into a cache whose saved input is compared
 *     on every use; an explicitly enabled approximate mode keeps the older
 *     per-frame and sparse checks for performance experiments;
 *   - blend, depth-stencil, rasteriser and sampler states are cached;
 *   - vertices go through one dynamic ring buffer mapped NO_OVERWRITE.
 *
 * Correctness reference: the CPU rasteriser. The defaults reproduce its
 * rules where they are a choice: texture level 0 only, colours and fog
 * interpolated affinely, the surface clip as the only scissor, depth as the
 * title's screen z over the zeta format's range (SET_CLIP_MIN/MAX are not
 * applied). Known differences: depth is D24 rather than the CPU path's float
 * buffer, and stencil state is honoured here while the CPU path ignores it.
 *
 * Vertex programs run on the GPU (draw_vsh): each program becomes a vertex
 * shader (vsh_hlsl.c) compiled on a worker thread; until it is ready the
 * executor transforms the batch on the CPU. Vertex streams arrive as the
 * title's bytes and are converted by the input layout and the shader.
 *
 * At each flip the presented surface is resolved to guest size and read back
 * into guest memory (the executor's readback hook), so the framework's CPU
 * consumers, frame dumps and frame statistics keep working unchanged.
 *
 * RECOMP_D3D11_WARP=1 uses the WARP software device (tests, no GPU needed).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d/d3d8_swizzle.h"
#include "kernel.h"
#include "nv2a_backend.h"
#include "nv2a_backend_ext.h"
#include "platform/host_time.h"
#include "xbox_memory_layout.h"
#include "video/gpu_present.h"
#ifdef RECOMP_DEBUG_TOOLS
#include "screenshot.h"
#include "tex_pack.h"
#endif
#include "d3d11_backend.h"
#include "d3d11_device_status.h"
#include "render_scale.h"
#include "kernel/recomp_perf.h"
#include "rc_hlsl.h"
#include "shader_cache.h"
#include "texture_decode.h"
#include "texture_sampler.h"
#include "fxaa.h"
#include "vsh_hlsl.h"
#include "../perf_counters.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
#include "../settings.h"

#define GUEST(va) ((uint8_t *)((uintptr_t)(va) + xbox_GetMemoryOffset()))

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
enum GuestReadKind {
    GUEST_READ_SURFACE,
    GUEST_READ_DEPTH,
    GUEST_READ_TEXELS,
    GUEST_READ_PALETTE,
    GUEST_READ_KINDS
};
static struct {
    int valid;
    ID3D11PixelShader *ps;
    ID3D11RasterizerState *raster;
    ID3D11BlendState *blend;
    ID3D11DepthStencilState *depth;
    ID3D11SamplerState *samplers[4];
    float factor[4];
    UINT stencil;
} s_fragment;

static int d3d_scale_blit(ID3D11ShaderResourceView *source, UINT source_width, UINT source_height,
                         ID3D11RenderTargetView *target, UINT target_width, UINT target_height,
                         enum GpuScaleBlitMode mode)
{
    s_fragment.valid = 0;
    return gpu_scale_blit(source, source_width, source_height, target, target_width,
                          target_height, mode);
}
static int s_failed;
static int s_warp;
static int s_depth_texture;
static int s_stencil_alias;
static int s_direct_present;
static int s_skip_clean_readback;
/* declined batches by reason: no color surface, begin, guest sync/clip,
 * texture, combiner shader */
static unsigned s_xrej[5];
static int s_xprep_texrej;          /* the last xdraw_prepare failed on a texture */
static unsigned s_texrej_skips;
/* color texture alias rejects: allocation gone, alias uncertain, sync failed,
 * dirty non-exact overlap, dirty overlap beside an exact one, strict bounds, no view */
static unsigned s_arej[7];
static uint32_t s_flip_life;
static uint64_t s_wserial;      /* color write order */
static unsigned s_order_wins;
static unsigned s_sync_order_wins;  /* perf10: raced partial overlaps superseded by a newer exact alias */
static int s_shared_ok;          /* this flip went out through the shared texture */
static int s_tex_mips;
static int s_tex_aniso;
static volatile LONG s_device_thread;
static GpuScaleConfig s_pending_scale;
static int s_scale_pending;

#ifdef BRUTEFORCE_D3D11_TESTING
static const char *s_test_fail_stage;
static unsigned s_test_fail_occurrence;

void bruteforce_d3d11_test_fail(const char *stage, unsigned occurrence)
{
    s_test_fail_stage = stage;
    s_test_fail_occurrence = occurrence;
}

static int scale_test_failure(const char *stage)
{
    if (!s_test_fail_stage || strcmp(stage, s_test_fail_stage) != 0 ||
        !s_test_fail_occurrence || --s_test_fail_occurrence)
        return 0;
    s_test_fail_stage = NULL;
    return 1;
}
#else
static int scale_test_failure(const char *stage) { (void)stage; return 0; }
#endif

void gpu_d3d11_check_device(ID3D11Device *device, HRESULT result, const char *operation)
{
    HRESULT reason;
    if (SUCCEEDED(result) && device && !rpz_ab_off(2)) {
        static ULONGLONG last_ms;
        ULONGLONG now = GetTickCount64();
        if (now - last_ms < 50)
            return;
        last_ms = now;
    }
    reason = device ? ID3D11Device_GetDeviceRemovedReason(device) : S_OK;
    if (FAILED(reason) || result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        fprintf(stderr, "[GPU-D3D11] device lost during %s: result 0x%08lX,"
                        " removed reason 0x%08lX; recovery is not implemented,"
                        " stopping rather than completing outstanding GPU work\n",
                operation, (unsigned long)result, (unsigned long)reason);
        fflush(stderr);
        abort();
    }
}

HRESULT gpu_d3d11_get_data(ID3D11Device *device, ID3D11DeviceContext *context,
                           ID3D11Asynchronous *query, void *data, UINT size, UINT flags,
                           const char *operation)
{
    HRESULT hr = ID3D11DeviceContext_GetData(context, query, data, size, flags);
    if (FAILED(hr) || rpz_ab_off(2))  /* S_FALSE (not ready yet) is the common answer */
        gpu_d3d11_check_device(device, hr, operation);
    return hr;
}

static int gpu_call_failed(HRESULT result, const char *operation)
{
    if (FAILED(result)) {
        gpu_d3d11_check_device(s_dev, result, operation);
        fprintf(stderr, "[GPU-D3D11] %s failed 0x%08lX\n",
                operation, (unsigned long)result);
        return 1;
    }
    return 0;
}

/* Which adapter the device is created on.
 *
 * D3D11CreateDevice(NULL, ...) takes DXGI's first adapter, which on a hybrid
 * laptop is the integrated GPU driving the panel. A test laptop (Intel UHD +
 * GTX 1650) ran the whole renderer on the Intel part that way. Ask DXGI for
 * the high-performance adapter instead (IDXGIFactory6, Windows 10 1803+).
 *
 * RECOMP_GPU_ADAPTER overrides it: a number picks that index in
 * high-performance order (0 = fastest; the list is printed at startup), any
 * other text picks the first adapter whose name contains it,
 * case-insensitively ("nvidia", "intel", "amd"). "default" restores the old
 * NULL adapter. Returns NULL (old behaviour) when nothing better is found;
 * otherwise an adapter reference the caller releases. */
static IDXGIAdapter *pick_adapter(void)
{
    const char *want = getenv("RECOMP_GPU_ADAPTER");
    IDXGIFactory1 *f1 = NULL;
    IDXGIFactory6 *f6 = NULL;
    IDXGIAdapter1 *chosen = NULL;
    char want_lc[128] = "";
    int want_index = -1;
    UINT i;

    if (want && _stricmp(want, "default") == 0)
        return NULL;
    if (want && *want) {
        char *end = NULL;
        long v = strtol(want, &end, 10);
        if (end && *end == 0 && v >= 0)
            want_index = (int)v;
        else {
            size_t k;
            for (k = 0; want[k] && k + 1 < sizeof want_lc; k++)
                want_lc[k] = (char)tolower((unsigned char)want[k]);
            want_lc[k] = 0;
        }
    }
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1)))
        return NULL;
    if (FAILED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory6, (void **)&f6)))
        f6 = NULL;
    for (i = 0;; i++) {
        IDXGIAdapter1 *a = NULL;
        DXGI_ADAPTER_DESC1 d;
        char name[256], name_lc[256];
        HRESULT hr;
        size_t k;
        int take;

        hr = f6 ? IDXGIFactory6_EnumAdapterByGpuPreference(
                      f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                      &IID_IDXGIAdapter1, (void **)&a)
                : IDXGIFactory1_EnumAdapters1(f1, i, &a);
        if (FAILED(hr) || !a)
            break;
        if (FAILED(IDXGIAdapter1_GetDesc1(a, &d))) {
            IDXGIAdapter1_Release(a);
            continue;
        }
        if (!WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name,
                                 NULL, NULL))
            snprintf(name, sizeof name, "?");
        for (k = 0; name[k] && k + 1 < sizeof name_lc; k++)
            name_lc[k] = (char)tolower((unsigned char)name[k]);
        name_lc[k] = 0;
        fprintf(stderr, "[GPU-D3D11] adapter %u%s: \"%s\" vendor=0x%04X vram=%llu MB%s\n",
                i, f6 ? " (high-performance order)" : "", name, d.VendorId,
                (unsigned long long)(d.DedicatedVideoMemory >> 20),
                (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " software" : "");
        if (chosen || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            IDXGIAdapter1_Release(a);
            continue;
        }
        if (want_index >= 0)
            take = (int)i == want_index;
        else if (want_lc[0])
            take = strstr(name_lc, want_lc) != NULL;
        else
            take = f6 != NULL;   /* first high-performance hardware adapter */
        if (take)
            chosen = a;
        else
            IDXGIAdapter1_Release(a);
    }
    if (f6)
        IDXGIFactory6_Release(f6);
    IDXGIFactory1_Release(f1);
    if (!chosen && want && *want)
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_ADAPTER=%s matched no adapter; using the default\n",
                want);
    return (IDXGIAdapter *)chosen;
}

static void report_device_identity(D3D_FEATURE_LEVEL feature_level)
{
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    LARGE_INTEGER driver_version;
    char adapter_name[512];
    char driver[64];
    HRESULT hr;

    hr = ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] cannot query DXGI device identity 0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] cannot query adapter identity 0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    hr = IDXGIAdapter_GetDesc(adapter, &desc);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] cannot read adapter description 0x%08lX\n",
                (unsigned long)hr);
        IDXGIAdapter_Release(adapter);
        return;
    }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, desc.Description, -1,
                             adapter_name, sizeof adapter_name, NULL, NULL)) {
        fprintf(stderr, "[GPU-D3D11] cannot encode adapter description (Windows error %lu)\n",
                (unsigned long)GetLastError());
        snprintf(adapter_name, sizeof adapter_name, "<encoding unavailable>");
    }
    hr = IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_ID3D11Device, &driver_version);
    if (SUCCEEDED(hr)) {
        snprintf(driver, sizeof driver, "%u.%u.%u.%u",
                 HIWORD(driver_version.HighPart), LOWORD(driver_version.HighPart),
                 HIWORD(driver_version.LowPart), LOWORD(driver_version.LowPart));
    } else {
        snprintf(driver, sizeof driver, "unavailable:0x%08lX", (unsigned long)hr);
    }
    fprintf(stderr, "[GPU-D3D11] device adapter=\"%s\" vendor=0x%04X device=0x%04X"
                    " subsystem=0x%08X revision=%u dedicated_vram=%llu"
                    " luid=%08lX:%08lX driver=%s feature_level=0x%04X\n",
            adapter_name, desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
            (unsigned long long)desc.DedicatedVideoMemory,
            (unsigned long)desc.AdapterLuid.HighPart,
            (unsigned long)desc.AdapterLuid.LowPart,
            driver, (unsigned)feature_level);
    IDXGIAdapter_Release(adapter);
}

/* ---- statistics ---------------------------------------------------------- */

static struct {
    unsigned draws, xdraws, tris, uploads, rtt, clears, readbacks, ps_compiles, flips;
    unsigned overlap_rtt;       /* a texture inside a surface but not at its start */
    unsigned vdraws, vraw, vfall, kup, kskip;  /* draw_vsh: drawn, raw streams, CPU, constants */
    unsigned tex_gpu, tex_cpu, tex_bc, tex_direct, tex_pitch, tex_hires;
    unsigned tex_unsupported, tex_unsupported_hits;
    unsigned tex_stale_fallback, tex_soft, tex_stale_surface;
    unsigned depth_seeds, depth_samples, depth_snapshots, depth_rejected;
    unsigned stencil_samples, stencil_conversions;
    unsigned msaa_resolve_samples;
    unsigned texmode_dep;       /* draws with a bump-env / dot-product stage */
    unsigned guest_uploads, guest_conflicts;
    unsigned present_shared, present_busy;
    unsigned readback_coherent, work_batches, work_waits;
    uint64_t tex_approx_hash_bytes;
    struct { uint64_t calls, bytes, ticks; } guest_reads[GUEST_READ_KINDS];
    uint64_t texture_physical_hits, surface_physical_hits;
    uint64_t vertex_draw_ticks, vertex_prepare_ticks, texture_prepare_ticks;
    double readback_ms;
} s_st;

static unsigned s_q_reads;
static double s_q_wait_ms;
static unsigned s_rep_written, s_rep_flushes;

static double qpc_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void report_validation_perf(const char *name,
                                   const BruteForceValidationPerfCounters *counters)
{
    fprintf(stderr, "[PERF] %s: %llu calls, %llu accepted, %llu rejected,"
                    " %.3f ms total; VirtualQuery %llu calls, %llu failures,"
                    " %llu regions, %.3f ms\n",
            name,
            (unsigned long long)counters->calls,
            (unsigned long long)counters->accepted,
            (unsigned long long)counters->rejected,
            bruteforce_perf_ticks_ms(counters->total_ticks),
            (unsigned long long)counters->virtual_queries,
            (unsigned long long)counters->query_failures,
            (unsigned long long)counters->regions,
            bruteforce_perf_ticks_ms(counters->query_ticks));
}

static void report_detailed_perf(void)
{
    static uint64_t previous_us;
    BruteForcePerfCounters counters;
    const BruteForceTexturePerfCounters *texture;
    uint64_t report_us;
    unsigned i;

    bruteforce_perf_counters_take(&counters);
    report_us = xbox_HostNowNs() / 1000u;
    report_validation_perf("guest_word", &counters.guest_word);
    report_validation_perf("readable_guest_range", &counters.guest_range);
    static const char *const read_names[GUEST_READ_KINDS] = {
        "color surface", "depth surface", "texels", "palette"
    };
    for (unsigned kind = 0; kind < GUEST_READ_KINDS; ++kind) {
        if (!s_st.guest_reads[kind].calls)
            continue;
        fprintf(stderr, "[PERF] ReadProcessMemory (%s): %llu calls, %llu bytes, %.3f ms\n",
                read_names[kind], (unsigned long long)s_st.guest_reads[kind].calls,
                (unsigned long long)s_st.guest_reads[kind].bytes,
                bruteforce_perf_ticks_ms(s_st.guest_reads[kind].ticks));
    }
    if (s_st.texture_physical_hits)
        fprintf(stderr, "[PERF] physical texture cache: %llu exact hits\n",
                (unsigned long long)s_st.texture_physical_hits);
    if (s_st.surface_physical_hits)
        fprintf(stderr, "[PERF] physical surface shadow: %llu exact hits\n",
                (unsigned long long)s_st.surface_physical_hits);
    if (s_st.vertex_draw_ticks)
        fprintf(stderr, "[PERF] vertex draw: %.3f ms total, %.3f ms preparing pixel state,"
                        " %.3f ms preparing textures and samplers\n",
                bruteforce_perf_ticks_ms(s_st.vertex_draw_ticks),
                bruteforce_perf_ticks_ms(s_st.vertex_prepare_ticks),
                bruteforce_perf_ticks_ms(s_st.texture_prepare_ticks));
    texture = &counters.texture;
    fprintf(stderr, "[PERF] texture cache: %llu lookups, %llu probes,"
                    " %llu key hits, %llu misses, %.3f ms scanning;"
                    " exact %llu checks, %llu hits, %llu source bytes,"
                    " %llu palette bytes, %.3f ms;"
                    " snapshots %llu copies, %llu bytes, %.3f ms;"
                    " rewrites %llu, entry evictions %llu, snapshot evictions %llu,"
                    " failed-upload discards %llu, unsupported hits %llu\n",
            (unsigned long long)texture->lookups,
            (unsigned long long)texture->probes,
            (unsigned long long)texture->key_hits,
            (unsigned long long)texture->key_misses,
            bruteforce_perf_ticks_ms(texture->lookup_ticks),
            (unsigned long long)texture->exact_checks,
            (unsigned long long)texture->exact_hits,
            (unsigned long long)texture->source_compare_bytes,
            (unsigned long long)texture->palette_compare_bytes,
            bruteforce_perf_ticks_ms(texture->exact_ticks),
            (unsigned long long)texture->snapshot_copies,
            (unsigned long long)texture->snapshot_copy_bytes,
            bruteforce_perf_ticks_ms(texture->snapshot_copy_ticks),
            (unsigned long long)texture->rewrites,
            (unsigned long long)texture->evictions,
            (unsigned long long)texture->snapshot_evictions,
            (unsigned long long)texture->discards,
            (unsigned long long)texture->unsupported_hits);
    fprintf(stderr, "[PERF-SHADER] kind=pixel-compile calls=%llu failures=%llu"
                    " elapsed_ms=%.3f window_start_us=%llu window_end_us=%llu\n",
            (unsigned long long)counters.pixel_shader_compile.calls,
            (unsigned long long)counters.pixel_shader_compile.failures,
            bruteforce_perf_ticks_ms(counters.pixel_shader_compile.ticks),
            (unsigned long long)previous_us, (unsigned long long)report_us);
    for (i = 0; i < BRUTEFORCE_PERF_MAX_CALLERS; ++i) {
        const BruteForceSubmitPerfCounter *pfb = &counters.pfb[i];
        const BruteForceSubmitPerfCounter *put = &counters.put[i];
        if (pfb->calls)
            fprintf(stderr, "[PERF-SUBMIT] kind=pfb site=%08X caller=%08X"
                            " calls=%llu elapsed_ms=%.3f window_start_us=%llu window_end_us=%llu\n",
                    pfb->site, pfb->caller, (unsigned long long)pfb->calls,
                    bruteforce_perf_ticks_ms(pfb->ticks),
                    (unsigned long long)previous_us, (unsigned long long)report_us);
        if (put->calls)
            fprintf(stderr, "[PERF-SUBMIT] kind=put site=%08X caller=%08X"
                            " calls=%llu elapsed_ms=%.3f window_start_us=%llu window_end_us=%llu\n",
                    put->site, put->caller, (unsigned long long)put->calls,
                    bruteforce_perf_ticks_ms(put->ticks),
                    (unsigned long long)previous_us, (unsigned long long)report_us);
    }
    if (counters.pfb_overflow || counters.put_overflow)
        fprintf(stderr, "[PERF-SUBMIT] caller table overflow: pfb=%llu put=%llu\n",
                (unsigned long long)counters.pfb_overflow,
                (unsigned long long)counters.put_overflow);
    previous_us = report_us;
}

/* Frames the game has completed so far (debug FPS counter, overlay.c). */
uint32_t halo2_gpu_flip_count(void) { return *(volatile uint32_t *)&s_flip_life; }

static void stats_flip(void)
{
    static double last;
    double now = qpc_ms();
    s_st.flips++;
    s_flip_life++;
    if (!last)
        last = now;
    if (now - last >= 10000.0) {
        fprintf(stderr, "[GPU-D3D11] declined batches: no color surface %u, begin %u,"
                        " guest sync/clip %u, texture %u (%u not sent to the CPU), combiner %u\n",
                s_xrej[0], s_xrej[1], s_xrej[2], s_xrej[3], s_texrej_skips, s_xrej[4]);
        fprintf(stderr, "[GPU-D3D11] alias rejects: allocation %u, uncertain %u, sync %u,"
                        " dirty non-exact %u, dirty beside exact %u, bounds %u, view %u\n",
                s_arej[0], s_arej[1], s_arej[2], s_arej[3], s_arej[4], s_arej[5], s_arej[6]);
        fprintf(stderr, "[GPU-D3D11] texture fallbacks: stale cache %u, soft stage %u, stale surface %u;"
                        " dot/bump-env stage draws %u; raced overlaps won by order %u\n",
                s_st.tex_stale_fallback, s_st.tex_soft, s_st.tex_stale_surface, s_st.texmode_dep,
                s_sync_order_wins);
        {
            extern unsigned s_relayout_rb_n(void), s_relayout_rb_fail_n(void);
            extern unsigned arb_stat(int);
            fprintf(stderr, "[GPU-D3D11] relayout readbacks %u, failed %u, write-order wins %u;"
                            " delayed readbacks %u done %u dropped %u; msaa-resolve samples %u\n",
                    s_relayout_rb_n(), s_relayout_rb_fail_n(), s_order_wins,
                    arb_stat(0), arb_stat(1), arb_stat(2), s_st.msaa_resolve_samples);
        }
        fprintf(stderr, "[GPU-D3D11] %u flips: %u draws (%u extended, %u vertex programs"
                        " [%u raw], %u back to the CPU), %u triangles, %u uploads,"
                        " %u render-to-texture, %u overlapping, %u clears, %u pixel shaders"
                        " compiled, constants %u uploaded %u skipped, readback %.2f ms/flip,"
                        " %u visibility reads waiting %.1f ms, %u reports written"
                        " asynchronously (%u polls found one pending),"
                        " textures GPU %u CPU %u BC %u direct %u, %u stale pitches,"
                        " %u hi-res replacements, unsupported %u (%u cached),"
                        " depth seeds %u samples %u snapshots %u rejected %u,"
                        " stencil aliases %u conversions %u,"
                        " guest uploads %u conflicts %u,"
                        " shared presents %u busy %u,"
                        " readback copies %u coherent %u, GPU batches %u waits %u,"
                        " %llu approximate texture-hash bytes\n",
                s_st.flips, s_st.draws, s_st.xdraws, s_st.vdraws, s_st.vraw, s_st.vfall,
                s_st.tris, s_st.uploads, s_st.rtt, s_st.overlap_rtt, s_st.clears,
                s_st.ps_compiles, s_st.kup, s_st.kskip,
                s_st.flips ? s_st.readback_ms / s_st.flips : 0.0,
                s_q_reads, s_q_wait_ms, s_rep_written, s_rep_flushes,
                s_st.tex_gpu, s_st.tex_cpu, s_st.tex_bc, s_st.tex_direct, s_st.tex_pitch,
                s_st.tex_hires, s_st.tex_unsupported, s_st.tex_unsupported_hits,
                s_st.depth_seeds, s_st.depth_samples, s_st.depth_snapshots, s_st.depth_rejected,
                s_st.stencil_samples, s_st.stencil_conversions,
                s_st.guest_uploads, s_st.guest_conflicts,
                s_st.present_shared, s_st.present_busy,
                s_st.readbacks, s_st.readback_coherent, s_st.work_batches, s_st.work_waits,
                (unsigned long long)s_st.tex_approx_hash_bytes);
        if (g_bruteforce_perf_counters_enabled)
            report_detailed_perf();
        s_q_reads = 0;
        s_q_wait_ms = 0.0;
        s_rep_written = s_rep_flushes = 0;
        memset(&s_st, 0, sizeof s_st);
        last = now;
    }
}

/* ---- shaders ------------------------------------------------------------- */

#define SHADER_COMPILE_FLAGS D3DCOMPILE_OPTIMIZATION_LEVEL3

static int compile(const char *src, size_t len, const char *name, const char *entry,
                   const char *target, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(src, len, name, NULL, NULL, entry, target,
                            SHADER_COMPILE_FLAGS, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] shader %s/%s failed: %s\n", name, entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err)
            ID3D10Blob_Release(err);
        return 0;
    }
    if (err)
        ID3D10Blob_Release(err);
    return 1;
}

/* Screen-space batches (base interface): stage 0 times the diffuse colour,
 * as the CPU path's raster_indexed. */
static const char s_base_hlsl[] =
GPU_WINDOW_CLIP_HLSL
GPU_SHADOW_HLSL
"cbuffer B : register(b0) { float4 surf; float4 texinfo; float4 alpha; float4 depth; };\n"
"struct VSIn { float4 p : POSITION; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"struct PSIn { float4 p : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"PSIn vs_main(VSIn i) {\n"
"  PSIn o;\n"
"  float w = (i.p.w > 0.0) ? 1.0 / i.p.w : 1.0;\n"
"  float x = i.p.x * surf.z * 2.0 - 1.0;\n"
"  float y = 1.0 - i.p.y * surf.w * 2.0;\n"
"  float z = (i.p.z - depth.x) * depth.y;\n"
"  o.p = float4(x * w, y * w, z * w, w);\n"
"  o.c = i.c;\n"
"  o.uv = i.uv * texinfo.xy;\n"
"  return o;\n"
"}\n"
"Texture2D t0 : register(t0);\n"
"SamplerState s0 : register(s0);\n"
"float4 ps_main(PSIn i) : SV_Target {\n"
"  if (!window_allows(i.p.xy)) discard;\n"
"  float4 c = i.c;\n"
"  if (texinfo.z > 0.5) {\n"
"    float4 t = t0.Sample(s0, i.uv);\n"
"    if (texinfo.w > 0.5) t = shadow_compare(t.r * 16777215.0, 0.0, (uint)texinfo.w - 1).xxxx;\n"
"    c *= t;\n"
"  }\n"
"  if (alpha.z > 0.5) { int v = (int)(c.a * 255.0 + 0.5), r = (int)alpha.y; int f = (int)alpha.x;\n"
"    bool ok = (f == 7) || (f == 1 && v < r) || (f == 2 && v == r) || (f == 3 && v <= r)\n"
"      || (f == 4 && v > r) || (f == 5 && v != r) || (f == 6 && v >= r);\n"
"    if (!ok) discard; }\n"
"  return c;\n"
"}\n";

typedef struct {
    float surf[4], tex[4], alpha[4], depth[4];
} BaseConsts;

/* Transformed batches (extended interface): the executor's vertices, in
 * surface pixels with 1/w, mapped to an affine image of clip space so the
 * texture coordinates interpolate perspective-correctly. */
static const char s_xvs_hlsl[] =
GPU_RC_CBUFFER
"struct VSIn { float4 p : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 fog : TEXCOORD4; float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
"  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n"
"struct VSOut { float4 p : SV_Position; noperspective float4 d0 : COLOR0;\n"
"  noperspective float4 d1 : COLOR1; noperspective float fog : FOG;\n"
"  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
"  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; float fogp : FOG1; };\n"
"VSOut vs_main(VSIn i) {\n"
"  VSOut o;\n"
"  float w = (i.p.w > 0.0) ? 1.0 / i.p.w : 1.0;\n"
"  float x = i.p.x * surf.z * 2.0 - 1.0;\n"
"  float y = 1.0 - i.p.y * surf.w * 2.0;\n"
"  float zr = (i.p.z - depth.x) * depth.y;\n"
"  float z = depth.z > 0.5 ? saturate(zr) : zr;\n"
"  o.p = float4(x * w, y * w, z * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog.x; o.fogp = i.fog.x;\n"
"  o.tc0 = i.tc0; o.tc1 = i.tc1; o.tc2 = i.tc2; o.tc3 = i.tc3;\n"
"  return o;\n"
"}\n";

static ID3D11VertexShader *s_base_vs, *s_xvs;
static ID3D11PixelShader *s_base_ps;
static ID3D11InputLayout *s_base_layout, *s_xlayout;
static ID3D11Buffer *s_base_cb, *s_xcb, *s_window_cb;
static ID3D11RasterizerState *s_rs;
static ID3D11RasterizerState *s_rs_cull[2][2];  /* [cull back][front counter-clockwise] */
/* Depth-biased twins for blended LEQUAL draws that write no depth (Halo 2's
 * light and decal passes over already-drawn surfaces). Without the bias
 * those passes depth-fight the surface and cut off on hard, flickering
 * edges (fire glow, corridor lights; tested 2026-11-18). On by default;
 * RECOMP_GPU_DECAL_BIAS=0 disables, =<units>,<slope> tunes (default -64,-1);
 * RECOMP_GPU_DECAL_BIAS_FILE=<path> applies it only while that file exists. */
static ID3D11RasterizerState *s_rs_bias, *s_rs_cull_bias[2][2];

#define MAX_PS 1024
typedef struct { GpuRcKey key; ID3D11PixelShader *ps; } PsEntry;
static PsEntry *s_ps;
static int s_nps;

static ID3D11PixelShader *get_rc_shader(const GpuRcKey *k)
{
    static char src[64 * 1024];
    static const char name[] = "rc", entry[] = "ps_main", target[] = "ps_5_0";
    size_t len, cached_size = 0;
    void *cached = NULL;
    ID3DBlob *blob = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    double started;
    uint64_t perf_started;
    int perf = g_bruteforce_perf_counters_enabled;
    int i;

    for (i = 0; i < s_nps; i++)
        if (!memcmp(&s_ps[i].key, k, sizeof *k))
            return s_ps[i].ps;
    if (s_nps == MAX_PS)
        return NULL;
    len = gpu_rc_hlsl(k, src, sizeof src);
    if (!len)
        return NULL;
    {   /* debug: RECOMP_DEBUG_RC_DUMP=<dir> saves each new combiner shader's HLSL */
        const char *dd = getenv("RECOMP_DEBUG_RC_DUMP");
        if (dd && *dd) {
            char p[512];
            FILE *f;
            snprintf(p, sizeof p, "%s\\rc_%03d_%08X_%08X_%08X.hlsl", dd, s_nps,
                     k->control, k->final0, k->final1);
            f = fopen(p, "wb");
            if (f) { fwrite(src, 1, len, f); fclose(f); }
        }
    }
    started = qpc_ms();
    if (gpu_pscache_load(src, len, name, entry, target, SHADER_COMPILE_FLAGS,
                         &cached, &cached_size)) {
        hr = ID3D11Device_CreatePixelShader(s_dev, cached, cached_size, NULL, &ps);
        free(cached);
        if (FAILED(hr) || !ps) {
            gpu_d3d11_check_device(s_dev, hr, "creating cached pixel shader");
            fprintf(stderr, "[GPU-PSCACHE] cached shader rejected 0x%08lX; recompiling\n",
                    (unsigned long)hr);
            if (ps)
                ID3D11PixelShader_Release(ps);
            ps = NULL;
        } else {
            fprintf(stderr, "[GPU-PSCACHE] hit in %.3f ms\n", qpc_ms() - started);
        }
    }
    if (!ps) {
        started = qpc_ms();
        perf_started = perf ? bruteforce_perf_clock() : 0;
        if (g_rpz_on > 0) rpz_count(RPZ_SHADER_COMPILE, 1);
        if (!compile(src, len, name, entry, target, &blob)) {
            if (perf)
                bruteforce_perf_record_pixel_shader_compile(
                    0, bruteforce_perf_clock() - perf_started);
            return NULL;
        }
        hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                            ID3D10Blob_GetBufferSize(blob), NULL, &ps);
        if (FAILED(hr) || !ps) {
            if (perf)
                bruteforce_perf_record_pixel_shader_compile(
                    0, bruteforce_perf_clock() - perf_started);
            gpu_d3d11_check_device(s_dev, hr, "creating pixel shader");
            fprintf(stderr, "[GPU-D3D11] CreatePixelShader(rc) failed 0x%08lX\n",
                    (unsigned long)hr);
            ID3D10Blob_Release(blob);
            return NULL;
        }
        if (perf)
            bruteforce_perf_record_pixel_shader_compile(
                1, bruteforce_perf_clock() - perf_started);
        s_st.ps_compiles++;
        if (gpu_pscache_enabled()) {
            fprintf(stderr, "[GPU-PSCACHE] compiled in %.3f ms\n", qpc_ms() - started);
            gpu_pscache_store(src, len, name, entry, target, SHADER_COMPILE_FLAGS,
                              ID3D10Blob_GetBufferPointer(blob),
                              ID3D10Blob_GetBufferSize(blob));
        }
        ID3D10Blob_Release(blob);
    }
    s_ps[s_nps].key = *k;
    s_ps[s_nps].ps = ps;
    s_nps++;
    return ps;
}

/* ---- device -------------------------------------------------------------- */

static void msaa_query_device(void);
static int ensure_device_for_scale(int scale_requested)
{
    static const D3D11_INPUT_ELEMENT_DESC base_il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    /* Nv2aXVertex: x y z rhw | d0 | d1 | fog pad[3] | tc[4]. */
    static const D3D11_INPUT_ELEMENT_DESC x_il[8] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    ID3DBlob *vsb = NULL, *psb = NULL, *xvsb = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    /* GetDeviceRemovedReason is a kernel call; per draw it was ~11% of the
     * GPU thread. Failed calls still check at once (gpu_call_failed). */
    if (s_dev) {
        static ULONGLONG s_dev_check_ms;
        ULONGLONG now = GetTickCount64();
        if (now - s_dev_check_ms >= 50 || rpz_ab_off(2)) {
            s_dev_check_ms = now;
            gpu_d3d11_check_device(s_dev, S_OK, "starting GPU work");
        }
    }
    if (s_failed)
        return 0;
    if (s_dev)
        return !scale_requested || gpu_scale_gpu_init(s_dev, s_ctx);
    s_failed = 1;               /* until everything below has worked */
    {
        IDXGIAdapter *adapter = s_warp ? NULL : pick_adapter();
        hr = D3D11CreateDevice(adapter,
                               s_warp ? D3D_DRIVER_TYPE_WARP
                                      : adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                : D3D_DRIVER_TYPE_HARDWARE,
                               NULL, D3D11_CREATE_DEVICE_SINGLETHREADED, &fl, 1,
                               D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
        if (FAILED(hr) && adapter) {
            fprintf(stderr, "[GPU-D3D11] device on the chosen adapter failed 0x%08lX; "
                    "retrying on the default adapter\n", (unsigned long)hr);
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   D3D11_CREATE_DEVICE_SINGLETHREADED, &fl, 1,
                                   D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
        }
        if (adapter)
            IDXGIAdapter_Release(adapter);
    }
    if (FAILED(hr)) {
        gpu_d3d11_check_device(NULL, hr, "creating D3D11 device");
        fprintf(stderr, "[GPU-D3D11] D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr);
        goto failed;
    }
    InterlockedExchange(&s_device_thread, (LONG)GetCurrentThreadId());
    report_device_identity(got);
    msaa_query_device();
    if (!compile(s_base_hlsl, sizeof s_base_hlsl - 1, "base", "vs_main", "vs_5_0", &vsb)
        || !compile(s_base_hlsl, sizeof s_base_hlsl - 1, "base", "ps_main", "ps_5_0", &psb)
        || !compile(s_xvs_hlsl, sizeof s_xvs_hlsl - 1, "xvs", "vs_main", "vs_5_0", &xvsb)) {
        goto failed;
    }
    gpu_call_failed(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_base_vs),
                    "creating base vertex shader");
    gpu_call_failed(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_base_ps),
                    "creating base pixel shader");
    gpu_call_failed(ID3D11Device_CreateInputLayout(s_dev, base_il, 3, ID3D10Blob_GetBufferPointer(vsb),
                                                   ID3D10Blob_GetBufferSize(vsb), &s_base_layout),
                    "creating base input layout");
    gpu_call_failed(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(xvsb),
                                                    ID3D10Blob_GetBufferSize(xvsb), NULL, &s_xvs),
                    "creating extended vertex shader");
    gpu_call_failed(ID3D11Device_CreateInputLayout(s_dev, x_il, 8, ID3D10Blob_GetBufferPointer(xvsb),
                                                   ID3D10Blob_GetBufferSize(xvsb), &s_xlayout),
                    "creating extended input layout");
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    ID3D10Blob_Release(xvsb);
    vsb = psb = xvsb = NULL;

    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(BaseConsts);
    gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_base_cb),
                    "creating base constants");
    bd.ByteWidth = (sizeof(GpuRcConsts) + 15) & ~15u;
    gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_xcb),
                    "creating extended constants");
    bd.ByteWidth = 9 * 16;
    gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_window_cb),
                    "creating window clip constants");

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;      /* the executor culls, as the CPU path */
    rd.DepthClipEnable = FALSE;         /* the executor clips at the near plane; far
                                         * geometry is clamped, which Halo 2's distant
                                         * scenery relies on (tested 2026-11-18) */
    rd.ScissorEnable = TRUE;
    rd.MultisampleEnable = TRUE;        /* used when a target has MSAA samples */
    gpu_call_failed(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs),
                    "creating rasterizer state");
    {
        int back, ccw;
        for (back = 0; back < 2; back++)
            for (ccw = 0; ccw < 2; ccw++) {
                rd.CullMode = back ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
                rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
                gpu_call_failed(ID3D11Device_CreateRasterizerState(s_dev, &rd,
                                                                   &s_rs_cull[back][ccw]),
                                "creating cull state");
            }
        rd.CullMode = D3D11_CULL_NONE;
        rd.FrontCounterClockwise = FALSE;
    }
    if (!(getenv("RECOMP_GPU_DECAL_BIAS") && getenv("RECOMP_GPU_DECAL_BIAS")[0] == '0')) {
        int back, ccw, units = -64;
        float slope = -1.0f;
        const char *e = getenv("RECOMP_GPU_DECAL_BIAS");
        if (e)
            sscanf(e, "%d,%f", &units, &slope);
        rd.DepthBias = units;
        rd.SlopeScaledDepthBias = slope;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_bias);
        for (back = 0; back < 2; back++)
            for (ccw = 0; ccw < 2; ccw++) {
                rd.CullMode = back ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
                rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
                ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_cull_bias[back][ccw]);
            }
        rd.CullMode = D3D11_CULL_NONE;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthBias = 0;
        rd.SlopeScaledDepthBias = 0.0f;
        fprintf(stderr, "[DECALBIAS] armed: units %d slope %g\n", units, slope);
    }

    s_ps = (PsEntry *)calloc(MAX_PS, sizeof *s_ps);
    if (!s_base_vs || !s_base_ps || !s_base_layout || !s_xvs || !s_xlayout || !s_base_cb
        || !s_xcb || !s_window_cb || !s_rs || !s_rs_cull[0][0] || !s_rs_cull[0][1]
        || !s_rs_cull[1][0] || !s_rs_cull[1][1] || !s_ps)
        goto failed;
    if (scale_requested &&
        (scale_test_failure("resolve-init") || !gpu_scale_gpu_init(s_dev, s_ctx))) {
        gpu_d3d11_check_device(s_dev, S_OK, "initializing scaled resolve");
        fprintf(stderr, "[GPU-SCALE] resolve initialization failed; configuration not committed\n");
        goto failed;
    }
    s_failed = 0;
    fprintf(stderr, "[GPU-D3D11] %s device ready (feature level 0x%x)\n",
            s_warp ? "WARP" : "hardware", (unsigned)got);
    return 1;

failed:
    if (vsb) ID3D10Blob_Release(vsb);
    if (psb) ID3D10Blob_Release(psb);
    if (xvsb) ID3D10Blob_Release(xvsb);
    free(s_ps); s_ps = NULL;
    {
        int back, ccw;
        for (back = 0; back < 2; ++back)
            for (ccw = 0; ccw < 2; ++ccw) {
                if (s_rs_cull[back][ccw]) ID3D11RasterizerState_Release(s_rs_cull[back][ccw]);
                s_rs_cull[back][ccw] = NULL;
            }
    }
    if (s_rs) ID3D11RasterizerState_Release(s_rs);
    if (s_window_cb) ID3D11Buffer_Release(s_window_cb);
    if (s_xcb) ID3D11Buffer_Release(s_xcb);
    if (s_base_cb) ID3D11Buffer_Release(s_base_cb);
    if (s_xlayout) ID3D11InputLayout_Release(s_xlayout);
    if (s_base_layout) ID3D11InputLayout_Release(s_base_layout);
    if (s_xvs) ID3D11VertexShader_Release(s_xvs);
    if (s_base_ps) ID3D11PixelShader_Release(s_base_ps);
    if (s_base_vs) ID3D11VertexShader_Release(s_base_vs);
    s_rs = NULL;
    s_window_cb = s_xcb = s_base_cb = NULL;
    s_xlayout = s_base_layout = NULL;
    s_xvs = s_base_vs = NULL;
    s_base_ps = NULL;
    if (s_ctx) ID3D11DeviceContext_Release(s_ctx);
    if (s_dev) ID3D11Device_Release(s_dev);
    s_ctx = NULL;
    s_fragment.valid = 0;
    s_dev = NULL;
    s_failed = 0;
    InterlockedExchange(&s_device_thread, 0);
    return 0;
}

static int ensure_device(void)
{
    if (s_scale_pending) {
        if (!ensure_device_for_scale(s_pending_scale.requested)) {
            fprintf(stderr, "[GPU-SCALE] D3D11 initialization failed;"
                            " retaining prior configuration\n");
            return 0;
        }
        if (!gpu_scale_commit_config(&s_pending_scale)) {
            fprintf(stderr, "[GPU-SCALE] cannot commit D3D11 configuration\n");
            return 0;
        }
        s_scale_pending = 0;
        return 1;
    }
    return ensure_device_for_scale(gpu_scale_requested());
}

#ifdef BRUTEFORCE_D3D11_TESTING
int bruteforce_d3d11_test_initialize_device(void) { return ensure_device(); }
uint32_t bruteforce_d3d11_test_device_thread(void)
{
    return (uint32_t)InterlockedCompareExchange(&s_device_thread, 0, 0);
}
#endif

/* ---- surfaces ------------------------------------------------------------ */

#define MAX_SURF 32
typedef struct {
    uint64_t layout, allocation, bounds;
} GuestSpanIdentity;

typedef struct {
    uint32_t va, pitch, bpp, w, h, pw, ph, aa_sx, aa_sy;
    unsigned scale_epoch;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *guest_tex;
    ID3D11ShaderResourceView *guest_srv;
    ID3D11Texture2D *sample_copy;
    ID3D11ShaderResourceView *sample_copy_srv;
    uint8_t *guest_shadow;
    size_t guest_shadow_bytes;
    GuestSpanIdentity identity;
    unsigned guest_check_gen;
    int gpu_dirty;
    int guest_exact;
    int alias_uncertain;
    uint64_t wserial;               /* s_wserial at the last color write */
    unsigned write_gen;             /* s_gen at the last color write */
    ID3D11Texture2D *msaa_tex;
    ID3D11RenderTargetView *msaa_rtv;
    UINT msaa_count;
    int msaa_dirty;
} Surf;

typedef struct {
    uint32_t va, w, h, pitch, format, surface_type, initialized, recreated;
    uint32_t logical_w[2], logical_h[2];
    unsigned scale_epoch;
    uint64_t revision, packed_revision;
    int packed_valid, packed_layout, gpu_dirty, guest_conflict;
    uint8_t *guest_shadow;
    size_t guest_shadow_bytes;
    GuestSpanIdentity identity;
    ID3D11Texture2D *tex, *snapshot;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv, *snapshot_srv;
    ID3D11ShaderResourceView *stencil_srv, *snapshot_stencil_srv, *packed_srv;
    ID3D11Texture2D *packed;
    ID3D11UnorderedAccessView *packed_uav;
    ID3D11Texture2D *msaa_tex;
    ID3D11DepthStencilView *msaa_dsv;
    UINT msaa_count;
    int msaa_dirty;
    uint64_t zserial;               /* s_wserial at the last draw with this depth bound */
    uint32_t bound_gen;             /* s_gen of that draw */
} Depth;

static Surf s_surf[MAX_SURF];
static int s_nsurf;
static Depth s_depth[MAX_SURF];
static int s_ndepth;
static Surf *s_cur;
/* Widescreen: how much the 2D layer is squeezed across (4/3 over the picture
 * aspect; 1 = 4:3, nothing squeezed). halo2_gpu_set_ui_squeeze. */
static float s_ui_squeeze = 1.0f;
void halo2_gpu_set_ui_squeeze(float squeeze) { if (squeeze > 0.3f && squeeze <= 1.0f) s_ui_squeeze = squeeze; }
static Surf *s_last_presented; /* Last readback surface for flip-only resize preflight. */
static Depth *s_cur_depth;
static UINT s_msaa_want = 1;
static UINT s_msaa_ok_color[9];
static UINT s_msaa_ok_depth[9];
static int s_msaa_queried;
static uint32_t s_gen = 1;          /* frame number */

/* Probe and draw-list hooks used by the draw paths. All of them are inert
 * unless their RECOMP_DEBUG_* switch is set; they are declared outside the
 * RECOMP_DEBUG_TOOLS block so a build without the debug tools compiles. */
static void probe_check(uint32_t seq, const Nv2aXBatch *xb);
static int zeq_enabled(void);
static uint32_t s_probe_armed;   /* debug probe window start (0 = off) */
static uint32_t s_dl_seq;        /* draw sequence number (debug draw list) */
static void zeq_measure(const Nv2aRenderState *rs, UINT n, UINT start);
static ID3D11Query *query_create(D3D11_QUERY type);
static void probe_srv(uint32_t seq);

#ifdef RECOMP_DEBUG_TOOLS
#define DEBUG_NOTE_MAX 32
typedef struct {
    uint32_t frame, va, w, h, fmt, extra;
    uint64_t vsh;
    uint32_t indices;
    uint8_t soft;
    uint32_t stage_va[4], stage_w[4], stage_h[4], stage_fmt[4], stage_mode[4];
    uint32_t block_va, block_w, block_h, block_pitch, block_bpp, block_age;
    uint64_t block_wserial;
    char kind[24];
    char detail[120];
} DebugNote;
static DebugNote s_dbg_notes[DEBUG_NOTE_MAX];
static unsigned s_dbg_note_n, s_dbg_note_i;
static uint64_t s_dbg_vsh;
static uint32_t s_dbg_indices;
static uint32_t s_dbg_stage_va[4], s_dbg_stage_w[4], s_dbg_stage_h[4], s_dbg_stage_fmt[4], s_dbg_stage_mode[4];
static uint32_t s_dbg_block_va, s_dbg_block_w, s_dbg_block_h, s_dbg_block_pitch, s_dbg_block_bpp, s_dbg_block_age;
static uint64_t s_dbg_block_wserial;

static void debug_note_set_draw(uint64_t vsh, uint32_t indices, const Nv2aXBatch *xb)
{
    int st;
    uint32_t prog;
    if (!halo2_screenshot_enabled())
        return;
    s_dbg_vsh = vsh;
    s_dbg_indices = indices;
    prog = xb && xb->rc ? xb->rc->stage_program : 0;
    for (st = 0; st < 4; st++) {
        if (xb && xb->tex_valid[st]) {
            s_dbg_stage_va[st] = xb->tex[st].offset;
            s_dbg_stage_w[st] = xb->tex[st].width;
            s_dbg_stage_h[st] = xb->tex[st].height;
            s_dbg_stage_fmt[st] = xb->tex[st].color;
        } else {
            s_dbg_stage_va[st] = s_dbg_stage_w[st] = s_dbg_stage_h[st] = s_dbg_stage_fmt[st] = 0;
        }
        s_dbg_stage_mode[st] = (prog >> (st * 5)) & 0x1F;
    }
}

static void debug_note_set_block(const Surf *s)
{
    if (!halo2_screenshot_enabled())
        return;
    if (!s) {
        s_dbg_block_va = s_dbg_block_w = s_dbg_block_h = 0;
        s_dbg_block_pitch = s_dbg_block_bpp = s_dbg_block_age = 0;
        s_dbg_block_wserial = 0;
        return;
    }
    s_dbg_block_va = s->va;
    s_dbg_block_w = s->w;
    s_dbg_block_h = s->h;
    s_dbg_block_pitch = s->pitch;
    s_dbg_block_bpp = s->bpp;
    s_dbg_block_wserial = s->wserial;
    s_dbg_block_age = s->write_gen && s_gen >= s->write_gen ? s_gen - s->write_gen : 0;
}

static void dl_outcome(const char *outcome, const char *reason);
static void dl_reason(const char *prefix, const char *detail);
static void debug_note(const char *kind, const char *detail, uint32_t va,
                       uint32_t w, uint32_t h, uint32_t fmt, uint32_t extra)
{
    DebugNote *n;
    int st;
    /* Event ring is only for the sidecar. Leave the hot path alone unless
     * the debug screenshot env is on, and never snprintf here. */
    if (!halo2_screenshot_enabled())
        return;
    if (kind && strcmp(kind, "declined") == 0)
        dl_outcome("declined", detail);
    else if (kind && strcmp(kind, "alias") == 0)
        dl_reason("alias:", detail);
    else if (kind && strcmp(kind, "unsupported_tex") == 0)
        dl_reason("unsupported_tex:", detail);
    n = &s_dbg_notes[s_dbg_note_i % DEBUG_NOTE_MAX];
    s_dbg_note_i++;
    if (s_dbg_note_n < DEBUG_NOTE_MAX)
        s_dbg_note_n++;
    n->frame = s_gen;
    n->va = va;
    n->w = w;
    n->h = h;
    n->fmt = fmt;
    n->extra = extra;
    n->vsh = s_dbg_vsh;
    n->indices = s_dbg_indices;
    n->soft = 0;
    if (kind && (kind[0] == 's' && kind[1] == 'o' && kind[2] == 'f' && kind[3] == 't'))
        n->soft = 1;
    else if (detail && detail[0] == 's' && detail[1] == 'o' && detail[2] == 'f' && detail[3] == 't')
        n->soft = 1;
    for (st = 0; st < 4; st++) {
        n->stage_va[st] = s_dbg_stage_va[st];
        n->stage_w[st] = s_dbg_stage_w[st];
        n->stage_h[st] = s_dbg_stage_h[st];
        n->stage_fmt[st] = s_dbg_stage_fmt[st];
        n->stage_mode[st] = s_dbg_stage_mode[st];
    }
    n->block_va = s_dbg_block_va;
    n->block_w = s_dbg_block_w;
    n->block_h = s_dbg_block_h;
    n->block_pitch = s_dbg_block_pitch;
    n->block_bpp = s_dbg_block_bpp;
    n->block_age = s_dbg_block_age;
    n->block_wserial = s_dbg_block_wserial;
    n->kind[0] = 0;
    n->detail[0] = 0;
    if (kind) {
        size_t i;
        for (i = 0; kind[i] && i + 1 < sizeof n->kind; i++)
            n->kind[i] = kind[i];
        n->kind[i] = 0;
    }
    if (detail) {
        size_t i;
        for (i = 0; detail[i] && i + 1 < sizeof n->detail; i++)
            n->detail[i] = detail[i];
        n->detail[i] = 0;
    }
}

/* ---- debug-only per-frame draw list (DEL writes it beside the PNG) -------
 *
 * While RECOMP_DEBUG_SCREENSHOT=1 every draw the back end sees is recorded
 * into one of two frame buffers (the frame being built and the one before
 * it). DEL writes both to <shot>.draws.csv: render states, shader ids,
 * texture formats/addresses/results per stage, render target and the
 * outcome (drawn, culled, declined, texture-skip, CPU fallback) with the
 * reason. Capped at DL_CAP rows per frame; overflow is counted. */
#define DL_CAP 12000
typedef struct {
    uint32_t gen, seq;
    char path;                      /* V: vertex program on GPU, X: CPU-transformed, F: vsh -> CPU */
    uint64_t vsh;
    uint32_t count;
    uint32_t surf_va, surf_w, surf_h, surf_bpp, zeta_va;
    uint32_t stage_program, rc_control, final0, final1, dot_mapping, other_input;
    uint32_t tex_va[4], tex_w[4], tex_h[4], tex_fmt[4], tex_pitch[4];
    uint8_t tex_cube[4], tex_valid[4];
    char tex_res[4][24];
    uint32_t blend_en, blend_src, blend_dst, blend_eq, color_mask;
    uint32_t alpha_en, alpha_func, alpha_ref;
    uint32_t depth_en, depth_func, depth_write, cull_en, cull_face;
    uint32_t stencil_en, stencil_func, stencil_ref;
    uint32_t fog_en, fog_mode, fog_gen, fog_color;
    float fog_p0, fog_p1;
    char outcome[16];
    char reason[48];
    /* perf12: full combiner and sampler state, for shader/lighting triage */
    Nv2aCombiner rc;
    uint8_t has_rc;
    uint32_t tex_addr[4], tex_filter[4], tex_border_src[4], tex_border[4];
    uint8_t has_zeq;
    uint32_t poff_en;
    float poff_scale, poff_bias;
    uint64_t zeq_eq, zeq_le, zeq_ge;
} DlRec;
static DlRec *s_dl[2];
static unsigned s_dl_n[2], s_dl_drop[2];
static uint32_t s_dl_gen[2];
static int s_dl_cur;
static DlRec *s_dl_rec;

static void dl_copy(char *dst, size_t cap, const char *a, const char *b)
{
    size_t n = 0;
    while (a && *a && n + 1 < cap) dst[n++] = *a++;
    while (b && *b && n + 1 < cap) dst[n++] = *b++;
    dst[n] = 0;
}

static float dl_regf(uint32_t m)
{
    uint32_t v = nv2a_backend_reg(m);
    float f;
    memcpy(&f, &v, 4);
    return f;
}


static void dl_begin(char path, uint64_t vsh, uint32_t count, const Nv2aSurface *s,
                     const Nv2aXBatch *xb)
{
    DlRec *r;
    int st;
    probe_check(s_dl_seq, xb);
    s_dl_rec = NULL;
    if (!halo2_screenshot_enabled())
        return;
    if (!s_dl[0]) {
        s_dl[0] = (DlRec *)calloc(DL_CAP, sizeof(DlRec));
        s_dl[1] = (DlRec *)calloc(DL_CAP, sizeof(DlRec));
        if (!s_dl[0] || !s_dl[1]) {
            free(s_dl[0]); free(s_dl[1]);
            s_dl[0] = s_dl[1] = NULL;
            return;
        }
        s_dl_gen[s_dl_cur] = s_gen;
    }
    if (s_dl_n[s_dl_cur] >= DL_CAP) {
        s_dl_drop[s_dl_cur]++;
        return;
    }
    r = &s_dl[s_dl_cur][s_dl_n[s_dl_cur]++];
    memset(r, 0, sizeof *r);
    s_dl_rec = r;
    r->gen = s_gen;
    r->seq = s_dl_seq++;
    r->path = path;
    r->vsh = vsh;
    r->count = count;
    if (s) {
        r->surf_va = s->color_va; r->surf_w = s->width; r->surf_h = s->height;
        r->surf_bpp = s->bytes_per_pixel;
    }
    if (xb && xb->state) {
        const Nv2aRenderState *rs = xb->state;
        r->zeta_va = rs->zeta_va;
        r->blend_en = rs->blend_enable; r->blend_src = rs->blend_src;
        r->blend_dst = rs->blend_dst; r->blend_eq = rs->blend_eq;
        r->color_mask = rs->color_mask;
        r->alpha_en = rs->alpha_test_enable; r->alpha_func = rs->alpha_func;
        r->alpha_ref = rs->alpha_ref;
        r->depth_en = rs->depth_test_enable; r->depth_func = rs->depth_func;
        r->depth_write = rs->depth_write;
        r->cull_en = rs->cull_enable; r->cull_face = rs->cull_face;
        r->stencil_en = rs->stencil_enable; r->stencil_func = rs->stencil_func;
        r->stencil_ref = rs->stencil_ref;
    }
    if (xb && xb->rc) {
        r->stage_program = xb->rc->stage_program;
        r->rc_control = xb->rc->control;
        r->final0 = xb->rc->final0;
        r->final1 = xb->rc->final1;
        r->rc = *xb->rc;
        r->has_rc = 1;
    }
    r->dot_mapping = nv2a_backend_reg(0x1E74);
    r->other_input = nv2a_backend_reg(0x1E78);
    r->fog_en = nv2a_backend_reg(0x02A4);
    r->fog_mode = nv2a_backend_reg(0x029C);
    r->fog_gen = nv2a_backend_reg(0x02A0);
    r->fog_color = nv2a_backend_reg(0x02A8);
    r->fog_p0 = dl_regf(0x09C0);
    r->fog_p1 = dl_regf(0x09C4);
    r->poff_en = nv2a_backend_reg(0x0384) | nv2a_backend_reg(0x0380) << 1 | nv2a_backend_reg(0x037C) << 2;
    r->poff_scale = dl_regf(0x0A78);
    r->poff_bias = dl_regf(0x0A7C);
    for (st = 0; st < 4; st++) {
        r->tex_res[st][0] = '-';
        if (!xb || !xb->tex_valid[st])
            continue;
        r->tex_valid[st] = 1;
        r->tex_va[st] = xb->tex[st].offset;
        r->tex_w[st] = xb->tex[st].width;
        r->tex_h[st] = xb->tex[st].height;
        r->tex_fmt[st] = xb->tex[st].color;
        r->tex_pitch[st] = xb->tex[st].pitch;
        r->tex_cube[st] = xb->tex_cube[st];
        r->tex_addr[st] = xb->tex[st].addr_u | (xb->tex[st].addr_v << 4);
        r->tex_filter[st] = xb->tex[st].filter;
        r->tex_border_src[st] = xb->tex[st].border_src;
        r->tex_border[st] = xb->tex[st].border;
    }
}

static void dl_stage(int st, int ok, const char *fail)
{
    if (!s_dl_rec || st < 0 || st > 3)
        return;
    if (ok)
        dl_copy(s_dl_rec->tex_res[st], sizeof s_dl_rec->tex_res[st],
                fail ? "stale:" : "ok", fail);
    else
        dl_copy(s_dl_rec->tex_res[st], sizeof s_dl_rec->tex_res[st],
                "soft:", fail ? fail : "invalid");
}

static void dl_outcome(const char *outcome, const char *reason)
{
    if (!s_dl_rec)
        return;
    if (outcome && (!s_dl_rec->outcome[0] || strcmp(s_dl_rec->outcome, "drawn") == 0 ||
                    strcmp(outcome, "drawn") != 0))
        dl_copy(s_dl_rec->outcome, sizeof s_dl_rec->outcome, outcome, NULL);
    if (reason && !s_dl_rec->reason[0])
        dl_copy(s_dl_rec->reason, sizeof s_dl_rec->reason, reason, NULL);
}

static void dl_reason(const char *prefix, const char *detail)
{
    if (s_dl_rec && !s_dl_rec->reason[0])
        dl_copy(s_dl_rec->reason, sizeof s_dl_rec->reason, prefix, detail);
}

static void dl_flip(void)
{
    if (!s_dl[0])
        return;
    s_dl_cur ^= 1;
    s_dl_n[s_dl_cur] = 0;
    s_dl_drop[s_dl_cur] = 0;
    s_dl_gen[s_dl_cur] = s_gen + 1;
    s_dl_rec = NULL;
}
#endif
static int s_tex_strict_bounds;
static int s_tex_physical_read;

typedef enum {
    GUEST_COPY_OK,
    GUEST_COPY_INVALID,
    GUEST_COPY_SHORT,
    GUEST_COPY_UNREGISTERED,
    GUEST_COPY_CHANGED,
    GUEST_COPY_UNREADABLE
} GuestCopyResult;

enum GuestBoundsPolicy {
    GUEST_BOUNDS_IGNORE,
    GUEST_BOUNDS_REQUIRED,
    GUEST_BOUNDS_TRACK_IF_REGISTERED
};

/* The RAM views and the separate contiguous/tiled views have distinct host
 * backing. Keep allocation identity in the appropriate backing domain. */
static int guest_data_span(uint32_t va, size_t bytes, uint64_t *physical)
{
    uint64_t mapped = xbox_GetMappedSize(), end, relative;
    uint64_t tiled_size = mapped < XBOX_CONTIG_SIZE ? mapped : XBOX_CONTIG_SIZE;
    if (!mapped || !va || !bytes || bytes > XBOX_CONTIG_SIZE)
        return 0;
    end = (uint64_t)va + bytes;
    if (va < XBOX_CONTIG_BASE && (uint64_t)va < mapped * (1ull + XBOX_NUM_MIRRORS)) {
        relative = va % mapped;
        if (end > XBOX_CONTIG_BASE || bytes > mapped - relative)
            return 0;
        *physical = relative;
        return 1;
    }
    if (va >= XBOX_CONTIG_BASE &&
        (uint64_t)va < (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE) {
        relative = (uint64_t)va - XBOX_CONTIG_BASE;
        if (bytes > XBOX_CONTIG_SIZE - relative)
            return 0;
        *physical = relative;
        return 2;
    }
    if (va >= XBOX_TILED_BASE &&
        (uint64_t)va < (uint64_t)XBOX_TILED_BASE + tiled_size) {
        relative = (uint64_t)va - XBOX_TILED_BASE;
        if (bytes > tiled_size - relative)
            return 0;
        *physical = relative;
        return 2;
    }
    return 0;
}

static uint64_t guest_allocation_identity(uint32_t va, uint32_t *remaining)
{
    uint64_t physical;
    int domain = guest_data_span(va, 1, &physical);
    if (domain == 1)
        return xbox_HeapBlockIdentity((uint32_t)physical, remaining);
    if (domain == 2)
        return xbox_ContiguousBlockIdentity(XBOX_CONTIG_BASE + (uint32_t)physical, remaining);
    if (remaining)
        *remaining = 0;
    return 0;
}

static GuestCopyResult checked_guest_copy(uint32_t va, size_t bytes, void *dest,
                                          int bounds_policy, GuestSpanIdentity *identity,
                                          enum GuestReadKind kind)
{
    GuestSpanIdentity before = {0}, after = {0};
    uint32_t remaining, after_remaining;
    uintptr_t offset = (uintptr_t)xbox_GetMemoryOffset(), host;
    uint64_t physical, start = 0;
    SIZE_T copied = 0;
    GuestCopyResult result = GUEST_COPY_INVALID;
    int perf = g_bruteforce_perf_counters_enabled;
    uint64_t read_start = 0;
    BOOL readable;

    if (perf) start = bruteforce_perf_clock();
    if (!guest_data_span(va, bytes, &physical) || offset > UINTPTR_MAX - va)
        goto done;
    host = offset + va;
    if (bytes > UINTPTR_MAX - host)
        goto done;
    before.layout = xbox_MemoryLayoutGeneration();
    if (!before.layout)
        goto done;
    before.allocation = guest_allocation_identity(va, &remaining);
    if (remaining && (!before.allocation || bytes > remaining)) {
        result = GUEST_COPY_SHORT;
        goto done;
    }
    if (bounds_policy != GUEST_BOUNDS_IGNORE) {
        before.bounds = xbox_GuestBoundsIdentity(va, &remaining);
        if (!before.bounds && bounds_policy == GUEST_BOUNDS_REQUIRED) {
            result = GUEST_COPY_UNREGISTERED;
            goto done;
        }
        if (before.bounds && bytes > remaining) {
            result = GUEST_COPY_SHORT;
            goto done;
        }
    }
    if (perf)
        read_start = bruteforce_perf_clock();
    readable = ReadProcessMemory(GetCurrentProcess(), (const void *)host, dest, bytes, &copied);
    if (perf) {
        s_st.guest_reads[kind].calls++;
        s_st.guest_reads[kind].bytes += copied;
        s_st.guest_reads[kind].ticks += bruteforce_perf_clock() - read_start;
    }
    if (!readable || copied != bytes) {
        result = GUEST_COPY_UNREADABLE;
        goto done;
    }
    after.layout = xbox_MemoryLayoutGeneration();
    after.allocation = guest_allocation_identity(va, &after_remaining);
    if (bounds_policy != GUEST_BOUNDS_IGNORE)
        after.bounds = xbox_GuestBoundsIdentity(va, &remaining);
    if (before.layout != after.layout || before.allocation != after.allocation ||
        before.bounds != after.bounds || (after_remaining && bytes > after_remaining) ||
        (after.bounds && bytes > remaining)) {
        result = GUEST_COPY_CHANGED;
        goto done;
    }
    if (identity) *identity = before;
    result = GUEST_COPY_OK;
done:
    if (perf)
        bruteforce_perf_record_guest_range(result == GUEST_COPY_OK, 0, 0, 0,
                                           bruteforce_perf_clock() - start, 0);
    return result;
}

static int guest_identity_still_valid(uint32_t va, size_t bytes, int bounds_policy,
                                      const GuestSpanIdentity *expected)
{
    uint32_t remaining;
    uint64_t allocation, bounds = 0;
    if (!expected->layout || xbox_MemoryLayoutGeneration() != expected->layout)
        return 0;
    allocation = guest_allocation_identity(va, &remaining);
    if (allocation != expected->allocation || (remaining && bytes > remaining))
        return 0;
    if (bounds_policy != GUEST_BOUNDS_IGNORE) {
        bounds = xbox_GuestBoundsIdentity(va, &remaining);
        if ((!bounds && bounds_policy == GUEST_BOUNDS_REQUIRED) ||
            (bounds && bytes > remaining))
            return 0;
    }
    return bounds == expected->bounds &&
           xbox_MemoryLayoutGeneration() == expected->layout;
}

static int guest_physical_bytes_match(uint32_t va, const void *snapshot, size_t bytes,
                                      const GuestSpanIdentity *expected)
{
    uint64_t physical;
    return s_tex_physical_read && !s_tex_strict_bounds && snapshot && expected->allocation &&
           guest_data_span(va, bytes, &physical) == 2 &&
           guest_identity_still_valid(va, bytes, GUEST_BOUNDS_IGNORE, expected) &&
           xbox_ContiguousGpuBytesEqual((uint32_t)physical, snapshot, bytes) &&
           guest_identity_still_valid(va, bytes, GUEST_BOUNDS_IGNORE, expected);
}

static int query_suspend_clear(void);
static void query_resume_clear(int counting);
static void bind_targets(Surf *s, Depth *d);
static void surf_resolve(Surf *s);
static void gpu_work_begin(void);
static void gpu_work_checkpoint(void);
static void gpu_work_finish(void);

static void restore_targets(void)
{
    if (s_cur)
        bind_targets(s_cur, s_cur_depth);
    else
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
}

/* Debug: RECOMP_GPU_TOGGLE_DIR=<dir> turns a fix off while <dir>/<name>
 * exists (polled about every 1024 calls), for live A/B tests:
 * wbuffer_off, argb_off, bias_off. */
static int toggle_off(const char *name, unsigned *poll, int *state)
{
    static const char *dir;
    static int init;
    char path[512];
    if (!init) {
        dir = getenv("RECOMP_GPU_TOGGLE_DIR");
        init = 1;
    }
    if (!dir)
        return 0;
    if (((*poll)++ & 1023u) == 0) {
        int now;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        now = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        if (now != *state)
            fprintf(stderr, "[TOGGLE] %s %s\n", name, now ? "set" : "cleared");
        *state = now;
    }
    return *state;
}

static int depth_argb_alias_on(void)
{
    static int on = -1, off_state;
    static unsigned poll;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_DEPTH_ARGB");
        on = !(e && e[0] == '0');
    }
    return on && !toggle_off("argb_off", &poll, &off_state);
}

static int depth_views_enabled(void)
{
    return s_depth_texture || s_stencil_alias;
}

static void release_depth(Depth *d)
{
    if (d->packed_uav) ID3D11UnorderedAccessView_Release(d->packed_uav);
    if (d->packed_srv) ID3D11ShaderResourceView_Release(d->packed_srv);
    if (d->packed) ID3D11Texture2D_Release(d->packed);
    if (d->snapshot_stencil_srv) ID3D11ShaderResourceView_Release(d->snapshot_stencil_srv);
    if (d->stencil_srv) ID3D11ShaderResourceView_Release(d->stencil_srv);
    if (d->snapshot_srv) ID3D11ShaderResourceView_Release(d->snapshot_srv);
    if (d->snapshot) ID3D11Texture2D_Release(d->snapshot);
    if (d->srv) ID3D11ShaderResourceView_Release(d->srv);
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    if (d->msaa_dsv) ID3D11DepthStencilView_Release(d->msaa_dsv);
    if (d->msaa_tex) ID3D11Texture2D_Release(d->msaa_tex);
    free(d->guest_shadow);
    d->guest_shadow = NULL;
    d->msaa_dsv = NULL;
    d->msaa_tex = NULL;
    d->msaa_count = 1;
}

static DXGI_FORMAT surf_format(uint32_t bpp)
{
    return bpp == 2 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
}

static void release_surf(Surf *s)
{
    if (s->sample_copy_srv) ID3D11ShaderResourceView_Release(s->sample_copy_srv);
    if (s->sample_copy) ID3D11Texture2D_Release(s->sample_copy);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    if (s->msaa_rtv) ID3D11RenderTargetView_Release(s->msaa_rtv);
    if (s->msaa_tex) ID3D11Texture2D_Release(s->msaa_tex);
    s->sample_copy_srv = NULL; s->sample_copy = NULL;
    s->srv = NULL; s->rtv = NULL; s->tex = NULL;
    s->msaa_rtv = NULL; s->msaa_tex = NULL;
    s->msaa_count = 1; s->msaa_dirty = 0;
}

static UINT msaa_supported(const UINT *table, UINT want)
{
    UINT n = want;
    if (n <= 1)
        return 1;
    while (n > 1) {
        if (n < 9 && table[n])
            return n;
        n >>= 1;
    }
    return 1;
}

static UINT msaa_color_samples(void)
{
    return msaa_supported(s_msaa_ok_color, s_msaa_want);
}

static UINT msaa_depth_samples(void)
{
    if (depth_views_enabled())
        return 1;
    return msaa_supported(s_msaa_ok_depth, s_msaa_want);
}

static void msaa_query_device(void)
{
    UINT n, q;
    if (!s_dev || s_msaa_queried)
        return;
    memset(s_msaa_ok_color, 0, sizeof s_msaa_ok_color);
    memset(s_msaa_ok_depth, 0, sizeof s_msaa_ok_depth);
    s_msaa_ok_color[1] = s_msaa_ok_depth[1] = 1;
    for (n = 2; n <= 8; n <<= 1) {
        q = 0;
        if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(
                s_dev, DXGI_FORMAT_B8G8R8A8_UNORM, n, &q)) && q > 0)
            s_msaa_ok_color[n] = 1;
        q = 0;
        if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(
                s_dev, DXGI_FORMAT_D24_UNORM_S8_UINT, n, &q)) && q > 0)
            s_msaa_ok_depth[n] = 1;
    }
    s_msaa_queried = 1;
    fprintf(stderr, "[GPU-D3D11] MSAA support 2x=%s 4x=%s 8x=%s (color); depth 2x=%s 4x=%s 8x=%s\n",
            s_msaa_ok_color[2] ? "yes" : "no",
            s_msaa_ok_color[4] ? "yes" : "no",
            s_msaa_ok_color[8] ? "yes" : "no",
            s_msaa_ok_depth[2] ? "yes" : "no",
            s_msaa_ok_depth[4] ? "yes" : "no",
            s_msaa_ok_depth[8] ? "yes" : "no");
}

static void tex_index_rebuild(void);

void halo2_gpu_set_aniso(int level)
{
    int enable_mips = 0;
    if (level >= 16) level = 16;
    else if (level >= 8) level = 8;
    else if (level >= 4) level = 4;
    else if (level >= 2) level = 2;
    else level = 0;
    s_tex_aniso = level;
    /* Turning aniso on also turns on mip chains, which are part of the
     * texture-descriptor hash. Rebuild the index before any further lookup
     * or evict, or remove will walk the wrong bucket and abort. Mips stay
     * on if aniso is later turned off, so the keys stay stable. */
    if (level >= 2 && !s_tex_mips) {
        s_tex_mips = 1;
        enable_mips = 1;
    }
    if (enable_mips) {
        tex_index_rebuild();
        fprintf(stderr, "[GPU-MIPS] anisotropic filtering %dx enabled;"
                        " rebuilt texture descriptor index\n", s_tex_aniso);
        fflush(stderr);
    }
}

int halo2_gpu_aniso(void)
{
    return s_tex_aniso;
}

void halo2_gpu_set_msaa(int samples)
{
    UINT n = 1;
    if (samples >= 8) n = 8;
    else if (samples >= 4) n = 4;
    else if (samples >= 2) n = 2;
    if (n == s_msaa_want)
        return;
    s_msaa_want = n;
    /* Existing surfaces rebuild on the next bind if their sample count differs. */
    fprintf(stderr, "[GPU-D3D11] MSAA set to %ux\n", (unsigned)n);
}

int halo2_gpu_msaa(void)
{
    return (int)s_msaa_want;
}

void halo2_gpu_set_fxaa(int on)
{
    halo2_fxaa_set_enabled(on ? 1 : 0);
    if (halo2_fxaa_enabled()) {
        if (!s_direct_present) {
            s_direct_present = 1;
            fprintf(stderr, "[GPU-D3D11] shared GPU presentation enabled for FXAA\n");
        }
        fprintf(stderr, "[FXAA] enabled (applied at present)\n");
    } else
        fprintf(stderr, "[FXAA] disabled\n");
    fflush(stderr);
}

int halo2_gpu_msaa_supported(int samples)
{
    UINT n = (UINT)samples;
    if (n <= 1)
        return 1;
    if (n > 8 || !s_msaa_queried)
        return 0;
    return s_msaa_ok_color[n] && s_msaa_ok_depth[n];
}

static void surf_resolve_raw(Surf *s)
{
    if (!s || !s->msaa_dirty || !s->msaa_tex || !s->tex || !s_ctx)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)s->tex, 0,
        (ID3D11Resource *)s->msaa_tex, 0, surf_format(s->bpp));
    s->msaa_dirty = 0;
}

static void surf_resolve(Surf *s)
{
    surf_resolve_raw(s);
    if (s_cur)
        bind_targets(s_cur, s_cur_depth);
}

static int create_color_msaa(Surf *s)
{
    D3D11_TEXTURE2D_DESC td;
    UINT n;
    HRESULT hr;

    s->msaa_tex = NULL;
    s->msaa_rtv = NULL;
    s->msaa_count = 1;
    s->msaa_dirty = 0;
    n = msaa_color_samples();
    if (n <= 1 || s->bpp != 4)
        return 1;
    if (msaa_depth_samples() < n)
        n = msaa_depth_samples();
    if (n <= 1)
        return 1;
    memset(&td, 0, sizeof td);
    td.Width = s->pw;
    td.Height = s->ph;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = surf_format(s->bpp);
    td.SampleDesc.Count = n;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->msaa_tex);
    if (FAILED(hr))
        return 1; /* keep single-sample */
    hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->msaa_tex, NULL, &s->msaa_rtv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(s->msaa_tex);
        s->msaa_tex = NULL;
        return 1;
    }
    s->msaa_count = n;
    return 1;
}

static int depth_argb_alias_on(void);
static int depth_views_enabled(void);
/* Depth textures carry a shader view: the full alias modes, or the
 * lightweight depth-as-colour alias (no guest-memory tracking). */
static int depth_srv_capable(void) { return depth_views_enabled() || depth_argb_alias_on(); }
static int create_depth_msaa(Depth *d)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv;
    UINT n;
    HRESULT hr;

    d->msaa_tex = NULL;
    d->msaa_dsv = NULL;
    d->msaa_count = 1;
    d->msaa_dirty = 0;
    n = msaa_depth_samples();
    if (n <= 1)
        return 1;
    memset(&td, 0, sizeof td);
    td.Width = d->w;
    td.Height = d->h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    /* Typeless so ResolveSubresource can copy into the resolved R24G8 texture. */
    td.Format = depth_srv_capable() ? DXGI_FORMAT_R24G8_TYPELESS
                                      : DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = n;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->msaa_tex);
    if (FAILED(hr))
        return 1;
    memset(&dv, 0, sizeof dv);
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    hr = ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->msaa_tex, &dv, &d->msaa_dsv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(d->msaa_tex);
        d->msaa_tex = NULL;
        return 1;
    }
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->msaa_dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    d->msaa_count = n;
    return 1;
}

static void release_surf_guest(Surf *s)
{
    if (s->guest_srv) ID3D11ShaderResourceView_Release(s->guest_srv);
    if (s->guest_tex) ID3D11Texture2D_Release(s->guest_tex);
    free(s->guest_shadow);
    s->guest_srv = NULL;
    s->guest_tex = NULL;
    s->guest_shadow = NULL;
    s->guest_shadow_bytes = 0;
    s->guest_check_gen = 0;
    s->gpu_dirty = 0;
    s->guest_exact = 0;
}

static size_t surf_guest_row(const Surf *s)
{
    size_t row = (size_t)s->w * s->bpp;
    return row <= s->pitch ? row : s->pitch;
}

static int ensure_guest_shadow(Surf *s)
{
    size_t row = surf_guest_row(s);
    size_t bytes;
    uint8_t *shadow;

    if (!row || !s->h || s->h > SIZE_MAX / row)
        return 0;
    bytes = row * s->h;
    if (s->guest_shadow && s->guest_shadow_bytes == bytes)
        return 1;
    shadow = (uint8_t *)realloc(s->guest_shadow, bytes);
    if (!shadow) {
        fprintf(stderr, "[GPU-D3D11] cannot allocate %zu-byte guest color shadow for %08X\n",
                bytes, s->va);
        return 0;
    }
    s->guest_shadow = shadow;
    s->guest_shadow_bytes = bytes;
    return 1;
}

static uint8_t *s_surface_probe;
static size_t s_surface_probe_cap;

static int surface_guest_span(const Surf *s, size_t *span)
{
    size_t row = surf_guest_row(s);
    if (!row || !s->h ||
        (s->pitch && (size_t)(s->h - 1) > (SIZE_MAX - row) / s->pitch))
        return 0;
    *span = (size_t)(s->h - 1) * s->pitch + row;
    return 1;
}

static int copy_guest_surface(const Surf *s, GuestSpanIdentity *identity)
{
    size_t span;
    uint8_t *probe;
    if (!surface_guest_span(s, &span))
        return 0;
    if (span > s_surface_probe_cap) {
        probe = (uint8_t *)realloc(s_surface_probe, span);
        if (!probe) return 0;
        s_surface_probe = probe;
        s_surface_probe_cap = span;
    }
    return checked_guest_copy(s->va, span, s_surface_probe,
                              s_tex_strict_bounds ? GUEST_BOUNDS_TRACK_IF_REGISTERED
                                                  : GUEST_BOUNDS_IGNORE,
                              identity, GUEST_READ_SURFACE) == GUEST_COPY_OK;
}

static int same_guest_identity(const GuestSpanIdentity *a, const GuestSpanIdentity *b)
{
    return a->layout && a->layout == b->layout && a->allocation == b->allocation &&
           a->bounds == b->bounds;
}

static int surf_identity_current(const Surf *s)
{
    size_t span;
    return surface_guest_span(s, &span) &&
           guest_identity_still_valid(s->va, span,
               s_tex_strict_bounds ? GUEST_BOUNDS_TRACK_IF_REGISTERED : GUEST_BOUNDS_IGNORE,
               &s->identity);
}

static void shadow_from_probe(Surf *s)
{
    size_t row = surf_guest_row(s);
    uint32_t y;
    for (y = 0; y < s->h; ++y)
        memcpy(s->guest_shadow + (size_t)y * row,
               s_surface_probe + (size_t)y * s->pitch, row);
}

static int snapshot_guest_surface(Surf *s)
{
    GuestSpanIdentity identity;

    if (!ensure_guest_shadow(s) || !copy_guest_surface(s, &identity) ||
        (s->identity.layout && !same_guest_identity(&s->identity, &identity)))
        return 0;
    s->identity = identity;
    shadow_from_probe(s);
    return 1;
}

static int guest_surface_changed(const Surf *s)
{
    size_t row = surf_guest_row(s);
    uint32_t y;
    GuestSpanIdentity identity;

    if (s->pitch == row && s->guest_shadow_bytes == row * s->h &&
        guest_physical_bytes_match(s->va, s->guest_shadow, s->guest_shadow_bytes, &s->identity)) {
        s_st.surface_physical_hits++;
        return 0;
    }
    if (!copy_guest_surface(s, &identity) || !same_guest_identity(&s->identity, &identity))
        return -1;
    if (!s->guest_shadow || s->guest_shadow_bytes != row * s->h)
        return 1;
    for (y = 0; y < s->h; ++y)
        if (memcmp(s->guest_shadow + (size_t)y * row,
                   s_surface_probe + (size_t)y * s->pitch, row))
            return 1;
    return 0;
}

/* The preceding checked read rejects existing guards. SEH writes respect
 * read-only protection, unlike debugger-oriented WriteProcessMemory on some
 * executable pages. */
/* Bumped by every backend write to guest memory: a texture verified
 * against guest bytes this frame is verified again after one. */
static volatile LONG s_tex_epoch = 1;
/* The guest ranges those writes covered, by epoch, so a write only re-arms
 * the compare for textures it overlaps. Addresses are compared modulo
 * 64 MB, which folds the contiguous (0x80000000/0xF0000000) and physical
 * views of a page together: aliases can only add overlaps, never hide one. */
#define TEX_WR_RING 64
#define TEX_WR_MASK 0x03FFFFFFu
static struct { uint32_t lo, hi; LONG epoch; } s_tex_wr[TEX_WR_RING];

static int guarded_guest_write(void *destination, const void *source, size_t bytes)
{
    {
        LONG e = InterlockedIncrement(&s_tex_epoch);
        uintptr_t va = (uintptr_t)destination - (uintptr_t)GUEST(0);
        uint32_t lo = (uint32_t)va & TEX_WR_MASK;
        uint64_t hi = (uint64_t)lo + bytes;
        int slot = (int)((uint32_t)e % TEX_WR_RING);
        s_tex_wr[slot].lo = hi > TEX_WR_MASK + 1ull ? 0 : lo;       /* wraps: everything */
        s_tex_wr[slot].hi = hi > TEX_WR_MASK + 1ull ? TEX_WR_MASK + 1u : (uint32_t)hi;
        s_tex_wr[slot].epoch = e;
    }
    __try {
        memcpy(destination, source, bytes);
        return 1;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                GetExceptionCode() == EXCEPTION_GUARD_PAGE
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}

#ifdef BRUTEFORCE_D3D11_TESTING
int bruteforce_d3d11_test_guarded_write(void *destination, const void *source, size_t bytes)
{
    return guarded_guest_write(destination, source, bytes);
}
#endif

/* RECOMP_GPU_SURFACE_FAST (default on, A/B bit 16): the backend's own writes
 * of a color surface into guest memory are trusted instead of re-read. A
 * guest CPU write racing one of those writes is seen at the next frame. */
static int surf_fast_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_SURFACE_FAST");
        on = !(e && e[0] == '0');
    }
    return on && !rpz_ab_off(16);
}

static int surf_write_identity(Surf *s, size_t span, GuestSpanIdentity *identity)
{
    if (surf_fast_enabled() && s->identity.layout &&
        guest_identity_still_valid(s->va, span,
            s_tex_strict_bounds ? GUEST_BOUNDS_TRACK_IF_REGISTERED : GUEST_BOUNDS_IGNORE,
            &s->identity)) {
        *identity = s->identity;
        return 1;
    }
    return copy_guest_surface(s, identity) && same_guest_identity(&s->identity, identity);
}

static int checked_write_guest_surface(Surf *s, const void *source, size_t source_pitch)
{
    GuestSpanIdentity identity;
    size_t row = surf_guest_row(s), span, bytes;
    uint32_t y, rows;
    static unsigned rejected;

    if (!source || !surface_guest_span(s, &span) || source_pitch < row ||
        (s->h - 1) > (SIZE_MAX - row) / source_pitch ||
        !surf_write_identity(s, span, &identity))
        goto failed;
    rows = s->pitch == row && source_pitch == row ? s->h : 1;
    bytes = row * rows;
    for (y = 0; y < s->h; y += rows) {
        if (!guarded_guest_write(GUEST(s->va) + (size_t)y * s->pitch,
                                 (const uint8_t *)source + (size_t)y * source_pitch, bytes))
            goto failed;
    }
    if (!guest_identity_still_valid(s->va, span,
            s_tex_strict_bounds ? GUEST_BOUNDS_TRACK_IF_REGISTERED : GUEST_BOUNDS_IGNORE,
            &identity))
        goto failed;
    return 1;
failed:
    if (rejected < 32) {
        ++rejected;
        fprintf(stderr, "[GPU-D3D11] checked guest color write for %08X failed or changed%s\n",
                s->va, rejected == 32 ? " (further reports suppressed)" : "");
    }
    return 0;
}

static int validate_seed_shadow(Surf *s)
{
    int changed = guest_surface_changed(s);
    if (changed < 0)
        return 0;
    if (changed) {
        s->gpu_dirty = 1;
        s->guest_check_gen = 0;
    }
    return 1;
}

/* Seeded from guest memory: whatever the title (or the boot) left there.
 * After this the GPU copy is authoritative. */
static int create_surf(Surf *s, ID3D11ShaderResourceView *gpu_seed,
                       uint32_t gpu_seed_width, uint32_t gpu_seed_height)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init, *pinit = NULL;
    ID3D11Texture2D *seed = NULL;
    ID3D11ShaderResourceView *seed_srv = NULL;
    HRESULT hr;

    s->sample_copy = NULL;
    s->sample_copy_srv = NULL;
    memset(&td, 0, sizeof td);
    td.Width = s->pw;
    td.Height = s->ph;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = surf_format(s->bpp);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (s->pw == s->w && s->ph == s->h && s->pitch >= s->w * s->bpp) {
        init.pSysMem = s->guest_shadow;
        init.SysMemPitch = (UINT)surf_guest_row(s);
        init.SysMemSlicePitch = 0;
        pinit = &init;
    }
    hr = scale_test_failure("color-texture") ? E_OUTOFMEMORY :
         ID3D11Device_CreateTexture2D(s_dev, &td, pinit, &s->tex);
    if (gpu_call_failed(hr, "creating color surface"))
        return 0;
#ifdef BRUTEFORCE_D3D11_TESTING
    if (scale_test_failure("guest-change-seed"))
        *(uint32_t *)GUEST(s->va) ^= 1u;
#endif
    hr = scale_test_failure("color-rtv") ? E_OUTOFMEMORY :
         ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv);
    if (gpu_call_failed(hr, "creating color render-target view")) {
        release_surf(s);
        return 0;
    }
    hr = scale_test_failure("color-srv") ? E_OUTOFMEMORY :
         ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    if (gpu_call_failed(hr, "creating color shader-resource view")) {
        release_surf(s);
        return 0;
    }
    if (!create_color_msaa(s)) {
        release_surf(s);
        return 0;
    }
    if (gpu_seed) {
        int counting, ok;
        enum GpuScaleBlitMode mode =
            gpu_seed_width > s->pw || gpu_seed_height > s->ph ? GPU_SCALE_BLIT_BOX
                                                               : GPU_SCALE_BLIT_BILINEAR;
        counting = query_suspend_clear();
        ok = !scale_test_failure("color-migrate") &&
             d3d_scale_blit(gpu_seed, gpu_seed_width, gpu_seed_height,
                            s->rtv, s->pw, s->ph, mode);
        restore_targets();
        query_resume_clear(counting);
        if (!ok) {
            gpu_d3d11_check_device(s_dev, S_OK, "migrating scaled color surface");
            fprintf(stderr, "[GPU-SCALE] migrating scaled color surface %08X failed\n", s->va);
            release_surf(s);
            return 0;
        }
    } else if ((s->pw != s->w || s->ph != s->h) && s->pitch >= s->w * s->bpp) {
        td.Width = s->w;
        td.Height = s->h;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = s->guest_shadow;
        init.SysMemPitch = (UINT)surf_guest_row(s);
        init.SysMemSlicePitch = 0;
        hr = ID3D11Device_CreateTexture2D(s_dev, &td, &init, &seed);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)seed,
                                                       NULL, &seed_srv);
        if (SUCCEEDED(hr)) {
            int counting = query_suspend_clear();
            int ok = d3d_scale_blit(seed_srv, s->w, s->h, s->rtv, s->pw, s->ph,
                                    GPU_SCALE_BLIT_BILINEAR);
            restore_targets();
            query_resume_clear(counting);
            if (!ok)
                hr = E_FAIL;
        }
        if (FAILED(hr)) {
            gpu_d3d11_check_device(s_dev, hr, "seeding scaled color surface");
            fprintf(stderr, "[GPU-SCALE] scaled color seed for %08X failed 0x%08lX\n",
                    s->va, (unsigned long)hr);
            if (seed_srv) ID3D11ShaderResourceView_Release(seed_srv);
            if (seed) ID3D11Texture2D_Release(seed);
            release_surf(s);
            return 0;
        }
        ID3D11ShaderResourceView_Release(seed_srv);
        ID3D11Texture2D_Release(seed);
    }
    s->guest_exact = 0;
    if (s->msaa_rtv && s->srv) {
        int counting = query_suspend_clear();
        int ok = d3d_scale_blit(s->srv, s->pw, s->ph, s->msaa_rtv, s->pw, s->ph,
                                GPU_SCALE_BLIT_BILINEAR);
        restore_targets();
        query_resume_clear(counting);
        if (ok)
            s->msaa_dirty = 0;
    }
    return s->rtv && s->srv;
}

static Surf *find_surf(uint32_t va)
{
    int i;
    for (i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == va && s_surf[i].tex && surf_identity_current(&s_surf[i]))
            return &s_surf[i];
    return NULL;
}

/* A surface's width is its pitch in pixels where that is at least the clip
 * width, so a texture reading the same memory with that pitch sees the same
 * layout. */
static int surf_relayout_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_SURF_RELAYOUT");
        on = !(e && e[0] == '0');
    }
    return on && !rpz_ab_off(64);
}

static unsigned s_relayout_rb, s_relayout_rb_fail;
unsigned s_relayout_rb_n(void) { return s_relayout_rb; }
unsigned s_relayout_rb_fail_n(void) { return s_relayout_rb_fail; }

static ID3D11Texture2D *readback_source(Surf *sf);

enum {
    RELAYOUT_FAIL_NO_TEX = 0,
    RELAYOUT_FAIL_PITCH,
    RELAYOUT_FAIL_TOO_LARGE,
    RELAYOUT_FAIL_SHADOW,
    RELAYOUT_FAIL_DOWNSAMPLE,
    RELAYOUT_FAIL_STAGING,
    RELAYOUT_FAIL_WRITE
};
static unsigned s_relayout_fail_why[7];
static const char *const s_relayout_why_name[7] = {
    "no_device_or_tex", "row_exceeds_pitch", "too_large", "no_shadow",
    "scale_downsample", "create_staging", "map_or_write"
};

static void relayout_fail(Surf *s, int why)
{
    s_relayout_rb_fail++;
    if ((unsigned)why < 7)
        s_relayout_fail_why[why]++;
#ifdef RECOMP_DEBUG_TOOLS
    debug_note("relayout", (unsigned)why < 7 ? s_relayout_why_name[why] : "unknown",
               s ? s->va : 0, s ? s->w : 0, s ? s->h : 0, s ? s->pitch : 0,
               (uint32_t)why);
#endif
    {
        static unsigned detailed;
        if (detailed < 40) {
            ++detailed;
            fprintf(stderr, "[RELAYOUT] fail %s va=%08X guest %ux%u phys %ux%u pitch %u bpp %u\n",
                    (unsigned)why < 7 ? s_relayout_why_name[why] : "?",
                    s ? s->va : 0, s ? s->w : 0, s ? s->h : 0,
                    s ? s->pw : 0, s ? s->ph : 0, s ? s->pitch : 0, s ? s->bpp : 0);
        }
    }
}

static int relayout_readback(Surf *s)
{
    static struct { uint32_t w, h, bpp; ID3D11Texture2D *tex; } cache[4];
    static int next;
    ID3D11Texture2D *st = NULL, *src;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    size_t row = surf_guest_row(s);
    uint32_t y;
    int i, ok = 0;

    /* Guest images can be 640x480x4 (1.2 MB), so the old 1 MB cap rejected
     * every full framebuffer. Scale > 1x also used to bail because pw!=w. */
    if (!s_dev || !s->tex) {
        relayout_fail(s, RELAYOUT_FAIL_NO_TEX);
        return 0;
    }
    if ((uint64_t)s->w * s->bpp > s->pitch) {
        relayout_fail(s, RELAYOUT_FAIL_PITCH);
        return 0;
    }
    if ((uint64_t)s->pitch * s->h > (16u << 20)) {
        relayout_fail(s, RELAYOUT_FAIL_TOO_LARGE);
        return 0;
    }
    if (!ensure_guest_shadow(s)) {
        relayout_fail(s, RELAYOUT_FAIL_SHADOW);
        return 0;
    }
    for (i = 0; i < 4; ++i)
        if (cache[i].tex && cache[i].w == s->w && cache[i].h == s->h && cache[i].bpp == s->bpp)
            st = cache[i].tex;
    if (!st) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = s->w;
        td.Height = s->h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = surf_format(s->bpp);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st);
        if (FAILED(hr)) {
            relayout_fail(s, RELAYOUT_FAIL_STAGING);
            return 0;
        }
        if (cache[next].tex)
            ID3D11Texture2D_Release(cache[next].tex);
        cache[next].tex = st;
        cache[next].w = s->w;
        cache[next].h = s->h;
        cache[next].bpp = s->bpp;
        next = (next + 1) % 4;
    }
    surf_resolve(s);
    src = s->tex;
    if (s->pw != s->w || s->ph != s->h) {
        src = readback_source(s);
        if (!src) {
            relayout_fail(s, RELAYOUT_FAIL_DOWNSAMPLE);
            return 0;
        }
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m);
    if (SUCCEEDED(hr)) {
        if (checked_write_guest_surface(s, m.pData, m.RowPitch)) {
            for (y = 0; y < s->h; ++y)
                memcpy(s->guest_shadow + (size_t)y * row,
                       (const uint8_t *)m.pData + (size_t)y * m.RowPitch, row);
            s->gpu_dirty = 0;
            s->guest_exact = 0;
            s->guest_check_gen = s_gen;
            ok = 1;
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    if (ok) s_relayout_rb++; else relayout_fail(s, RELAYOUT_FAIL_WRITE);
    return ok;
}

static Surf *get_surf(const Nv2aSurface *in)
{
    uint32_t bpp = in->bytes_per_pixel ? in->bytes_per_pixel : 4;
    uint32_t w = in->pitch && in->pitch / bpp >= in->width ? in->pitch / bpp : in->width;
    uint32_t h = in->height;
    uint32_t aa_sx = in->aa_sx > 1 ? in->aa_sx : 1;
    uint32_t aa_sy = in->aa_sy > 1 ? in->aa_sy : 1;
    uint32_t pw, ph;
    Surf *s = NULL;
    int i, stale;

    if (aa_sx > 2 || aa_sy > 2) {
        fprintf(stderr, "[GPU-SCALE] unsupported guest anti-aliasing factors %ux%u\n",
                aa_sx, aa_sy);
        return NULL;
    }
    for (i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == in->color_va) { s = &s_surf[i]; break; }
    stale = s && !surf_identity_current(s);
    if (s && !stale &&
        (!surf_relayout_enabled() || (s->pitch == in->pitch && s->bpp == bpp))) {
        w = w > s->w ? w : s->w;
        h = h > s->h ? h : s->h;
    }
    if (!gpu_scale_surface_size(w, h, aa_sx, aa_sy, &pw, &ph))
        return NULL;
    if (s && (stale || s->bpp != bpp || s->w != w || s->h != h || s->pw != pw || s->ph != ph ||
              s->pitch != in->pitch || !s->tex || s->msaa_count != msaa_color_samples())) {
        Surf replacement = *s;
        int guest_layout_changed =
            stale || s->bpp != bpp || s->w != w || s->h != h || s->pitch != in->pitch;
        int migrate = !stale && gpu_scale_requested() && s->srv;
        int uncertain = 0;
        if (guest_layout_changed && !stale && s->tex && s->gpu_dirty &&
            surf_relayout_enabled() && relayout_readback(s))
            s->alias_uncertain = 0;
        if (guest_layout_changed && !stale && s->tex) {
            int changed = guest_surface_changed(s);
            if (changed < 0 || (changed && s->gpu_dirty) || (s->gpu_dirty && !migrate))
                uncertain = 1;
            if (changed > 0 && !s->gpu_dirty)
                migrate = 0;
        }
        if (stale)
            memset(&replacement, 0, sizeof replacement);
        replacement.va = in->color_va;
        replacement.bpp = bpp;
        replacement.w = w;
        replacement.h = h;
        replacement.pw = pw;
        replacement.ph = ph;
        replacement.aa_sx = aa_sx;
        replacement.aa_sy = aa_sy;
        replacement.scale_epoch = gpu_scale_epoch();
        replacement.pitch = in->pitch;
        replacement.tex = NULL;
        replacement.rtv = NULL;
        replacement.srv = NULL;
        replacement.sample_copy = NULL;
        replacement.sample_copy_srv = NULL;
        replacement.msaa_tex = NULL;
        replacement.msaa_rtv = NULL;
        replacement.msaa_count = 1;
        replacement.msaa_dirty = 0;
        if (guest_layout_changed) {
            replacement.guest_tex = NULL;
            replacement.guest_srv = NULL;
            replacement.guest_shadow = NULL;
            replacement.guest_shadow_bytes = 0;
            replacement.guest_check_gen = 0;
            replacement.gpu_dirty = migrate != 0;
            replacement.guest_exact = 0;
            replacement.alias_uncertain = !stale && (s->alias_uncertain || uncertain);
        }
        if ((guest_layout_changed && !snapshot_guest_surface(&replacement)) ||
            !create_surf(&replacement, migrate ? s->srv : NULL,
                         s->pw, s->ph) ||
            (guest_layout_changed && !validate_seed_shadow(&replacement))) {
            release_surf(&replacement);
            if (guest_layout_changed)
                release_surf_guest(&replacement);
            fprintf(stderr, "[GPU-D3D11] retaining prior color resources for %08X\n", s->va);
            return NULL;
        }
        if (guest_layout_changed && migrate && guest_surface_changed(s) != 0)
            replacement.alias_uncertain = 1;
        if (rtlog_on())
            fprintf(stderr, "[RTLOG] g%u replace %08X stale %d layout %d migrate %d\n", s_gen, s->va,
                    stale, guest_layout_changed, migrate);
        if (s_cur == s)
            s_cur = NULL;
        if (stale && s_last_presented == s)
            s_last_presented = NULL;
        release_surf(s);
        if (guest_layout_changed)
            release_surf_guest(s);
        *s = replacement;
    }
    if (s) {
        s->aa_sx = aa_sx;
        s->aa_sy = aa_sy;
        s->scale_epoch = gpu_scale_epoch();
    }
    if (!s) {
        if (s_nsurf == MAX_SURF) {
            fprintf(stderr, "[GPU-D3D11] color surface limit reached for %08X\n", in->color_va);
            return NULL;
        }
        s = &s_surf[s_nsurf++];
        memset(s, 0, sizeof *s);
        s->va = in->color_va;
        s->pitch = in->pitch;
        s->bpp = bpp;
        s->w = w;
        s->h = h;
        s->pw = pw;
        s->ph = ph;
        s->aa_sx = aa_sx;
        s->aa_sy = aa_sy;
        s->scale_epoch = gpu_scale_epoch();
        if (!snapshot_guest_surface(s) || !create_surf(s, NULL, 0, 0) ||
            !validate_seed_shadow(s)) {
            release_surf(s);
            release_surf_guest(s);
            s_nsurf--;
            return NULL;
        }
    }
    return s;
}

static int readable_guest_range(uint32_t va, uint64_t bytes)
{
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    uint64_t guest_end = (uint64_t)va + bytes;
    uintptr_t offset = (uintptr_t)xbox_GetMemoryOffset(), p, end;
    uint64_t total_start = 0, query_start = 0, query_ticks = 0;
    unsigned queries = 0, query_failures = 0, regions = 0;
    int accepted = 0;
    int perf = g_bruteforce_perf_counters_enabled;

    if (perf)
        total_start = bruteforce_perf_clock();
    if (!va || !bytes || bytes > XBOX_CONTIG_SIZE ||
        !(guest_end <= XBOX_CONTIG_SIZE ||
          (va >= XBOX_CONTIG_BASE && guest_end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)) ||
        offset > UINTPTR_MAX - va)
        goto done;
    p = offset + va;
    if (bytes > UINTPTR_MAX - p)
        goto done;
    end = p + (uintptr_t)bytes;
    while (p < end) {
        MEMORY_BASIC_INFORMATION info;
        uintptr_t base, available;
        SIZE_T query_result;
        if (perf)
            query_start = bruteforce_perf_clock();
        queries++;
        query_result = VirtualQuery((const void *)p, &info, sizeof info);
        if (perf)
            query_ticks += bruteforce_perf_clock() - query_start;
        if (query_result != sizeof info) {
            query_failures++;
            goto done;
        }
        regions++;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(info.Protect & readable))
            goto done;
        base = (uintptr_t)info.BaseAddress;
        if (p < base || p - base >= info.RegionSize)
            goto done;
        available = info.RegionSize - (p - base);
        p += available < end - p ? available : end - p;
    }
    accepted = 1;

done:
    if (perf)
        bruteforce_perf_record_guest_range(accepted, queries, query_failures, regions,
                                           bruteforce_perf_clock() - total_start, query_ticks);
    return accepted;
}

static int depth_guest_span(const Depth *d, size_t *span)
{
    uint64_t row = (uint64_t)d->w * 4;
    uint64_t bytes;
    if (!d->w || !d->h || d->format != 2 || d->surface_type != 1 ||
        d->pitch < row)
        return 0;
    bytes = (uint64_t)(d->h - 1) * d->pitch + row;
    if (bytes > SIZE_MAX || bytes > XBOX_CONTIG_SIZE)
        return 0;
    *span = (size_t)bytes;
    return 1;
}

static int capture_depth_guest(Depth *d)
{
    GuestSpanIdentity identity;
    uint8_t *shadow;
    size_t span;
    if (!depth_guest_span(d, &span) || !readable_guest_range(d->va, span))
        return 0;
    shadow = (uint8_t *)malloc(span);
    if (!shadow)
        return 0;
    if (checked_guest_copy(d->va, span, shadow, GUEST_BOUNDS_TRACK_IF_REGISTERED,
                           &identity, GUEST_READ_DEPTH) != GUEST_COPY_OK) {
        free(shadow);
        return 0;
    }
    if (d->identity.layout == identity.layout && d->identity.allocation &&
        !identity.allocation) {
        free(shadow);
        return 0;
    }
    free(d->guest_shadow);
    d->guest_shadow = shadow;
    d->guest_shadow_bytes = span;
    d->identity = identity;
    return 1;
}

/* A different nonzero allocation identity, or a new memory layout, proves
 * the previous resource belonged to an ended guest lifetime. */
static int depth_new_lifetime(const Depth *d, const GuestSpanIdentity *current)
{
    return d->identity.layout && current->layout &&
           (d->identity.layout != current->layout ||
            (d->identity.allocation && current->allocation &&
             d->identity.allocation != current->allocation));
}

/* -1: unreadable or unverifiable, 0: unchanged, 1: changed, 2: new lifetime. */
static int probe_depth_guest(const Depth *d)
{
    GuestSpanIdentity identity;
    size_t span;
    uint8_t *probe;
    if (!d->guest_shadow || !depth_guest_span(d, &span) ||
        span != d->guest_shadow_bytes || !readable_guest_range(d->va, span))
        return -1;
    if (span > s_surface_probe_cap) {
        probe = (uint8_t *)realloc(s_surface_probe, span);
        if (!probe) return -1;
        s_surface_probe = probe;
        s_surface_probe_cap = span;
    }
    if (checked_guest_copy(d->va, span, s_surface_probe,
                           GUEST_BOUNDS_TRACK_IF_REGISTERED, &identity,
                           GUEST_READ_DEPTH) != GUEST_COPY_OK)
        return -1;
    if (d->identity.layout == identity.layout && d->identity.allocation &&
        !identity.allocation)
        return -1;
    if (depth_new_lifetime(d, &identity))
        return 2;
    if (!same_guest_identity(&d->identity, &identity) ||
        memcmp(d->guest_shadow, s_surface_probe, span))
        return 1;
    return 0;
}

/* Xbox Z24S8 stores depth in the high 24 bits; D3D11 R24G8 stores it in
 * the low 24. Only a checked snapshot may seed a GPU depth resource. */
static uint32_t *depth_seed(const Depth *d)
{
    uint32_t *seed, x, y;
    size_t row = (size_t)d->w * 4;
    if (!d->guest_shadow || d->h > SIZE_MAX / row)
        return NULL;
    seed = (uint32_t *)malloc(row * d->h);
    if (!seed) {
        fprintf(stderr, "[GPU-D3D11] cannot allocate depth seed for surface %08X\n", d->va);
        return NULL;
    }
    for (y = 0; y < d->h; y++) {
        const uint8_t *src = d->guest_shadow + (size_t)y * d->pitch;
        for (x = 0; x < d->w; x++) {
            uint32_t value;
            memcpy(&value, src + (size_t)x * 4, sizeof value);
            seed[(size_t)y * d->w + x] = (value >> 8) | (value << 24);
        }
    }
    return seed;
}

static void depth_record_use(Depth *d, const Surf *surf)
{
    unsigned x = surf->aa_sx > 1, y = surf->aa_sy > 1;
    if (d->logical_w[x] < surf->w) d->logical_w[x] = surf->w;
    if (d->logical_h[y] < surf->h) d->logical_h[y] = surf->h;
}

static int depth_size_for(const Depth *d, float scale, uint32_t *width, uint32_t *height)
{
    uint32_t w = 0, h = 0;
    unsigned x, y;
    for (x = 0; x < 2; ++x)
        for (y = 0; y < 2; ++y) {
            uint32_t pw, ph;
            if (!d->logical_w[x] || !d->logical_h[y])
                continue;
            if (!gpu_scale_surface_size_for(scale, d->logical_w[x], d->logical_h[y],
                                            x + 1, y + 1, &pw, &ph))
                return 0;
            if (w < pw) w = pw;
            if (h < ph) h = ph;
        }
    if (!w || !h)
        return 0;
    *width = w;
    *height = h;
    return 1;
}

static int create_depth_resource(Depth *d, int existing)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    uint32_t *seed = NULL;
    int seeded = 0;
    HRESULT hr;

    memset(&td, 0, sizeof td);
    td.Width = d->w; td.Height = d->h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = depth_srv_capable() ? DXGI_FORMAT_R24G8_TYPELESS
                                      : DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL |
                   (depth_srv_capable() ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (depth_views_enabled() && !gpu_scale_requested() &&
        d->format == 2 && d->surface_type == 1) {
        if (capture_depth_guest(d) && !existing)
            seed = depth_seed(d);
        if (!d->guest_shadow || (!existing && !seed))
            d->guest_conflict = 1;
    }
    memset(&init, 0, sizeof init);
    init.pSysMem = seed;
    init.SysMemPitch = d->w * 4;
    hr = scale_test_failure("depth-texture") ? E_OUTOFMEMORY :
         ID3D11Device_CreateTexture2D(s_dev, &td, seed ? &init : NULL, &d->tex);
    if (FAILED(hr) && seed) {
        gpu_d3d11_check_device(s_dev, hr, "uploading depth seed");
        fprintf(stderr, "[GPU-D3D11] depth seed upload for %08X failed 0x%08lX;"
                        " sampling will require a GPU depth clear\n", d->va, (unsigned long)hr);
        free(seed);
        seed = NULL;
        d->guest_conflict = 1;
        hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex);
    }
    if (FAILED(hr))
        goto failed;
    if (seed) {
        d->initialized = 3;
        seeded = 1;
        free(seed);
        seed = NULL;
    }
    memset(&dv, 0, sizeof dv);
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = scale_test_failure("depth-view") ? E_OUTOFMEMORY :
         ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, &dv, &d->dsv);
    if (FAILED(hr))
        goto failed;
    if (depth_srv_capable()) {
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        hr = scale_test_failure("depth-srv") ? E_OUTOFMEMORY :
             ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)d->tex, &sv, &d->srv);
        if (FAILED(hr))
            goto failed;
    }
#ifdef BRUTEFORCE_D3D11_TESTING
    if (seeded && scale_test_failure("depth-seed-race"))
        *(uint32_t *)GUEST(d->va) ^= 1u;
#endif
    if (seeded && probe_depth_guest(d) != 0) {
        hr = E_FAIL;
        goto failed;
    }
    if (seeded)
        s_st.depth_seeds++;
    if (!create_depth_msaa(d)) {
        hr = E_FAIL;
        goto failed;
    }
    return 1;

failed:
    gpu_d3d11_check_device(s_dev, hr, "creating depth resources");
    free(seed);
    fprintf(stderr, "[GPU-D3D11] depth surface creation failed 0x%08lX\n", (unsigned long)hr);
    release_depth(d);
    memset(d, 0, sizeof *d);
    return 0;
}

static void depth_reject(const char *reason, uint32_t va, uint32_t other)
{
    static unsigned reports;
    s_st.depth_rejected++;
    if (reports < 32) {
        ++reports;
        fprintf(stderr, "[GPU-D3D11] depth ownership rejected: %s (%08X, %08X)%s\n",
                reason, va, other, reports == 32 ? " (further reports suppressed)" : "");
    }
}

static void retire_depth(Depth *d)
{
    if (s_cur_depth == d) {
        if (s_cur)
            bind_targets(s_cur, NULL);
        else
            s_cur_depth = NULL;
    }
    release_depth(d);
    memset(d, 0, sizeof *d);
}

static int depth_reseed(Depth *d)
{
    Depth replacement;
    int bound = s_cur && s_cur_depth == d;
    memset(&replacement, 0, sizeof replacement);
    replacement.va = d->va;
    replacement.w = d->w;
    replacement.h = d->h;
    replacement.pitch = d->pitch;
    replacement.format = d->format;
    replacement.surface_type = d->surface_type;
    replacement.scale_epoch = d->scale_epoch;
    replacement.recreated = 1;
    replacement.revision = d->revision + 1;
    replacement.identity = d->identity;
    memcpy(replacement.logical_w, d->logical_w, sizeof replacement.logical_w);
    memcpy(replacement.logical_h, d->logical_h, sizeof replacement.logical_h);
    if (!create_depth_resource(&replacement, 0))
        return 0;
    if (replacement.initialized != 3 || replacement.guest_conflict) {
        release_depth(&replacement);
        return 0;
    }
    release_depth(d);
    *d = replacement;
    if (bound)
        bind_targets(s_cur, d);
    return 1;
}

static int sync_depth_guest(Depth *d)
{
    int change;
    if (!depth_views_enabled() || gpu_scale_requested() ||
        d->format != 2 || d->surface_type != 1)
        return 1;
    change = probe_depth_guest(d);
    if (change == 2 || (!d->gpu_dirty && change >= 0 &&
                        (change != 0 || d->guest_conflict))) {
        if (depth_reseed(d))
            return 1;
    } else if (change == 0 && !d->guest_conflict) {
        return 1;
    }
    d->guest_conflict = 1;
    depth_reject(change < 0 ? "guest depth range unreadable or unverifiable" :
                 "guest bytes or identity conflict with GPU depth", d->va, 0);
    return 0;
}

/* A full depth+stencil clear can discard the old GPU planes, but only after
 * establishing a fresh, checked baseline for subsequent guest-write checks. */
static int rebaseline_depth(Depth *d)
{
    Depth snapshot = *d;
    snapshot.guest_shadow = NULL;
    snapshot.guest_shadow_bytes = 0;
    if (!capture_depth_guest(&snapshot))
        goto failed;
#ifdef BRUTEFORCE_D3D11_TESTING
    if (scale_test_failure("depth-baseline-race"))
        *(uint32_t *)GUEST(snapshot.va) ^= 1u;
#endif
    if (probe_depth_guest(&snapshot) != 0)
        goto failed;
    free(d->guest_shadow);
    d->guest_shadow = snapshot.guest_shadow;
    d->guest_shadow_bytes = snapshot.guest_shadow_bytes;
    d->identity = snapshot.identity;
    d->guest_conflict = 0;
    return 1;

failed:
    free(snapshot.guest_shadow);
    depth_reject("cannot establish guest baseline for full clear", d->va, 0);
    return 0;
}

static int physical_range(uint64_t start, uint64_t span, uint64_t *physical);

static int guest_ranges_overlap(uint32_t a, uint64_t aspan, uint32_t b, uint64_t bspan)
{
    uint64_t ap = 0, bp = 0;
    int av, bv;
    if (!aspan || !bspan || (uint64_t)a + aspan > 0x100000000ull ||
        (uint64_t)b + bspan > 0x100000000ull)
        return 1;
    av = physical_range(a, aspan, &ap);
    bv = physical_range(b, bspan, &bp);
    /* A failed physical_range is unknown, not a hit. Treating it as overlap
     * made every texture collide with an unmapped or overflowed surface. */
    return ((uint64_t)a < (uint64_t)b + bspan && (uint64_t)b < (uint64_t)a + aspan) ||
           (av > 0 && bv > 0 && ap < bp + bspan && bp < ap + aspan);
}

static int depth_known_stale(const Depth *d)
{
    uint32_t remaining;
    uint64_t allocation, layout;
    if (!d->identity.layout)
        return 0;
    layout = xbox_MemoryLayoutGeneration();
    allocation = guest_allocation_identity(d->va, &remaining);
    return (layout && layout != d->identity.layout) ||
           (d->identity.allocation && allocation && allocation != d->identity.allocation);
}

static int tracked_allocation_lost(uint32_t va, const GuestSpanIdentity *identity)
{
    uint32_t remaining;
    return identity->allocation && identity->layout == xbox_MemoryLayoutGeneration() &&
           !guest_allocation_identity(va, &remaining);
}

static uint64_t depth_owner_span(const Depth *d)
{
    uint32_t height = d->logical_h[0] > d->logical_h[1]
                    ? d->logical_h[0] : d->logical_h[1];
    return (uint64_t)d->pitch * height;
}

static int depth_owners_compatible(uint32_t va, uint64_t span, Depth *same,
                                   const Surf *target)
{
    int i;
    for (i = 0; i < s_ndepth; ++i) {
        Depth *other = &s_depth[i];
        if (!other->tex || other == same)
            continue;
        if (depth_known_stale(other)) {
            retire_depth(other);
            continue;
        }
        if (!guest_ranges_overlap(va, span, other->va, depth_owner_span(other)))
            continue;
        if (other->gpu_dirty || other->guest_conflict ||
            tracked_allocation_lost(other->va, &other->identity)) {
            depth_reject("overlapping GPU depth owners", va, other->va);
            return 0;
        }
        retire_depth(other);
    }
    for (i = 0; i < s_nsurf; ++i) {
        Surf *other = &s_surf[i];
        size_t other_span;
        int current;
        if (!other->tex)
            continue;
        current = surf_identity_current(other);
        if (!current && !tracked_allocation_lost(other->va, &other->identity))
            continue;
        if (!surface_guest_span(other, &other_span) ||
            guest_ranges_overlap(va, span, other->va, other_span)) {
            if (!current || other == target || other->gpu_dirty || other->alias_uncertain) {
                depth_reject("overlapping color and depth owners", va, other->va);
                return 0;
            }
            if (s_cur == other) {
                ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
                s_cur = NULL;
                s_cur_depth = NULL;
            }
            if (s_last_presented == other)
                s_last_presented = NULL;
            release_surf(other);
            release_surf_guest(other);
        }
    }
    return 1;
}

static int color_depth_compatible(const Nv2aSurface *in)
{
    uint32_t bpp = in->bytes_per_pixel ? in->bytes_per_pixel : 4;
    uint32_t width = in->pitch && in->pitch / bpp >= in->width
                   ? in->pitch / bpp : in->width;
    uint32_t height = in->height;
    uint64_t row, span;
    int i;
    for (i = 0; i < s_nsurf; ++i) {
        Surf *existing = &s_surf[i];
        if (existing->tex && existing->va == in->color_va && surf_identity_current(existing)) {
            if (width < existing->w) width = existing->w;
            if (height < existing->h) height = existing->h;
            break;
        }
    }
    row = (uint64_t)width * bpp;
    span = height && in->pitch
         ? (uint64_t)(height - 1) * in->pitch +
           (row < in->pitch ? row : in->pitch) : 0;
    for (i = 0; i < s_ndepth; ++i) {
        Depth *other = &s_depth[i];
        if (!other->tex)
            continue;
        if (depth_known_stale(other)) {
            retire_depth(other);
            continue;
        }
        if (!guest_ranges_overlap(in->color_va, span, other->va, depth_owner_span(other)))
            continue;
        if (other->gpu_dirty || other->guest_conflict ||
            tracked_allocation_lost(other->va, &other->identity)) {
            depth_reject("color overlaps GPU depth owner", in->color_va, other->va);
            return 0;
        }
        retire_depth(other);
    }
    return 1;
}

static Depth *get_depth(uint32_t va, const Surf *surf)
{
    uint32_t w = surf->pw, h = surf->ph;
    uint32_t pitch = nv2a_backend_reg(0x020C) >> 16;
    uint32_t surface_format = nv2a_backend_reg(0x0208);
    uint32_t format = (surface_format >> 4) & 0xF;
    uint32_t surface_type = (surface_format >> 8) & 0xF;
    Depth *d = NULL, *empty = NULL, replacement;
    uint32_t guest_height = surf->h;
    int existing, same_layout, unsafe_recreation, i;

    if (!va)
        return NULL;
    for (i = 0; i < s_ndepth; i++) {
        if (s_depth[i].tex && s_depth[i].va == va) { d = &s_depth[i]; break; }
        if (!s_depth[i].tex && !empty) empty = &s_depth[i];
    }
    if (d && depth_views_enabled() && depth_known_stale(d)) {
        retire_depth(d);
        empty = d;
        d = NULL;
    }
    existing = d != NULL;
    same_layout = d && (!depth_views_enabled() ||
                        (d->pitch == pitch && d->format == format &&
                         d->surface_type == surface_type));
    if (d) {
        if (guest_height < d->logical_h[0]) guest_height = d->logical_h[0];
        if (guest_height < d->logical_h[1]) guest_height = d->logical_h[1];
    }
    if (depth_views_enabled() &&
        !depth_owners_compatible(va, (uint64_t)pitch * guest_height, d, surf))
        return NULL;
    if (d && d->scale_epoch == gpu_scale_epoch() && d->w >= w && d->h >= h && same_layout &&
        d->msaa_count == msaa_depth_samples()) {
        depth_record_use(d, surf);
        return d;
    }
    if (!d && !empty && s_ndepth == MAX_SURF) {
        fprintf(stderr, "[GPU-D3D11] depth surface limit reached for %08X\n", va);
        return NULL;
    }
    memset(&replacement, 0, sizeof replacement);
    if (same_layout) {
        memcpy(replacement.logical_w, d->logical_w, sizeof replacement.logical_w);
        memcpy(replacement.logical_h, d->logical_h, sizeof replacement.logical_h);
        if (d->scale_epoch == gpu_scale_epoch()) {
            if (w < d->w) w = d->w;
            if (h < d->h) h = d->h;
        }
    }
    replacement.va = va; replacement.w = w; replacement.h = h; replacement.pitch = pitch;
    replacement.format = format; replacement.surface_type = surface_type;
    replacement.recreated = existing;
    replacement.scale_epoch = gpu_scale_epoch();
    unsafe_recreation = depth_views_enabled() && d && (d->gpu_dirty || d->guest_conflict);
    if (unsafe_recreation &&
        (gpu_scale_requested() || format != 2 || surface_type != 1)) {
        depth_reject("dirty depth transition to unsupported layout", va, va);
        return NULL;
    }
    if (d) {
        replacement.revision = d->revision + 1;
        replacement.identity = d->identity;
    }
    depth_record_use(&replacement, surf);
    if (!create_depth_resource(&replacement, depth_views_enabled() ? unsafe_recreation : existing)) {
        if (d)
            fprintf(stderr, "[GPU-D3D11] retaining prior depth resources for %08X\n", va);
        return NULL;
    }
    if (unsafe_recreation) {
        replacement.gpu_dirty = 1;
        replacement.guest_conflict = 1;
    }
    if (d) {
        if (s_cur_depth == d) {
            if (s_cur)
                bind_targets(s_cur, NULL);
            else
                s_cur_depth = NULL;
        }
        release_depth(d);
    } else {
        d = empty ? empty : &s_depth[s_ndepth++];
    }
    *d = replacement;
    return d;
}

/* The RAM mirrors, contiguous window and tiled aperture may name the same
 * bytes. A span crossing one of their backing boundaries is indeterminate. */
static int physical_range(uint64_t start, uint64_t span, uint64_t *physical)
{
    uint64_t mapped = xbox_GetMappedSize(), end = start + span;
    uint64_t tiled_size = mapped < 0x0D000000u ? mapped : 0x0D000000u;
    uint64_t relative;

    if (!mapped || end > 0x100000000ull)
        return -1;
    if (start >= XBOX_CONTIG_BASE && start < (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE) {
        relative = start - XBOX_CONTIG_BASE;
        if (span > XBOX_CONTIG_SIZE - relative || relative + span > mapped)
            return -1;
    } else if (start >= XBOX_TILED_BASE && start < (uint64_t)XBOX_TILED_BASE + tiled_size) {
        relative = start - XBOX_TILED_BASE;
        if (span > tiled_size - relative)
            return -1;
    } else if (start < mapped * (1ull + XBOX_NUM_MIRRORS) && start < XBOX_CONTIG_BASE) {
        relative = start % mapped;
        if (span > mapped - relative)
            return -1;
    } else {
        if ((start < XBOX_CONTIG_BASE && end > XBOX_CONTIG_BASE) ||
            (start < XBOX_TILED_BASE && end > XBOX_TILED_BASE))
            return -1;
        return 0;
    }
    *physical = relative;
    return 1;
}

static Depth *overlapping_depth(const Nv2aTexture *t, int cube, int *ambiguous)
{
    GpuTextureLayout layout;
    uint64_t start = t->offset, end = start;
    uint64_t physical_start = 0;
    Depth *match = NULL;
    int i, physical_valid = 0;
    int valid = gpu_texture_layout(t, cube ? 6 : 1, s_tex_mips, &layout);

    *ambiguous = !valid;
    if (valid) {
        end += layout.span;
        physical_valid = physical_range(start, layout.span, &physical_start);
        if (physical_valid < 0)
            *ambiguous = 1;
    }
    for (i = 0; i < s_ndepth; i++) {
        Depth *d = &s_depth[i];
        uint32_t guest_height = d->logical_h[0] > d->logical_h[1]
                              ? d->logical_h[0] : d->logical_h[1];
        uint64_t depth_span = (uint64_t)d->pitch * guest_height;
        uint64_t depth_end = (uint64_t)d->va + depth_span, depth_physical = 0;
        int depth_physical_valid, overlaps;
        if (!d->tex)
            continue;
        if (depth_views_enabled() && depth_known_stale(d)) {
            retire_depth(d);
            continue;
        }
        depth_physical_valid = physical_range(d->va, depth_span, &depth_physical);
        overlaps = !valid || !depth_span || physical_valid < 0 || depth_physical_valid < 0 ||
                   (start < depth_end && d->va < end) ||
                   (physical_valid > 0 && depth_physical_valid > 0 &&
                    physical_start < depth_physical + depth_span &&
                    depth_physical < physical_start + layout.span);
        if (overlaps) {
            if (!depth_span || physical_valid < 0 || depth_physical_valid < 0)
                *ambiguous = 1;
            if (match)
                *ambiguous = 1;
            else
                match = d;
        }
    }
    return match;
}

#ifdef BRUTEFORCE_D3D11_TESTING
int bruteforce_d3d11_test_depth_overlap(const Nv2aTexture *texture, int cube, int *ambiguous)
{
    return overlapping_depth(texture, cube, ambiguous) != NULL;
}
#endif

static int depth_guest_size_matches(const Depth *d, const Nv2aTexture *t)
{
    unsigned x, y;
    if (t->width == d->w && t->height == d->h)
        return 1;
    /* d->w/h are the physical (scaled) size. A guest-sized depth texture
     * still samples correctly with 0-1 UVs from the higher-resolution SRV. */
    for (x = 0; x < 2; ++x)
        for (y = 0; y < 2; ++y)
            if (d->logical_w[x] == t->width && d->logical_h[y] == t->height)
                return 1;
    return 0;
}

static Depth *check_depth_texture(const Nv2aTexture *t, int cube, unsigned levels,
                                  unsigned initialized)
{
    static int warned;
    Depth *d = NULL, *overlap;
    int ambiguous;
    int i;

    for (i = 0; i < s_ndepth; i++)
        if (s_depth[i].tex && s_depth[i].va == t->offset) { d = &s_depth[i]; break; }
    overlap = overlapping_depth(t, cube, &ambiguous);
    if (!d || !d->srv || cube || levels > 1 || ambiguous || overlap != d ||
        d->format != 2 || d->surface_type != 1 ||
        !depth_guest_size_matches(d, t) || t->pitch != d->pitch ||
        t->pitch < t->width * 4) {
        s_st.depth_rejected++;
        if (!warned++) {
            fprintf(stderr, "[GPU-D3D11] depth texture alias rejected: va %08X %ux%u"
                            " pitch %u levels %u cube %d; surface %s %ux%u pitch %u"
                            " format %u type %u; ambiguous overlap %d, scaled %d\n",
                    t->offset, t->width, t->height, t->pitch, levels, cube,
                    d ? "tracked" : "missing", d ? d->w : 0, d ? d->h : 0,
                    d ? d->pitch : 0, d ? d->format : 0, d ? d->surface_type : 0,
                    ambiguous, gpu_scale_requested());
        }
        return NULL;
    }
    if (!sync_depth_guest(d))
        return NULL;
    if ((d->initialized & initialized) != initialized) {
        static int warned_uninitialized;
        s_st.depth_rejected++;
        if (!warned_uninitialized++)
            fprintf(stderr, "[GPU-D3D11] depth texture alias rejected: surface %08X"
                            " has not been initialized from guest memory or a GPU depth clear"
                            " (required planes %u, initialized %u; %ux%u pitch %u,"
                            " type %u, recreated %u)\n",
                    d->va, initialized, d->initialized, d->w, d->h, d->pitch,
                    d->surface_type, d->recreated);
        return NULL;
    }
    return d;
}

static void depth_resolve_raw(Depth *d)
{
    DXGI_FORMAT fmt;
    if (!d || !d->msaa_dirty || !d->msaa_tex || !d->tex || !s_ctx)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    fmt = depth_srv_capable() ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                : DXGI_FORMAT_D24_UNORM_S8_UINT;
    ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)d->tex, 0,
        (ID3D11Resource *)d->msaa_tex, 0, fmt);
    d->msaa_dirty = 0;
}

static ID3D11ShaderResourceView *depth_view(Depth *d)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    HRESULT hr;

    s_st.depth_samples++;
    if (d->msaa_dirty) {
        s_st.msaa_resolve_samples++;
#ifdef RECOMP_DEBUG_TOOLS
        debug_note("msaa_resolve", d == s_cur_depth ? "depth_bound" : "depth_unbound",
                   d->va, d->w, d->h, 0, d->msaa_count);
#endif
        depth_resolve_raw(d);
        restore_targets();
    }
    if (d != s_cur_depth)
        return d->srv;
    if (!d->snapshot) {
        ID3D11Texture2D_GetDesc(d->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->snapshot);
        if (FAILED(hr))
            goto failed;
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)d->snapshot,
                                                   &sv, &d->snapshot_srv);
        if (FAILED(hr))
            goto failed;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)d->snapshot,
                                     (ID3D11Resource *)d->tex);
    restore_targets();
    s_st.depth_snapshots++;
    return d->snapshot_srv;

failed:
    gpu_d3d11_check_device(s_dev, hr, "creating depth snapshot");
    fprintf(stderr, "[GPU-D3D11] depth snapshot creation failed 0x%08lX\n", (unsigned long)hr);
    if (d->snapshot_srv) ID3D11ShaderResourceView_Release(d->snapshot_srv);
    if (d->snapshot) ID3D11Texture2D_Release(d->snapshot);
    d->snapshot = NULL;
    d->snapshot_srv = NULL;
    return NULL;
}

static ID3D11ShaderResourceView *get_depth_texture(const Nv2aTexture *t, int cube,
                                                    unsigned levels)
{
    Depth *d = check_depth_texture(t, cube, levels, 1);
    return d ? depth_view(d) : NULL;
}

static int required_depth_alias(const Nv2aTexture *t, int cube)
{
    int ambiguous;
    return (s_depth_texture && t->color == 0x2E) ||
           (depth_views_enabled() && overlapping_depth(t, cube, &ambiguous));
}

static const char s_depth_packed_hlsl[] =
    "Texture2D<float> depth_tex : register(t0);\n"
    "Texture2D<uint2> stencil_tex : register(t1);\n"
    "RWTexture2D<float4> packed_tex : register(u0);\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint w, h; packed_tex.GetDimensions(w, h);\n"
    "  if (id.x >= w || id.y >= h) return;\n"
    "  int3 p = int3(id.xy, 0);\n"
    "  uint z = (uint)round(saturate(depth_tex.Load(p)) * 16777215.0f);\n"
    "  uint s = stencil_tex.Load(p).g;\n"
    "  packed_tex[id.xy] = float4((z >> 16) & 255u, (z >> 8) & 255u,"
    " z & 255u, s) / 255.0f;\n"
    "}\n";

/* Layout 1: the guest's Z24S8 word ((z << 8) | s, little-endian) sampled as
 * an A8R8G8B8 colour texture, which the back end views as B8G8R8A8: r = z
 * bits 8-15, g = z bits 0-7, b = stencil, a = z bits 16-23. Halo 2's patchy
 * fog reads the depth buffer this way. */
static const char s_depth_argb_hlsl[] =
    "Texture2D<float> depth_tex : register(t0);\n"
    "Texture2D<uint2> stencil_tex : register(t1);\n"
    "RWTexture2D<float4> packed_tex : register(u0);\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint w, h; packed_tex.GetDimensions(w, h);\n"
    "  if (id.x >= w || id.y >= h) return;\n"
    "  int3 p = int3(id.xy, 0);\n"
    "  uint z = (uint)round(saturate(depth_tex.Load(p)) * 16777215.0f);\n"
    "  uint s = stencil_tex.Load(p).g;\n"
    "  packed_tex[id.xy] = float4((z >> 8) & 255u, z & 255u, s,"
    " (z >> 16) & 255u) / 255.0f;\n"
    "}\n";

static ID3D11ComputeShader *s_depth_packed_cs, *s_depth_argb_cs;
static int s_depth_packed_failed, s_depth_argb_failed;

static ID3D11ComputeShader *packed_depth_cs(int layout)
{
    ID3DBlob *code = NULL;
    HRESULT hr;
    ID3D11ComputeShader **cs = layout ? &s_depth_argb_cs : &s_depth_packed_cs;
    int *failed = layout ? &s_depth_argb_failed : &s_depth_packed_failed;
    const char *src = layout ? s_depth_argb_hlsl : s_depth_packed_hlsl;
    size_t n = layout ? sizeof s_depth_argb_hlsl - 1 : sizeof s_depth_packed_hlsl - 1;

    if (*cs || *failed)
        return *cs;
    if (!compile(src, n, layout ? "depth_argb" : "depth_packed", "main", "cs_5_0", &code)) {
        *failed = 1;
        return NULL;
    }
    hr = ID3D11Device_CreateComputeShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                           ID3D10Blob_GetBufferSize(code), NULL, cs);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) {
        gpu_d3d11_check_device(s_dev, hr, "creating packed-depth shader");
        fprintf(stderr, "[GPU-D3D11] packed depth shader creation failed 0x%08lX\n",
                (unsigned long)hr);
        *failed = 1;
        *cs = NULL;
    }
    return *cs;
}

static int prepare_packed_depth(Depth *d, int layout)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (!packed_depth_cs(layout))
        return 0;
    if (d->packed)
        return d->packed_srv && d->packed_uav;
    memset(&td, 0, sizeof td);
    td.Width = d->w; td.Height = d->h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->packed);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)d->packed,
                                                   NULL, &d->packed_srv);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateUnorderedAccessView(s_dev, (ID3D11Resource *)d->packed,
                                                    NULL, &d->packed_uav);
    if (FAILED(hr)) {
        gpu_d3d11_check_device(s_dev, hr, "creating packed-depth texture");
        fprintf(stderr, "[GPU-D3D11] packed depth texture creation failed 0x%08lX\n",
                (unsigned long)hr);
        if (d->packed_uav) ID3D11UnorderedAccessView_Release(d->packed_uav);
        if (d->packed_srv) ID3D11ShaderResourceView_Release(d->packed_srv);
        if (d->packed) ID3D11Texture2D_Release(d->packed);
        d->packed_uav = NULL; d->packed_srv = NULL; d->packed = NULL;
        return 0;
    }
    return 1;
}

static ID3D11ShaderResourceView *get_packed_depth_texture(const Nv2aTexture *t, int cube,
                                                           unsigned levels, int layout)
{
    Depth *d = check_depth_texture(t, cube, levels, layout ? 1 : 3);
    ID3D11ShaderResourceView *views[2], *none[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView **stencil;
    ID3D11UnorderedAccessView *no_uav = NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    ID3D11Texture2D *source;
    HRESULT hr;

    if (!d || !prepare_packed_depth(d, layout))
        return NULL;
    s_st.stencil_samples++;
    if (d->packed_valid && d->packed_revision == d->revision && d->packed_layout == layout)
        return d->packed_srv;
    views[0] = depth_view(d);
    if (!views[0])
        return NULL;
    source = d == s_cur_depth ? d->snapshot : d->tex;
    stencil = d == s_cur_depth ? &d->snapshot_stencil_srv : &d->stencil_srv;
    if (!*stencil) {
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)source,
                                                   &sv, stencil);
        if (FAILED(hr)) {
            gpu_d3d11_check_device(s_dev, hr, "creating stencil view");
            fprintf(stderr, "[GPU-D3D11] stencil view creation failed 0x%08lX\n",
                    (unsigned long)hr);
            return NULL;
        }
    }
    views[1] = *stencil;
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, none);
    ID3D11DeviceContext_CSSetShader(s_ctx, packed_depth_cs(layout), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 2, views);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &d->packed_uav, NULL);
    ID3D11DeviceContext_Dispatch(s_ctx, (d->w + 7) / 8, (d->h + 7) / 8, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &no_uav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 2, none);
    ID3D11DeviceContext_CSSetShader(s_ctx, NULL, NULL, 0);
    d->packed_revision = d->revision;
    d->packed_layout = layout;
    d->packed_valid = 1;
    s_st.stencil_conversions++;
    return d->packed_srv;
}

static void reject_depth_mode(uint32_t mode, int cube, int base)
{
    static uint32_t warned_base, warned_extended;
    static int warned_cube;
    uint32_t *warned = base ? &warned_base : &warned_extended;
    s_st.depth_rejected++;
    if (cube) {
        if (!warned_cube++)
            fprintf(stderr, "[GPU-D3D11] depth cube textures are unsupported\n");
    } else if (!(*warned & (1u << mode))) {
        *warned |= 1u << mode;
        fprintf(stderr, "[GPU-D3D11] depth texture stage mode %u is unsupported on %s draws\n",
                mode, base ? "base" : "extended");
    }
}

static int shadow_alias_lit(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_SHADOW_ALIAS"); on = e && strcmp(e, "lit") == 0; }
    return on;
}

static int rtlog_on(void)
{
    static long start = -1;
    if (start < 0) {
        const char *e = getenv("RECOMP_RTLOG_GEN");
        start = e && *e ? atol(e) : 0;
    }
    return (start && s_gen >= (uint32_t)start && s_gen < (uint32_t)start + 2) ||
           (s_probe_armed && s_gen >= s_probe_armed && s_gen < s_probe_armed + 2);
}

/* The bound depth surface took part in the draw just counted in s_wserial. */
static void note_depth_use(void)
{
    if (s_cur_depth) {
        s_cur_depth->zserial = s_wserial;
        s_cur_depth->bound_gen = s_gen;
    }
}

/* A colour texture over depth surface d samples the depth word only while
 * the memory is still the depth buffer: d was drawn with this frame or the
 * last, and no colour target at the same address was written after that.
 * Halo 2's loading screen reuses the zeta memory (834A0000) as colour; read
 * as packed depth it lost its blue channel (yellow scanner bar). */
static int depth_still_current(const Depth *d)
{
    int i;
    if (!d->bound_gen || d->bound_gen + 1 < s_gen)
        return 0;
    for (i = 0; i < s_nsurf; i++)
        if (s_surf[i].tex && s_surf[i].va == d->va && s_surf[i].wserial > d->zserial)
            return 0;
    return 1;
}

static void bind_targets(Surf *s, Depth *d)
{
    if (rtlog_on() && s != s_cur)
        fprintf(stderr, "[RTLOG] g%u bind %08X %ux%u phys %ux%u aa %ux%u pitch %u bpp %u dirty %d unc %d"
                " depth %08X %ux%u\n", s_gen,
                s->va, s->w, s->h, s->pw, s->ph, s->aa_sx, s->aa_sy, s->pitch, s->bpp, s->gpu_dirty,
                s->alias_uncertain, d ? d->va : 0, d ? d->w : 0, d ? d->h : 0);
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };

    /* A surface about to be drawn into must not stay bound as a texture. */
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, none);
    if (s->msaa_rtv && (!d || (d->msaa_dsv && d->msaa_count == s->msaa_count))) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s->msaa_rtv,
                                               d ? d->msaa_dsv : NULL);
        s->msaa_dirty = 1;
        if (d)
            d->msaa_dirty = 1;
    } else {
        if (s->msaa_dirty)
            surf_resolve_raw(s);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s->rtv, d ? d->dsv : NULL);
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)s->pw; vp.Height = (float)s->ph;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    s_cur = s;
    s_cur_depth = d;
}

static LONG scale_edge(uint32_t value, uint32_t logical, uint32_t physical)
{
    return logical ? (LONG)(((uint64_t)value * physical + logical / 2) / logical) : 0;
}

/* The surface clip, as the CPU rasteriser bounds its triangles. */
static void apply_scissor(const Nv2aSurface *s)
{
    D3D11_RECT r;
    uint32_t right = s->width < s_cur->w ? s->width : s_cur->w;
    uint32_t bottom = s->height < s_cur->h ? s->height : s_cur->h;
    r.left = 0; r.top = 0;
    r.right = scale_edge(right, s_cur->w, s_cur->pw);
    r.bottom = scale_edge(bottom, s_cur->h, s_cur->ph);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
}

static void surface_clear_rect(const Nv2aSurface *s, const Surf *surf, D3D11_RECT *rect)
{
    uint32_t bounds[4];
    if (nv2a_backend_clear_rect(bounds)) {
        uint32_t sx = s->aa_sx > 1 ? 2 : 1, sy = s->aa_sy > 1 ? 2 : 1;
        uint32_t x0 = bounds[0] * sx, y0 = bounds[1] * sy;
        uint32_t x1 = bounds[2] * sx, y1 = bounds[3] * sy;
        x0 = x0 < surf->w ? x0 : surf->w;
        y0 = y0 < surf->h ? y0 : surf->h;
        x1 = x1 < surf->w ? x1 : surf->w;
        y1 = y1 < surf->h ? y1 : surf->h;
        rect->left = scale_edge(x0, surf->w, surf->pw);
        rect->top = scale_edge(y0, surf->h, surf->ph);
        rect->right = scale_edge(x1, surf->w, surf->pw);
        rect->bottom = scale_edge(y1, surf->h, surf->ph);
    } else {
        rect->left = rect->top = 0;
        rect->right = (LONG)surf->pw;
        rect->bottom = (LONG)surf->ph;
    }
}

static void pump_live_settings(void);

static int begin(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t clear_flags)
{
    Surf *surf;
    Depth *d;

    pump_live_settings();
    if (!ensure_device() || !s || !s->color_va)
        return 0;
    gpu_work_begin();
    if (depth_views_enabled() && !color_depth_compatible(s))
        return 0;
    surf = get_surf(s);
    if (!surf)
        return 0;
    d = rs && (!depth_views_enabled() || !clear_flags || (clear_flags & 3))
      ? get_depth(rs->zeta_va, surf) : NULL;
    if (rs && rs->zeta_va && !d &&
        (!depth_views_enabled() || !clear_flags || (clear_flags & 3)))
        return 0;
    if (d && !sync_depth_guest(d)) {
        D3D11_RECT rect;
        surface_clear_rect(s, surf, &rect);
        if ((clear_flags & 3) != 3 || rect.left != 0 || rect.top != 0 ||
            (uint32_t)rect.right != d->w || (uint32_t)rect.bottom != d->h ||
            !rebaseline_depth(d))
            return 0;
    }
    if (surf != s_cur || d != s_cur_depth) {
        int counting = surf != s_cur ? query_suspend_clear() : 0;
        bind_targets(surf, d);
        query_resume_clear(counting);
    }
    apply_scissor(s);
    return 1;
}

static int ensure_guest_texture(Surf *s)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (s->guest_tex && s->guest_srv)
        return 1;
    memset(&td, 0, sizeof td);
    td.Width = s->w;
    td.Height = s->h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = surf_format(s->bpp);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->guest_tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->guest_tex,
                                                   NULL, &s->guest_srv);
    if (FAILED(hr)) {
        gpu_d3d11_check_device(s_dev, hr, "creating guest color upload texture");
        fprintf(stderr, "[GPU-D3D11] guest color upload texture for %08X failed 0x%08lX\n",
                s->va, (unsigned long)hr);
        if (s->guest_srv) ID3D11ShaderResourceView_Release(s->guest_srv);
        if (s->guest_tex) ID3D11Texture2D_Release(s->guest_tex);
        s->guest_srv = NULL;
        s->guest_tex = NULL;
        return 0;
    }
    return 1;
}

static int upload_guest_surface(Surf *s)
{
    Surf *restore = s_cur;
    Depth *restore_depth = s_cur_depth;
    int counting, ok;

    if ((uint64_t)s->w * s->bpp > s->pitch)
        return 0;
    if (rtlog_on())
        fprintf(stderr, "[RTLOG] g%u guest upload %08X %ux%u\n", s_gen, s->va, s->w, s->h);
    if (s->pw == s->w && s->ph == s->h) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s->tex, 0, NULL,
                                             s_surface_probe, s->pitch, 0);
        return 1;
    }
    if (!ensure_guest_texture(s))
        return 0;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s->guest_tex, 0, NULL,
                                         s_surface_probe, s->pitch, 0);
    counting = query_suspend_clear();
    ok = d3d_scale_blit(s->guest_srv, s->w, s->h, s->rtv, s->pw, s->ph,
                        GPU_SCALE_BLIT_BILINEAR);
    if (!ok) {
        fprintf(stderr, "[GPU-D3D11] guest color upload for %08X failed\n", s->va);
        if (restore)
            bind_targets(restore, restore_depth);
        else
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        query_resume_clear(counting);
        return 0;
    }
    if (restore)
        bind_targets(restore, restore_depth);
    else
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    query_resume_clear(counting);
    return 1;
}

static int sync_guest_surface(Surf *s, int force)
{
    static int warned_conflict;
    int changed;

    if (!force && s->guest_check_gen == s_gen)
        return 1;
    changed = guest_surface_changed(s);
    if (changed < 0)
        return 0;
    s->guest_check_gen = s_gen;
    if (!changed)
        return 1;
    if (s->gpu_dirty) {
        s_st.guest_conflicts++;
        if (!warned_conflict++)
            fprintf(stderr, "[GPU-D3D11] guest writes raced GPU color work on surface %08X;"
                            " preserving the GPU frame\n", s->va);
        return 2;
    }
    if (!upload_guest_surface(s))
        return 0;
    shadow_from_probe(s);
#ifdef BRUTEFORCE_D3D11_TESTING
    if (scale_test_failure("guest-change-upload"))
        *(uint32_t *)GUEST(s->va) ^= 1u;
#endif
    s->guest_exact = 0;
    s_st.guest_uploads++;
    changed = guest_surface_changed(s);
    if (changed) {
        s->gpu_dirty = 1;
        s->guest_check_gen = 0;
        return changed < 0 ? 0 : 2;
    }
    return 1;
}

static uint32_t guest_clear_value(const Surf *s, uint32_t argb)
{
    if (s->bpp == 2)
        return ((argb >> 8) & 0xF800u) | ((argb >> 5) & 0x07E0u) |
               ((argb >> 3) & 0x001Fu);
    return argb;
}

static void shadow_from_clear(Surf *s, uint32_t argb)
{
    uint32_t value = guest_clear_value(s, argb);
    size_t i, count = (size_t)s->w * s->h;
    /* typed stores: a per-pixel memcpy of a run-time size was a call per pixel */
    if (s->bpp == 4) {
        uint32_t *p = (uint32_t *)s->guest_shadow;
        for (i = 0; i < count; ++i) p[i] = value;
    } else if (s->bpp == 2) {
        uint16_t *p = (uint16_t *)s->guest_shadow;
        for (i = 0; i < count; ++i) p[i] = (uint16_t)value;
    } else {
        for (i = 0; i < count; ++i)
            memcpy(s->guest_shadow + i * s->bpp, &value, s->bpp);
    }
}

static int mirror_full_color_clear(Surf *s, uint32_t argb)
{
    int changed;

    if ((s->bpp != 2 && s->bpp != 4) ||
        (uint64_t)s->w * s->bpp > s->pitch || !ensure_guest_shadow(s))
        return 0;
    shadow_from_clear(s, argb);
    if (!checked_write_guest_surface(s, s->guest_shadow, surf_guest_row(s)))
        return 0;
#ifdef BRUTEFORCE_D3D11_TESTING
    if (scale_test_failure("guest-change-mirror"))
        *(uint32_t *)GUEST(s->va) ^= 1u;
#endif
    if (surf_fast_enabled()) {
        changed = 0;
        s->guest_check_gen = s_gen;
    } else {
        changed = guest_surface_changed(s);
        if (changed < 0)
            return 0;
        s->guest_check_gen = 0;
    }
    s->gpu_dirty = changed != 0;
    s->guest_exact = !changed && s_skip_clean_readback && s->bpp == 4 && s->pitch >= s->w * 4 &&
                     s->pw == s->w && s->ph == s->h;
    return 1;
}

/* ---- textures ------------------------------------------------------------ */

#define MAX_TEX 2048
#define TEX_INDEX_BUCKETS 4096
#define MAX_TEX_SNAPSHOT_BYTES (128u << 20)
typedef struct {
    uint32_t offset, w, h, pitch, color, levels, uploaded_levels, palette, palette_entries;
    uint64_t hash, sparse, palette_hash;
    GuestSpanIdentity source_identity, palette_identity;
    uint32_t gen, stable, last_use;
    uint32_t vgen, vepoch;          /* frame and write epoch of the last full compare */
    const uint8_t *vsnap;
    int index_next;
    int index_bucket;          /* bucket used at insert, or -1 */
    uint8_t cube, unsupported, indexed;
    uint8_t hires, lod_extra;       /* view is a texture-pack replacement (tex_pack.c) */
    uint32_t view_levels;           /* mip levels of that view */
    uint8_t *snapshot;
    size_t snapshot_bytes;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} Tex;

static Tex *s_tex;
static int s_ntex;
static int s_tex_index[TEX_INDEX_BUCKETS];
static uint32_t *s_decode;
static size_t s_decode_cap;
static uint8_t *s_tex_probe;
static size_t s_tex_probe_cap;
static size_t s_tex_snapshot_bytes;
static GpuTextureDecoder s_texture_decoder;
static int s_tex_gpu = 1, s_tex_log, s_tex_approx, s_tex_index_enabled = 1;

/* RECOMP_TEST_LIVE_SETTINGS=1 waits until the texture cache looks in-game,
 * applies scale 5x + MSAA 4x + aniso 8x + FXAA, then re-toggles those
 * settings so evictions run under the same pressure as a reported crash. */
static void maybe_test_live_settings(void)
{
    static int started;
    static unsigned last_gen, cycle;
    const char *e;
    Halo2Settings t;
    /* Called for every draw: getenv scans the whole environment (strchr was
     * ~12% of the GPU thread in Outskirts), so read it once. */
    static int enabled = -1;
    if (enabled < 0) {
        e = getenv("RECOMP_TEST_LIVE_SETTINGS");
        enabled = e && *e && strcmp(e, "0") != 0;
    }
    if (!enabled)
        return;
    if (!started) {
        if (s_ntex < 150 || s_gen < 5000u)
            return;
        started = 1;
        last_gen = s_gen;
        halo2_settings_copy(&t);
        t.resolution_scale = 5;
        t.msaa = 4;
        t.anisotropy = 8;
        t.fxaa = 1;
        fprintf(stderr, "[SETTINGS] test live apply queued (scale=5x msaa=4 aniso=8 fxaa=1 ntex=%d gen=%u)\n",
                s_ntex, s_gen);
        fflush(stderr);
        halo2_settings_apply(&t, 0);
        return;
    }
    if (s_gen == last_gen || (s_gen - last_gen) < 90u)
        return;
    last_gen = s_gen;
    ++cycle;
    halo2_settings_copy(&t);
    t.resolution_scale = 5;
    t.msaa = (cycle % 3u == 0) ? 1 : ((cycle % 3u == 1) ? 4 : 8);
    t.anisotropy = (cycle % 2u) ? 8 : 1;
    t.fxaa = (int)(cycle % 2u);
    fprintf(stderr, "[SETTINGS] test live cycle %u queued (scale=5x msaa=%d aniso=%d fxaa=%d ntex=%d gen=%u)\n",
            cycle, t.msaa, t.anisotropy, t.fxaa, s_ntex, s_gen);
    fflush(stderr);
    halo2_settings_apply(&t, 0);
}

static void pump_live_settings(void)
{
    halo2_settings_pump();
    maybe_test_live_settings();
    halo2_settings_pump();
}


/* RECOMP_TEX_VERIFY_ONCE (default on): a cached texture whose guest bytes
 * matched its snapshot earlier in this frame, with no backend write to guest
 * memory since and the same allocation identity, is not compared again until
 * the next flip. The exact compare was ~25% of the GPU thread in gameplay.
 * A CPU write to a texture between two of its draws in one frame is seen at
 * the next frame instead of the next draw. */
static int tex_verify_once_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_TEX_VERIFY_ONCE");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* No backend write since the texture's last compare overlaps [va, va+bytes). */
static int tex_range_untouched(uint32_t since, uint32_t now, uint32_t va, size_t bytes)
{
    uint32_t lo = va & TEX_WR_MASK, ep;
    uint64_t hi = (uint64_t)lo + bytes;
    if (!bytes) return 1;
    if (now - since > TEX_WR_RING || hi > TEX_WR_MASK + 1ull) return 0;
    for (ep = since + 1; ep != now + 1; ++ep) {
        int slot = (int)(ep % TEX_WR_RING);
        if ((uint32_t)s_tex_wr[slot].epoch != ep) return 0;
        if (s_tex_wr[slot].lo < hi && lo < s_tex_wr[slot].hi) return 0;
    }
    return 1;
}

static int tex_verified(Tex *e, uint32_t va, size_t bytes,
                        uint32_t palette, size_t palette_bytes)
{
    uint32_t now = (uint32_t)s_tex_epoch;
    if (!(tex_verify_once_enabled() && !rpz_ab_off(1) && s_tex_physical_read &&
          !s_tex_strict_bounds && e->vgen == s_gen &&
          e->vsnap == e->snapshot && e->snapshot))
        return 0;
    if (e->vepoch != now) {
        if (!tex_range_untouched(e->vepoch, now, va, bytes) ||
            !tex_range_untouched(e->vepoch, now, palette, palette_bytes))
            return 0;
        e->vepoch = now;
    }
    return guest_identity_still_valid(va, bytes, GUEST_BOUNDS_IGNORE, &e->source_identity) &&
           (!palette_bytes ||
            guest_identity_still_valid(palette, palette_bytes, GUEST_BOUNDS_IGNORE,
                                       &e->palette_identity));
}

static int tex_mark_verified(Tex *e, LONG epoch)
{
    e->vgen = s_gen;
    e->vepoch = (uint32_t)epoch;
    e->vsnap = e->snapshot;
    return 1;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static unsigned tex_key_bucket_values(uint32_t offset, uint32_t width, uint32_t height,
                                      uint32_t pitch, uint32_t color, int cube,
                                      uint32_t palette, unsigned palette_entries,
                                      uint32_t levels, unsigned uploaded_levels)
{
    uint32_t key[] = {
        offset, width, height, pitch, color, (uint32_t)cube, palette, palette_entries,
        (cube || s_tex_mips) ? levels : 0,
        s_tex_mips ? uploaded_levels : 0,
    };
    uint64_t hash = hash_bytes((const uint8_t *)key, sizeof key);
    return (unsigned)((hash ^ (hash >> 32)) & (TEX_INDEX_BUCKETS - 1));
}

static unsigned tex_key_bucket(const Nv2aTexture *t, int cube, unsigned palette_entries,
                               unsigned uploaded_levels)
{
    return tex_key_bucket_values(t->offset, t->width, t->height, t->pitch, t->color,
                                 cube, t->palette, palette_entries, t->levels,
                                 uploaded_levels);
}

static unsigned tex_entry_bucket(const Tex *e)
{
    return tex_key_bucket_values(e->offset, e->w, e->h, e->pitch, e->color, e->cube,
                                 e->palette, e->palette_entries, e->levels,
                                 e->uploaded_levels);
}

static int tex_key_matches(const Tex *e, const Nv2aTexture *t, int cube,
                           unsigned palette_entries, unsigned uploaded_levels)
{
    return e->offset == t->offset && e->w == t->width && e->h == t->height &&
           e->color == t->color && e->pitch == t->pitch && e->cube == (uint8_t)cube &&
           e->palette == t->palette && e->palette_entries == palette_entries &&
           (!(cube || s_tex_mips) || e->levels == t->levels) &&
           (!s_tex_mips || e->uploaded_levels == uploaded_levels);
}

static void tex_index_rebuild(void);

static void tex_index_corrupt(const char *operation, int index)
{
    fprintf(stderr, "[GPU-D3D11] texture descriptor index corrupt during %s at slot %d;"
                    " rebuilding index instead of aborting\n",
            operation, index);
    fflush(stderr);
    tex_index_rebuild();
}

static Tex *tex_index_find(const Nv2aTexture *t, int cube, unsigned palette_entries,
                           unsigned uploaded_levels, unsigned *probes)
{
    int link = s_tex_index[tex_key_bucket(t, cube, palette_entries, uploaded_levels)];
    if (probes)
        *probes = 0;
    while (link) {
        Tex *e;
        if (link < 1 || link > s_ntex) {
            fprintf(stderr, "[GPU-D3D11] texture descriptor index corrupt during lookup at slot %d;"
                            " rebuilding index\n", link - 1);
            fflush(stderr);
            tex_index_rebuild();
            return NULL;
        }
        e = &s_tex[link - 1];
        if (probes)
            ++*probes;
        if (tex_key_matches(e, t, cube, palette_entries, uploaded_levels))
            return e;
        link = e->index_next;
    }
    return NULL;
}

static Tex *tex_descriptor_find(const Nv2aTexture *t, int cube, unsigned palette_entries,
                                unsigned uploaded_levels, unsigned *probes)
{
    int i;
    if (s_tex_index_enabled)
        return tex_index_find(t, cube, palette_entries, uploaded_levels, probes);
    if (probes)
        *probes = 0;
    for (i = 0; i < s_ntex; ++i) {
        if (probes)
            ++*probes;
        if (tex_key_matches(&s_tex[i], t, cube, palette_entries, uploaded_levels))
            return &s_tex[i];
    }
    return NULL;
}

static int tex_index_unlink(Tex *e);

static void tex_index_insert(Tex *e)
{
    int index = (int)(e - s_tex);
    unsigned bucket;
    if (!s_tex_index_enabled)
        return;
    if (index < 0 || index >= s_ntex) {
        fprintf(stderr, "[GPU-D3D11] texture descriptor index refuse insert at slot %d\n", index);
        fflush(stderr);
        return;
    }
    if (e->indexed)
        tex_index_unlink(e);
    bucket = tex_entry_bucket(e);
    e->index_bucket = (int)bucket;
    e->index_next = s_tex_index[bucket];
    s_tex_index[bucket] = index + 1;
    e->indexed = 1;
}

static int tex_index_unlink(Tex *e)
{
    int index = (int)(e - s_tex);
    int *link;
    int bucket;
    if (!s_tex_index_enabled)
        return 1;
    if (index < 0 || index >= s_ntex)
        return 0;
    if (!e->indexed) {
        e->index_next = 0;
        e->index_bucket = -1;
        return 1;
    }
    /* Unlink from the bucket recorded at insert time, not a recomputed hash.
     * Live settings can change s_tex_mips (and so the hash) long before an
     * eviction walks this entry; using the new hash is what produced
     * "remove missing" at slot 1654 and later slot 0. */
    bucket = e->index_bucket;
    if (bucket < 0 || bucket >= TEX_INDEX_BUCKETS)
        return 0;
    link = &s_tex_index[bucket];
    while (*link) {
        Tex *candidate;
        if (*link < 1 || *link > s_ntex)
            return 0;
        if (*link == index + 1) {
            *link = e->index_next;
            e->index_next = 0;
            e->index_bucket = -1;
            e->indexed = 0;
            return 1;
        }
        candidate = &s_tex[*link - 1];
        link = &candidate->index_next;
    }
    return 0;
}

static int s_tex_index_rebuilding;

static void tex_index_rebuild(void)
{
    int i;
    if (!s_tex_index_enabled || s_tex_index_rebuilding)
        return;
    s_tex_index_rebuilding = 1;
    memset(s_tex_index, 0, sizeof s_tex_index);
    for (i = 0; i < s_ntex; ++i) {
        s_tex[i].index_next = 0;
        s_tex[i].index_bucket = -1;
        s_tex[i].indexed = 0;
        tex_index_insert(&s_tex[i]);
    }
    s_tex_index_rebuilding = 0;
    fprintf(stderr, "[GPU-D3D11] rebuilt texture descriptor index (%d entries)\n", s_ntex);
    fflush(stderr);
}

static void tex_index_remove(Tex *e)
{
    int index = (int)(e - s_tex);
    if (!s_tex_index_enabled)
        return;
    if (index < 0 || index >= s_ntex) {
        fprintf(stderr, "[GPU-D3D11] texture descriptor index refuse remove at slot %d\n", index);
        fflush(stderr);
        return;
    }
    if (tex_index_unlink(e))
        return;
    fprintf(stderr, "[GPU-D3D11] texture descriptor missing during remove at slot %d;"
                    " rebuilding index\n", index);
    fflush(stderr);
    tex_index_rebuild();
    if (!tex_index_unlink(e)) {
        fprintf(stderr, "[GPU-D3D11] texture descriptor still missing at slot %d after rebuild;"
                        " ignoring\n", index);
        fflush(stderr);
        e->index_next = 0;
        e->index_bucket = -1;
        e->indexed = 0;
    }
}

static uint64_t tex_hash(const uint8_t *p, uint32_t color, size_t n)
{
    if (!n)
        return 0;
    s_st.tex_approx_hash_bytes += n;
    return hash_bytes(p, n) ^ ((uint64_t)color << 56);
}

/* RECOMP_TEX_HASH_LOG=<file>: append one line per distinct texture uploaded,
 * "hash width height color cube offset", where hash is hash_bytes() over the
 * face-0 level-0 guest bytes. This is the key the hi-res texture pack names
 * carry (analysis/textures/README.md). Off unless the variable is set. */
static void tex_hash_log(const Nv2aTexture *t, int cube, const GpuTextureLayout *layout,
                         const uint8_t *snapshot)
{
    static int state = -1;
    static FILE *f;
    static uint64_t seen[1u << 16];
    const GpuTextureSubresource *s0;
    uint64_t key;
    unsigned i;

    if (state < 0) {
        const char *path = getenv("RECOMP_TEX_HASH_LOG");
        f = path && *path ? fopen(path, "a") : NULL;
        state = f != NULL;
    }
    if (!state)
        return;
    s0 = &layout->subresource[0][0];
    if (!s0->size)
        return;
    key = hash_bytes(snapshot + s0->offset, s0->size);
    for (i = (unsigned)(key ^ (key >> 32) ^ t->width ^ (t->height << 13)) & 0xFFFFu;;
         i = (i + 1) & 0xFFFFu) {
        uint64_t tag = key ^ ((uint64_t)t->width << 40) ^ ((uint64_t)t->height << 20) ^ 1;
        if (seen[i] == tag)
            return;
        if (!seen[i]) {
            seen[i] = tag;
            break;
        }
    }
    fprintf(f, "%016llx %u %u %02x %d %08x%s", (unsigned long long)key, t->width, t->height,
            t->color, cube, t->offset, "\n");
    fflush(f);
}

/* 128 eight-byte samples spread over the texture. */
static uint64_t tex_sparse(const uint8_t *p, size_t n)
{
    size_t i;
    uint64_t h = 1469598103934665603ull;
    if (n < 8)
        return 0;
    for (i = 0; i < 128; i++) {
        uint64_t v;
        size_t off = ((n - 8) * i / 127) & ~(size_t)7;
        memcpy(&v, p + off, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

static void release_tex_snapshot(Tex *e)
{
    if (e->snapshot) {
        free(e->snapshot);
        s_tex_snapshot_bytes -= e->snapshot_bytes;
        e->snapshot = NULL;
        e->snapshot_bytes = 0;
    }
}

/* Levels of a cache entry's view, and the LOD shift its sampler needs: a
 * hi-res replacement has `lod_extra` more levels above the game's level 0,
 * so the game's max-LOD clamp moves down the chain by as many levels. Read by
 * the draw that called get_texture(), right after it returns. */
static unsigned s_tex_lod_extra;

static unsigned tex_levels_out(const Tex *e)
{
    if (e->hires) {
        s_tex_lod_extra = e->lod_extra;
        return e->view_levels;
    }
    return e->uploaded_levels ? e->uploaded_levels : 1;
}

/* control0 (NV097 texture control 0) with its min/max LOD clamps (4.8 fixed
 * point, bits 18-29 and 6-17) moved down `extra` levels for a hi-res view.
 * A min clamp of 0 stays 0, so the replacement's top level is reachable. */
static uint32_t tex_lod_control(uint32_t control0, unsigned extra)
{
    uint32_t min_lod = (control0 >> 18) & 0xFFFu, max_lod = (control0 >> 6) & 0xFFFu;
    if (!extra)
        return control0;
    if (min_lod)
        min_lod = min_lod + extra * 256 > 0xFFF ? 0xFFF : min_lod + extra * 256;
    max_lod = max_lod + extra * 256 > 0xFFF ? 0xFFF : max_lod + extra * 256;
    return (control0 & ~((0xFFFu << 18) | (0xFFFu << 6))) | min_lod << 18 | max_lod << 6;
}

static void release_tex(Tex *e)
{
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    release_tex_snapshot(e);
    e->srv = NULL;
    e->tex = NULL;
}

static void retain_tex_snapshot(Tex *e, uint8_t *snapshot, size_t bytes)
{
    int i, lru;
    if (bytes > MAX_TEX_SNAPSHOT_BYTES) {
        free(snapshot);
        return;
    }
    while (s_tex_snapshot_bytes > MAX_TEX_SNAPSHOT_BYTES - bytes) {
        lru = -1;
        for (i = 0; i < s_ntex; ++i)
            if (&s_tex[i] != e && s_tex[i].snapshot
                && (lru < 0 || s_tex[i].last_use < s_tex[lru].last_use))
                lru = i;
        if (lru < 0) {
            free(snapshot);
            return;
        }
        if (g_bruteforce_perf_counters_enabled)
            bruteforce_perf_record_texture_snapshot_eviction();
        release_tex_snapshot(&s_tex[lru]);
    }
    e->snapshot = snapshot;
    e->snapshot_bytes = bytes;
    s_tex_snapshot_bytes += bytes;
}

/* A slot for a texture: its old entry, a free one, or the least recently
 * used. */
static Tex *tex_slot(Tex *e)
{
    int i, lru = 0;
    if (e) {
        tex_index_remove(e);
        release_tex(e);
        return e;
    }
    if (s_ntex < MAX_TEX) {
        Tex *fresh = &s_tex[s_ntex++];
        fresh->index_next = 0;
        fresh->index_bucket = -1;
        fresh->indexed = 0;
        return fresh;
    }
    for (i = 1; i < s_ntex; i++)
        if (s_tex[i].last_use < s_tex[lru].last_use)
            lru = i;
    if (g_bruteforce_perf_counters_enabled)
        bruteforce_perf_record_texture_eviction();
    tex_index_remove(&s_tex[lru]);
    release_tex(&s_tex[lru]);
    return &s_tex[lru];
}

static void discard_tex(Tex *e)
{
    int index = (int)(e - s_tex);
    int last = s_ntex - 1;
    if (g_bruteforce_perf_counters_enabled)
        bruteforce_perf_record_texture_discard();
    tex_index_remove(e);
    release_tex(e);
    if (index != last) {
        tex_index_remove(&s_tex[last]);
        s_tex[index] = s_tex[last];
        tex_index_insert(&s_tex[index]);
    }
    memset(&s_tex[last], 0, sizeof s_tex[last]);
    s_ntex = last;
}

static void set_tex_key(Tex *e, const Nv2aTexture *t, int cube, unsigned palette_entries,
                        unsigned uploaded_levels)
{
    e->offset = t->offset; e->w = t->width; e->h = t->height;
    e->color = t->color; e->pitch = t->pitch; e->cube = (uint8_t)cube;
    e->levels = t->levels; e->uploaded_levels = uploaded_levels;
    e->palette = t->palette; e->palette_entries = palette_entries;
    e->last_use = s_gen;
    tex_index_insert(e);
}

static int ensure_decode(size_t texels)
{
    if (texels <= s_decode_cap)
        return 1;
    if (texels > SIZE_MAX / sizeof *s_decode)
        return 0;
    free(s_decode);
    s_decode = (uint32_t *)malloc(texels * 4);
    s_decode_cap = s_decode ? texels : 0;
    return s_decode != NULL;
}

static int ensure_tex_probe(size_t bytes)
{
    uint8_t *probe;
    if (bytes <= s_tex_probe_cap)
        return 1;
    probe = (uint8_t *)realloc(s_tex_probe, bytes);
    if (!probe)
        return 0;
    s_tex_probe = probe;
    s_tex_probe_cap = bytes;
    return 1;
}

static void report_texture_copy_failure(GuestCopyResult result, uint32_t va,
                                        size_t bytes, int palette, int p8)
{
    static unsigned reports;
    const char *part = palette ? "palette" : "texture";
    if (!p8) {
        if (reports >= 32)
            return;
        ++reports;
    }
    if (result == GUEST_COPY_SHORT)
        fprintf(stderr, "[GPU-D3D11] %s%s allocation is too short for %zu bytes at 0x%08X\n",
                p8 ? "P8 " : "", part, bytes, va);
    else if (result == GUEST_COPY_UNREGISTERED)
        fprintf(stderr, "[GPU-D3D11] %s%s span at 0x%08X (%zu bytes) lacks registered bounds\n",
                p8 ? "P8 " : "", part, va, bytes);
    else if (result == GUEST_COPY_CHANGED)
        fprintf(stderr, "[GPU-D3D11] %s%s span at 0x%08X (%zu bytes) changed allocation"
                        " or mapping during the checked read\n",
                p8 ? "P8 " : "", part, va, bytes);
    else
        fprintf(stderr, "[GPU-D3D11] %s%s span at 0x%08X (%zu bytes) is not readable\n",
                p8 ? "P8 " : "", part, va, bytes);
    if (p8) {
        fflush(stderr);
        abort();
    }
}

static void mip_fallback(const char *reason, const Nv2aTexture *t, unsigned stage)
{
    static unsigned reports;
    if (reports++ < 32)
        fprintf(stderr, "[GPU-MIPS] stage %u texture 0x%08X format 0x%02X: %s%s\n",
                stage, t->offset, t->color, reason,
                reports == 32 ? " (further mip fallback reports suppressed)" : "");
}

static int mip_filter_requested(uint32_t filter)
{
    uint32_t min_filter = (filter >> 16) & 0xFFu;
    return min_filter >= 3 && min_filter <= 6;
}

typedef enum {
    TEX_UPLOAD_FAILED,
    TEX_UPLOAD_OK,
    TEX_UPLOAD_UNSUPPORTED
} TextureUploadResult;

/* DXT blocks go as BC1-3; the other supported formats use the compute or
 * reference CPU decoder. D3D subresources are ordered by face, then level. */
static TextureUploadResult upload(Tex *e, const Nv2aTexture *t, int cube,
                                  const GpuTextureLayout *layout,
                                  const uint8_t *source, const uint8_t *palette,
                                  unsigned palette_entries, const char **path)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[GPU_TEXTURE_MAX_FACES * GPU_TEXTURE_MAX_LEVELS];
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    int dxt = (int)d3d8_format_dxt_block_bytes(t->color);
    unsigned face, level;
    HRESULT hr;
    int decoded_gpu = 0, attempted_gpu = 0;

    memset(&td, 0, sizeof td);
    td.Width = t->width;
    td.Height = t->height;
    td.MipLevels = layout->levels;
    td.ArraySize = layout->faces;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    memset(init, 0, sizeof init);
    if (dxt && !t->pitch && (t->width & 3) == 0 && (t->height & 3) == 0) {
        td.Format = t->color == 0x0C ? DXGI_FORMAT_BC1_UNORM
                  : t->color == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        for (face = 0; face < layout->faces; ++face)
            for (level = 0; level < layout->levels; ++level) {
                const GpuTextureSubresource *sub = &layout->subresource[face][level];
                unsigned index = level + face * layout->levels;
                init[index].pSysMem = source + sub->offset;
                init[index].SysMemPitch = (UINT)sub->row_pitch;
            }
        *path = "BC";
        s_st.tex_bc++;
    } else if (t->color == 0x12 && t->pitch >= t->width * 4) {
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        for (face = 0; face < layout->faces; ++face) {
            const GpuTextureSubresource *sub = &layout->subresource[face][0];
            unsigned index = face * layout->levels;
            init[index].pSysMem = source + sub->offset;
            init[index].SysMemPitch = (UINT)sub->row_pitch;
        }
        *path = "direct";
        s_st.tex_direct++;
    } else {
        if (s_tex_gpu && gpu_texture_decode_supported(t)) {
            attempted_gpu = 1;
            decoded_gpu = gpu_texture_decode(&s_texture_decoder, s_dev, s_ctx, t, layout,
                                              source, layout->span, palette, palette_entries,
                                              &e->tex);
            if (!decoded_gpu) {
                gpu_d3d11_check_device(s_dev, S_OK, "texture compute decoding");
                fprintf(stderr, "[GPU-D3D11] falling back to CPU texture decoding\n");
            }
        }
        if (decoded_gpu) {
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            *path = "GPU";
            s_st.tex_gpu++;
        } else {
            size_t total_texels = 0, used_texels = 0;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            for (face = 0; face < layout->faces; ++face)
                for (level = 0; level < layout->levels; ++level) {
                    const GpuTextureSubresource *sub = &layout->subresource[face][level];
                    size_t texels = (size_t)sub->width * sub->height;
                    if (texels > SIZE_MAX - total_texels)
                        return TEX_UPLOAD_FAILED;
                    total_texels += texels;
                }
            if (!ensure_decode(total_texels)) {
                fprintf(stderr, "[GPU-D3D11] cannot allocate CPU texture decode buffer\n");
                return TEX_UPLOAD_FAILED;
            }
            for (face = 0; face < layout->faces; ++face)
                for (level = 0; level < layout->levels; ++level) {
                    const GpuTextureSubresource *sub = &layout->subresource[face][level];
                    size_t texels = (size_t)sub->width * sub->height;
                    unsigned index = level + face * layout->levels;
                    Nv2aTexture image = *t;
                    if (t->color == 0x0B) {
                        if (!gpu_texture_decode_p8_cpu(layout, face, level, source, layout->span,
                                                       palette, palette_entries,
                                                       s_decode + used_texels, texels))
                            return TEX_UPLOAD_FAILED;
                    } else {
                        image.width = sub->width;
                        image.height = sub->height;
                        image.pitch = layout->storage == GPU_TEXTURE_LINEAR
                                    ? (uint32_t)sub->row_pitch : 0;
                        image.levels = 1;
                        if (!nv2a_backend_decode_texture_data(&image, source + sub->offset,
                                                               sub->size, s_decode + used_texels,
                                                               texels)) {
                            static unsigned failures;
                            if (failures++ < 10)
                                fprintf(stderr, "[GPU-D3D11] cannot decode texture format 0x%02X"
                                                " at 0x%08X%s\n", t->color, t->offset,
                                        failures == 10 ? " (further failures suppressed)" : "");
                            /* An attempted GPU decode may have failed transiently. */
                            return attempted_gpu ? TEX_UPLOAD_FAILED : TEX_UPLOAD_UNSUPPORTED;
                        }
                    }
                    init[index].pSysMem = s_decode + used_texels;
                    init[index].SysMemPitch = sub->width * 4;
                    used_texels += texels;
                }
            *path = "CPU";
            s_st.tex_cpu++;
        }
    }
    if (!decoded_gpu) {
        hr = ID3D11Device_CreateTexture2D(s_dev, &td, init, &e->tex);
        if (FAILED(hr)) {
            gpu_d3d11_check_device(s_dev, hr, "texture upload");
            fprintf(stderr, "[GPU-D3D11] texture upload failed 0x%08lX\n", (unsigned long)hr);
            return TEX_UPLOAD_FAILED;
        }
    }
    memset(&sv, 0, sizeof sv);
    sv.Format = td.Format;
    if (cube) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = layout->levels;
    } else {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = layout->levels;
    }
    hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, &sv, &e->srv);
    if (FAILED(hr)) {
        gpu_d3d11_check_device(s_dev, hr, "texture view creation");
        fprintf(stderr, "[GPU-D3D11] texture view creation failed 0x%08lX\n", (unsigned long)hr);
        release_tex(e);
        return TEX_UPLOAD_FAILED;
    }
    s_st.uploads++;
    return TEX_UPLOAD_OK;
}

static int s_texture_hard_reject;

static ID3D11ShaderResourceView *snapshot_bound_surface(Surf *s)
{
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;
    if (!s->sample_copy) {
        ID3D11Texture2D_GetDesc(s->tex, &desc);
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        hr = ID3D11Device_CreateTexture2D(s_dev, &desc, NULL, &s->sample_copy);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(s_dev,
                    (ID3D11Resource *)s->sample_copy, NULL, &s->sample_copy_srv);
        if (FAILED(hr)) {
            gpu_d3d11_check_device(s_dev, hr, "creating color alias snapshot");
            fprintf(stderr, "[GPU-D3D11] color alias snapshot creation failed 0x%08lX\n",
                    (unsigned long)hr);
            if (s->sample_copy) ID3D11Texture2D_Release(s->sample_copy);
            s->sample_copy = NULL;
            return NULL;
        }
    }
    surf_resolve_raw(s);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s->sample_copy,
                                    (ID3D11Resource *)s->tex);
    restore_targets();
    return s->sample_copy_srv;
}

/* A GPU-owned overlap may not be read from stale guest bytes. An exact,
 * layout-compatible surface can be sampled after reconciling its guest
 * shadow; the currently bound target needs a separate ordered GPU copy. */
static int surf_order_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_SURF_ORDER");
        on = !(e && e[0] == '0');
    }
    return on && !rpz_ab_off(128);
}

/* NV2A cannot render into palettized or compressed formats. A gpu_dirty
 * surface overlapping one of these is a stale color-buffer record over
 * reused texture-cache memory (character skins, first-person arms). */
static int texture_is_non_rt_format(uint32_t color)
{
    return color == 0x0B || color == 0x0C || color == 0x0E || color == 0x0F;
}

static int surface_exact_color_alias(const Surf *s, const Nv2aTexture *t, int cube)
{
    return !cube && (uint64_t)s->w * s->bpp <= s->pitch &&
           t->offset == s->va && t->width == s->w && t->height == s->h &&
           (t->pitch == s->pitch ||
            (t->color == 0x06 && !t->pitch && s->pitch == s->w * 4)) &&
           /* 0x1E: linear X8R8G8B8, the same bytes with alpha ignored.
            * XDK D3D samples a supersampled back buffer that way when its
            * Present filters it into the front buffer (Halo 2's disc
            * downloader); rejected, the front buffer stayed black. */
           ((s->bpp == 4 && (t->color == 0x12 || t->color == 0x06 || t->color == 0x1E ||
                             (t->color == 0x2E && !s_depth_texture))) ||
            (s->bpp == 2 && t->color == 0x11));
}

/* Clear a stale RT record when guest bytes have moved on (or cannot be
 * confirmed). Leave the bound target and an unchanged guest image alone. */
static void retire_stale_non_rt_overlap(Surf *s)
{
    int changed;
    if (!s || s == s_cur)
        return;
    changed = guest_surface_changed(s);
    if (changed != 0) {
        s->gpu_dirty = 0;
        s->guest_exact = 0;
        s->guest_check_gen = 0;
        s_st.tex_stale_surface++;
    }
}

static int d3d_readback(uint32_t color_va);

/* A texture that lies wholly inside a small GPU-newer color target that is
 * not its exact alias: the target's row is padded wider than the texture
 * (same address and pitch, 106 of 112 pixels used). Halo 2's disc downloader
 * renders its button hints and the selection bar into such targets (106x32
 * in 112x32, 71x32 in 80x32, 280x22 in 288x22) and draws them as textures;
 * rejected, the A/B hints and the highlight never appeared. Put the target's
 * pixels back in guest memory and let the texture load from there, at its
 * own size. Bounded to small targets, where a readback costs nothing. */
static int pull_small_overlap(Surf *s, uint64_t start, uint64_t end)
{
    uint64_t lo = s->va, hi = (uint64_t)s->va + (uint64_t)s->pitch * s->h;
    if (s == s_cur || !s->gpu_dirty || s->pitch * s->h > (1u << 20))
        return 0;
    if (start < lo || end > hi)
        return 0;
    return d3d_readback(s->va);
}

static int color_texture_alias(const Nv2aTexture *t, int cube,
                               const GpuTextureLayout *layout,
                               ID3D11ShaderResourceView **view)
{
    uint64_t start = t->offset, end = start + layout->span, phys = 0;
    int physical = physical_range(start, layout->span, &phys);
    int i, overlaps = 0, dirty_overlap = 0;
    Surf *exact = NULL;
    static unsigned rejected;
    int why = 0, bad = -1, other_bad = -1, sync_bad = -1;
    uint64_t newest_other = 0, newest_sync = 0;

    *view = NULL;
    for (i = 0; i < s_nsurf; ++i) {
        Surf *s = &s_surf[i];
        uint64_t span, surface_end, surface_phys = 0;
        int sp, overlap, sync;
        if (!s->tex)
            continue;
        span = (uint64_t)(s->h - 1) * s->pitch + surf_guest_row(s);
        surface_end = (uint64_t)s->va + span;
        sp = physical_range(s->va, span, &surface_phys);
        /* VA overlap, or a known physical alias. A failed physical_range
         * is unknown, not a hit (it used to make every texture overlap
         * every unmapped or overflowed surface). */
        overlap = (start < surface_end && (uint64_t)s->va < end);
        if (!overlap && physical > 0 && sp > 0)
            overlap = (phys < surface_phys + span &&
                       surface_phys < phys + layout->span);
        if (!overlap)
            continue;
        if (!surf_identity_current(s)) {
            uint32_t remaining;
            if (s->identity.layout == xbox_MemoryLayoutGeneration() &&
                s->identity.allocation &&
                !guest_allocation_identity(t->offset, &remaining))
                { why = 0; bad = s_nsurf > i ? i : -1; goto reject; }
            continue;
        }
        if (s->alias_uncertain)
            { why = 1; bad = s_nsurf > i ? i : -1; goto reject; }
        ++overlaps;
        if (surface_exact_color_alias(s, t, cube))
            exact = s;
        else
            s_st.overlap_rtt++;
        sync = sync_guest_surface(s, !surf_fast_enabled());
        if (sync != 1) {
            /* Exact color-buffer alias: sync 2 keeps the GPU frame after a
             * guest race; sync 0 cannot read guest. Sample the GPU view
             * so this pass still runs, instead of rejecting the draw. */
            if (exact == s && (s->gpu_dirty || s->srv))
                ;
            else if (texture_is_non_rt_format(t->color)) {
                retire_stale_non_rt_overlap(s);
                continue;
            } else if (sync == 2 && s->gpu_dirty && s->wserial && surf_order_enabled() &&
                       !surface_exact_color_alias(s, t, cube)) {
                /* perf10: a partial overlap whose guest bytes raced its own
                 * GPU work. Defer the verdict: if an exact color alias of
                 * this texture was written after it (write order, as xemu
                 * evicts an older overlapping surface when a newer one is
                 * bound), the exact surface owns every texel and the older
                 * record cannot matter. Halo 2 reuses the same memory for
                 * its glow/bloom chain (0x82B2B000 128x128 over the previous
                 * frame's 0x82B31800 160x120; 0x82C1B000 640x480 over
                 * 0x82C66000 320x240), which rejected those passes every
                 * frame (alias:sync_failed, stage read as 0). */
                if (s->wserial > newest_sync) { newest_sync = s->wserial; sync_bad = i; }
                continue;
            } else if (exact != s && pull_small_overlap(s, start, end)) {
                continue;
            } else {
                why = 2; bad = s_nsurf > i ? i : -1; goto reject;
            }
        }
        /* DXT/P8 cannot be an RT. A leftover gpu_dirty surface over this
         * memory is stale texture-cache reuse, not a live color target. */
        if (exact != s && s->gpu_dirty && texture_is_non_rt_format(t->color)) {
            retire_stale_non_rt_overlap(s);
            continue;
        }
        if (exact != s && s->gpu_dirty && pull_small_overlap(s, start, end))
            continue;
        if (s->gpu_dirty)
            ++dirty_overlap;
        if (exact != s && s->gpu_dirty) {
            if (surf_order_enabled()) {
                uint64_t w = s->wserial ? s->wserial : UINT64_MAX;
                if (w > newest_other) newest_other = w;
                other_bad = i;
            } else
            { why = 3; bad = s_nsurf > i ? i : -1; goto reject; }
        }
    }
    if (newest_sync) {
        /* Depth formats keep the old verdict: a color record at a shadow
         * map's address is not its depth, so it must not win by order. */
        if (!exact || t->color == 0x2E || exact->wserial <= newest_sync)
            { why = 2; bad = sync_bad; goto reject; }
        s_order_wins++;
        s_sync_order_wins++;
    }
    if (newest_other) {
        if (!exact || newest_other == UINT64_MAX || exact->wserial <= newest_other)
            { why = 3; bad = other_bad; goto reject; }
        s_order_wins++;
    }
    /* dirty_overlap includes the exact match. The exact surface being
     * GPU-dirty is why we want to sample it (the GPU copy is current).
     * Only a dirty *other* overlapping surface is "beside" it. Counting
     * the exact surface itself as a blocker dropped ~69k lighting draws
     * per window (black council / Outskirts regions) after texture-rejected
     * batches stopped falling back to the CPU. */
    {
        int dirty_beside = dirty_overlap - (exact && exact->gpu_dirty ? 1 : 0);
        if (exact && (overlaps == 1 || !dirty_beside || newest_other)) {
            if (s_tex_strict_bounds) {
                GpuTextureLayout base;
                uint32_t remaining;
                uint64_t id = xbox_GuestBoundsIdentity(t->offset, &remaining);
                if (!gpu_texture_layout(t, 1, 0, &base) || !id || base.span > remaining)
                    { why = 5; bad = s_nsurf > i ? i : -1; goto reject; }
            }
            if (exact->msaa_dirty) {
                s_st.msaa_resolve_samples++;
#ifdef RECOMP_DEBUG_TOOLS
                debug_note("msaa_resolve", exact == s_cur ? "bound" : "unbound",
                           exact->va, exact->w, exact->h, exact->bpp,
                           exact->msaa_count);
#endif
                surf_resolve(exact);
            }
            *view = exact == s_cur ? snapshot_bound_surface(exact) : exact->srv;
            if (!*view)
                { why = 6; bad = s_nsurf > i ? i : -1; goto reject; }
            s_st.rtt++;
            return 1;
        }
        if (dirty_beside)
            { why = 4; bad = s_nsurf > i ? i : -1; goto reject; }
    }
    return 0;
reject:
    s_arej[why]++;
#ifdef RECOMP_DEBUG_TOOLS
    {
        static const char *const arej_name[7] = {
            "allocation_gone", "alias_uncertain", "sync_failed", "dirty_non_exact",
            "dirty_beside_exact", "strict_bounds", "no_view"
        };
        const char *nm = (why >= 0 && why < 7) ? arej_name[why] : "alias";
        debug_note_set_block(bad >= 0 && bad < s_nsurf ? &s_surf[bad] : NULL);
        debug_note("alias", nm, t->offset, t->width, t->height, t->color, (uint32_t)why);
    }
#endif
    {
        static unsigned detailed;
        if (detailed < 40) {
            const Surf *o = bad >= 0 && bad < s_nsurf ? &s_surf[bad] : NULL;
            ++detailed;
            fprintf(stderr, "[ALIAS] why=%d tex %08X %ux%u pitch %u fmt %02X cube %d levels %u"
                            " | surf %08X %ux%u pitch %u bpp %u dirty %d uncertain %d cur %d\n",
                    why, t->offset, t->width, t->height, t->pitch, t->color, cube, t->levels,
                    o ? o->va : 0, o ? o->w : 0, o ? o->h : 0, o ? o->pitch : 0, o ? o->bpp : 0,
                    o ? o->gpu_dirty : -1, o ? o->alias_uncertain : -1, o ? o == s_cur : -1);
        }
    }
    if (rejected < 32) {
        ++rejected;
        fprintf(stderr, "[GPU-D3D11] color texture alias at %08X is conflicting,"
                        " ambiguous or unsafe; draw rejected%s\n", t->offset,
                rejected == 32 ? " (further reports suppressed)" : "");
    }
    return -1;
}

/* A previously uploaded view of this descriptor, ignoring guest-identity
 * checks. Used when a color-alias or copy reject would otherwise drop a
 * whole character mesh. Never returns an unsupported placeholder. */
static ID3D11ShaderResourceView *tex_stale_view(const Nv2aTexture *t, int cube,
                                               unsigned levels, unsigned *mip_levels)
{
    Tex *best;
    int i;
    unsigned pal = 0;
    if (!t || !t->width || !t->height)
        return NULL;
    if (t->color == 0x0B)
        pal = nv2a_p8_palette_entries(t->palette_entries);
    best = tex_descriptor_find(t, cube, pal, levels ? levels : 1, NULL);
    if (best && (!best->srv || best->unsupported))
        best = NULL;
    if (!best) {
        /* Same address and size, any palette or mip count. Pick the last
         * used view so a mid-frame identity reject can still draw the mesh. */
        for (i = 0; i < s_ntex; ++i) {
            Tex *e = &s_tex[i];
            if (!e->srv || e->unsupported)
                continue;
            if (e->offset != t->offset || e->w != t->width || e->h != t->height ||
                e->color != t->color || e->cube != (uint8_t)cube)
                continue;
            if (t->pitch && e->pitch && e->pitch != t->pitch)
                continue;
            if (!best || e->last_use >= best->last_use)
                best = e;
        }
    }
    if (!best)
        return NULL;
    best->last_use = s_gen;
    if (mip_levels)
        *mip_levels = tex_levels_out(best);
    s_st.tex_stale_fallback++;
    return best->srv;
}

static const char *s_tex_fail;
static int s_texture_soft_reject;

/* The view a stage samples, and the size its texel-space coordinates are
 * divided by (*tw, *th). */
static ID3D11ShaderResourceView *get_texture(const Nv2aTexture *t, int cube, unsigned stage,
                                             float *tw, float *th, unsigned *mip_levels)
{
    Tex *e = NULL;
    ID3D11ShaderResourceView *alias = NULL;
    Nv2aTexture normalized;
    GpuTextureLayout layout, base;
    uint64_t h, palette_hash = 0;
    size_t source_bytes, palette_bytes, snapshot_bytes;
    uint8_t *snapshot;
    GuestSpanIdentity source_identity = {0}, palette_identity = {0};
    GuestCopyResult copy_result;
    unsigned palette_entries = 0, invalid_index;
    unsigned perf_probes = 0;
    const char *path = NULL;
    TextureUploadResult upload_result;
    uint64_t perf_start = 0;
    int perf = g_bruteforce_perf_counters_enabled;
    int rewritten, approximate, same_lifetime;
    int alias_result, faces = cube ? 6 : 1;

    s_texture_hard_reject = 0;
    s_texture_soft_reject = 0;
    s_tex_fail = NULL;
    s_tex_lod_extra = 0;
    *mip_levels = 1;
    if (!t || !t->width || !t->height || t->width > 4096 || t->height > 4096) {
        s_texture_soft_reject = 1;
        s_tex_fail = "size";
        s_st.tex_soft++;
        return NULL;
    }
    if (t->pitch && (d3d8_format_is_swizzled(t->color)
                     || d3d8_format_dxt_block_bytes(t->color))) {
        normalized = *t;
        normalized.pitch = 0;
        t = &normalized;
        s_st.tex_pitch++;
    }
    *tw = (float)t->width;
    *th = (float)t->height;
    if (s_depth_texture && t->color == 0x2E) {
        uint32_t levels = (nv2a_backend_reg(0x1B04 + stage * 0x40) >> 16) & 0xF;
        if (s_tex_mips && (levels > 1 || mip_filter_requested(t->filter)))
            mip_fallback(levels > 1 ? "multi-level depth alias unsupported; texture rejected"
                                    : "depth alias is level 0 only", t, stage);
        return get_depth_texture(t, cube, levels);
    }
    if (s_stencil_alias && t->color == 0x41) {
        int ambiguous;
        Depth *overlap = overlapping_depth(t, cube, &ambiguous);
        uint32_t format = nv2a_backend_reg(0x1B04 + stage * 0x40);
        uint32_t levels = (format >> 16) & 0xF;
        if (overlap) {
            if (s_tex_mips && (levels > 1 || mip_filter_requested(t->filter)))
                mip_fallback(levels > 1 ? "multi-level stencil alias unsupported; texture rejected"
                                        : "stencil alias is level 0 only", t, stage);
            return get_packed_depth_texture(t, cube || (format & 4) != 0, levels, 0);
        }
    }
    if (depth_argb_alias_on() && (t->color == 0x12 || t->color == 0x06) && !cube) {
        /* A 32-bit colour texture over a tracked depth surface: the depth
         * word as the guest would read it, not stale guest memory. */
        int ambiguous, i;
        Depth *overlap = overlapping_depth(t, cube, &ambiguous);
        for (i = 0; overlap && i < s_ndepth; i++)
            if (s_depth[i].tex && s_depth[i].va == t->offset) {
                ID3D11ShaderResourceView *v;
                if (!depth_still_current(&s_depth[i])) {
                    static int noted_stale;
                    if (!noted_stale++)
                        fprintf(stderr, "[GPU-D3D11] colour texture over depth %08X: memory"
                                        " reused as colour, guest texture sampled\n",
                                t->offset);
                    break;
                }
                v = get_packed_depth_texture(t, 0, 1, 1);
                static int noted;
                if (v) {
                    if (!noted++)
                        fprintf(stderr, "[GPU-D3D11] depth %08X sampled as colour format"
                                        " 0x%02X: packed depth view\n", t->offset, t->color);
                    return v;
                }
                break;
            }
    }
    if (depth_views_enabled()) {
        int ambiguous;
        if (overlapping_depth(t, cube, &ambiguous)) {
            static int warned_overlap;
            s_st.depth_rejected++;
            s_texture_soft_reject = 1;
            s_tex_fail = "depth_rejected";
            s_st.tex_soft++;
            if (!warned_overlap++)
                fprintf(stderr, "[GPU-D3D11] texture overlaps GPU-owned depth with"
                                " an unsupported format or address; texture rejected\n");
            return NULL;
        }
    }
    if (!gpu_texture_layout(t, faces, s_tex_mips, &layout)) {
        fprintf(stderr, "[GPU-D3D11] invalid texture span or layout for format 0x%02X, %ux%u"
                        " faces=%d levels=%u\n", t->color, t->width, t->height, faces, t->levels);
        s_texture_soft_reject = 1;
        s_tex_fail = "invalid_layout";
        s_st.tex_soft++;
        return NULL;
    }
    alias_result = color_texture_alias(t, cube, &layout, &alias);
    if (rtlog_on())
        fprintf(stderr, "[RTLOG] g%u tex %08X %ux%u pitch %u fmt %02X span %zu -> %s (target %08X)\n",
                s_gen, t->offset, t->width, t->height, t->pitch, t->color, (size_t)layout.span,
                alias_result < 0 ? "REJECT" : alias_result > 0 ? "rtt" : "guest",
                s_cur ? s_cur->va : 0);
    if (alias_result < 0) {
        ID3D11ShaderResourceView *stale = tex_stale_view(t, cube, layout.levels, mip_levels);
        if (stale) {
            s_tex_fail = "alias_stale_cache";
#ifdef RECOMP_DEBUG_TOOLS
            debug_note("stale_cache", "alias", t->offset, t->width, t->height, t->color, 0);
#endif
            return stale;
        }
        /* Keep the draw: this stage samples nothing rather than dropping
         * the whole mesh (missing heads, bodies, or first-person arms). */
        s_texture_soft_reject = 1;
        s_tex_fail = "alias_soft";
        s_st.tex_soft++;
        return NULL;
    }
    if (alias_result > 0) {
        if (s_tex_mips && (t->levels > 1 || mip_filter_requested(t->filter)))
            mip_fallback("render-target alias is level 0 only", t, stage);
        return alias;
    }
    /* Unsupported-format entries depend on metadata and allocation identity,
     * not the inaccessible bytes. Preserve the existing no-read fallback. */
    if (t->color != 0x0B) {
        Tex *cached = tex_descriptor_find(t, cube, 0, layout.levels, NULL);
        if (cached && cached->unsupported) {
            uint32_t remaining;
            GuestSpanIdentity current = {0};
            current.layout = xbox_MemoryLayoutGeneration();
            current.allocation = guest_allocation_identity(t->offset, &remaining);
            if (s_tex_strict_bounds)
                current.bounds = xbox_GuestBoundsIdentity(t->offset, &remaining);
            if ((!s_tex_strict_bounds || (current.bounds && layout.span <= remaining)) &&
                same_guest_identity(&cached->source_identity, &current)) {
                cached->last_use = s_gen;
                s_st.tex_unsupported_hits++;
                s_texture_soft_reject = 1;
                s_tex_fail = "unsupported";
                s_st.tex_soft++;
                if (perf)
                    bruteforce_perf_record_texture_unsupported_hit();
                return NULL;
            }
        }
        LONG epoch = s_tex_epoch;
        if (cached && !cached->unsupported && cached->snapshot_bytes == layout.span &&
            (tex_verified(cached, t->offset, layout.span, 0, 0) ||
             (guest_physical_bytes_match(t->offset, cached->snapshot, layout.span,
                                         &cached->source_identity) &&
              tex_mark_verified(cached, epoch)))) {
            cached->last_use = s_gen;
            *mip_levels = tex_levels_out(cached);
            s_st.texture_physical_hits++;
            return cached->srv;
        }
    }
    if (s_tex_mips) {
        if (layout.storage != GPU_TEXTURE_LINEAR && t->levels > layout.levels)
            mip_fallback("declared levels exceed the 1x1 chain; capped", t, stage);
        if (layout.storage == GPU_TEXTURE_LINEAR && mip_filter_requested(t->filter))
            mip_fallback("linear texture has no mip chain; sampling level 0", t, stage);
        else if (layout.levels == 1 && mip_filter_requested(t->filter))
            mip_fallback("no additional mip levels declared; sampling level 0", t, stage);
    }
    if (t->color == 0x0B) {
        palette_entries = nv2a_p8_palette_entries(t->palette_entries);
        if (!palette_entries) {
            fprintf(stderr, "[GPU-D3D11] invalid P8 palette length %u at stage %u\n",
                    t->palette_entries, stage);
            s_texture_soft_reject = 1;
            s_tex_fail = "palette_len";
            s_st.tex_soft++;
            return NULL;
        }
    }
    palette_bytes = (size_t)palette_entries * 4;
    if (palette_bytes && s_tex_physical_read && !s_tex_strict_bounds &&
        palette_bytes <= SIZE_MAX - layout.span) {
        Tex *cached = tex_descriptor_find(t, cube, palette_entries, layout.levels, NULL);
        LONG epoch = s_tex_epoch;
        if (cached && !cached->unsupported && cached->snapshot &&
            cached->snapshot_bytes == layout.span + palette_bytes &&
            (tex_verified(cached, t->offset, layout.span, t->palette, palette_bytes) ||
             (guest_physical_bytes_match(t->offset, cached->snapshot, layout.span,
                                         &cached->source_identity) &&
              guest_physical_bytes_match(t->palette, cached->snapshot + layout.span, palette_bytes,
                                         &cached->palette_identity) &&
              tex_mark_verified(cached, epoch))) &&
            guest_identity_still_valid(t->offset, layout.span, GUEST_BOUNDS_IGNORE,
                                       &cached->source_identity)) {
            cached->last_use = s_gen;
            *mip_levels = tex_levels_out(cached);
            s_st.texture_physical_hits++;
            return cached->srv;
        }
    }
    if (palette_bytes > SIZE_MAX - layout.span || !ensure_tex_probe(layout.span + palette_bytes)) {
        fprintf(stderr, "[GPU-D3D11] cannot allocate checked texture snapshot\n");
        return NULL;
    }
    copy_result = checked_guest_copy(t->offset, layout.span, s_tex_probe,
                                     s_tex_strict_bounds, &source_identity, GUEST_READ_TEXELS);
    if (copy_result != GUEST_COPY_OK && s_tex_mips && layout.levels > 1 &&
        gpu_texture_layout(t, faces, 0, &base) &&
        (!cube || base.face_stride == layout.face_stride) &&
        checked_guest_copy(t->offset, base.span, s_tex_probe, s_tex_strict_bounds,
                           &source_identity, GUEST_READ_TEXELS) == GUEST_COPY_OK) {
        mip_fallback("mip chain is unreadable or shorter than its tracked allocation;"
                     " falling back to level 0", t, stage);
        layout = base;
        copy_result = GUEST_COPY_OK;
    }
    if (copy_result != GUEST_COPY_OK) {
        if (s_tex_mips)
            mip_fallback("texture span is unreadable or shorter than its tracked allocation;"
                         " texture rejected", t, stage);
        report_texture_copy_failure(copy_result, t->offset, layout.span, 0, t->color == 0x0B);
        {
            ID3D11ShaderResourceView *stale = tex_stale_view(t, cube, layout.levels, mip_levels);
            if (stale) {
                s_tex_fail = "copy_stale_cache";
#ifdef RECOMP_DEBUG_TOOLS
                debug_note("stale_cache", "copy", t->offset, t->width, t->height, t->color, 0);
#endif
                return stale;
            }
        }
        s_texture_soft_reject = 1;
        s_tex_fail = "copy_soft";
        s_st.tex_soft++;
        return NULL;
    }
    source_bytes = layout.span;
    if (palette_bytes) {
        copy_result = checked_guest_copy(t->palette, palette_bytes, s_tex_probe + source_bytes,
                                         s_tex_strict_bounds, &palette_identity, GUEST_READ_PALETTE);
        if (copy_result != GUEST_COPY_OK) {
            report_texture_copy_failure(copy_result, t->palette, palette_bytes, 1, 1);
            {
                ID3D11ShaderResourceView *stale = tex_stale_view(t, cube, layout.levels, mip_levels);
                if (stale) {
                    s_tex_fail = "palette_stale_cache";
#ifdef RECOMP_DEBUG_TOOLS
                    debug_note("stale_cache", "palette", t->offset, t->width, t->height, t->color, 0);
#endif
                    return stale;
                }
            }
            s_texture_soft_reject = 1;
            s_tex_fail = "palette_copy";
            s_st.tex_soft++;
            return NULL;
        }
    }
    if (!guest_identity_still_valid(t->offset, source_bytes, s_tex_strict_bounds,
                                    &source_identity) ||
        (palette_bytes && !guest_identity_still_valid(t->palette, palette_bytes,
                                                       s_tex_strict_bounds,
                                                       &palette_identity))) {
        report_texture_copy_failure(GUEST_COPY_CHANGED, t->offset, source_bytes, 0,
                                    t->color == 0x0B);
        {
            ID3D11ShaderResourceView *stale = tex_stale_view(t, cube, layout.levels, mip_levels);
            if (stale) {
                s_tex_fail = "identity_stale_cache";
#ifdef RECOMP_DEBUG_TOOLS
                debug_note("stale_cache", "identity", t->offset, t->width, t->height, t->color, 0);
#endif
                return stale;
            }
        }
        s_texture_soft_reject = 1;
        s_tex_fail = "identity_soft";
        s_st.tex_soft++;
        return NULL;
    }
    if (palette_bytes > SIZE_MAX - source_bytes)
        return NULL;
    snapshot_bytes = source_bytes + palette_bytes;
    approximate = s_tex_approx && !(s_tex_mips && layout.storage != GPU_TEXTURE_LINEAR &&
                                   layout.declared_levels > 1);
    if (s_tex_approx && !approximate) {
        static int reported;
        if (!reported++)
            fprintf(stderr, "[GPU-MIPS] mipped textures use exact snapshots even when"
                            " the approximate cache is enabled\n");
    }
    if (perf)
        perf_start = bruteforce_perf_clock();
    e = tex_descriptor_find(t, cube, palette_entries, layout.levels,
                            perf ? &perf_probes : NULL);
    if (perf)
        bruteforce_perf_record_texture_lookup(perf_probes, e != NULL,
                                              bruteforce_perf_clock() - perf_start);
    same_lifetime = e &&
        e->source_identity.layout == source_identity.layout &&
        e->source_identity.allocation == source_identity.allocation &&
        e->source_identity.bounds == source_identity.bounds &&
        e->palette_identity.layout == palette_identity.layout &&
        e->palette_identity.allocation == palette_identity.allocation &&
        e->palette_identity.bounds == palette_identity.bounds;
    if (e && e->source_identity.layout == source_identity.layout &&
        e->source_identity.allocation && !source_identity.allocation &&
        !source_identity.bounds) {
        report_texture_copy_failure(GUEST_COPY_CHANGED, t->offset, source_bytes, 0,
                                    t->color == 0x0B);
        {
            ID3D11ShaderResourceView *stale = tex_stale_view(t, cube, layout.levels, mip_levels);
            if (stale) {
                s_tex_fail = "alloc_stale_cache";
#ifdef RECOMP_DEBUG_TOOLS
                debug_note("stale_cache", "alloc", t->offset, t->width, t->height, t->color, 0);
#endif
                return stale;
            }
        }
        s_texture_soft_reject = 1;
        s_tex_fail = "alloc_soft";
        s_st.tex_soft++;
        return NULL;
    }
    if (e && palette_bytes && e->palette_identity.layout == palette_identity.layout &&
        e->palette_identity.allocation && !palette_identity.allocation &&
        !palette_identity.bounds) {
        report_texture_copy_failure(GUEST_COPY_CHANGED, t->palette, palette_bytes, 1, 1);
        return NULL;
    }
    if (same_lifetime && e->unsupported) {
        e->last_use = s_gen;
        s_st.tex_unsupported_hits++;
        if (perf)
            bruteforce_perf_record_texture_unsupported_hit();
        return NULL;
    }
    if (approximate && palette_bytes) {
        s_st.tex_approx_hash_bytes += palette_bytes;
        palette_hash = hash_bytes(s_tex_probe + source_bytes, palette_bytes);
    }
    if (!approximate) {
        if (same_lifetime && e->snapshot && e->snapshot_bytes == snapshot_bytes) {
            size_t compared_palette_bytes = 0;
            int source_equal, palette_equal;
            if (perf)
                perf_start = bruteforce_perf_clock();
            source_equal = !memcmp(e->snapshot, s_tex_probe, source_bytes);
            palette_equal = !palette_bytes;
            if (source_equal && palette_bytes) {
                compared_palette_bytes = palette_bytes;
                palette_equal = !memcmp(e->snapshot + source_bytes,
                                        s_tex_probe + source_bytes, palette_bytes);
            }
            if (perf)
                bruteforce_perf_record_texture_exact(
                    source_bytes, compared_palette_bytes, source_equal && palette_equal,
                    bruteforce_perf_clock() - perf_start);
            if (source_equal && palette_equal) {
                e->last_use = s_gen;
                *mip_levels = tex_levels_out(e);
                return e->srv;
            }
        }
    } else {
        if (same_lifetime && e->gen == s_gen && e->palette_hash == palette_hash) {
            e->last_use = s_gen;
            *mip_levels = tex_levels_out(e);
            return e->srv;
        }
        if (same_lifetime && e->palette_hash == palette_hash && e->stable >= 30
                && ((s_gen + (e->offset >> 12)) & 31) != 0
                && tex_sparse(s_tex_probe, source_bytes) == e->sparse) {
            e->gen = s_gen;
            e->last_use = s_gen;
            *mip_levels = tex_levels_out(e);
            return e->srv;
        }
        h = tex_hash(s_tex_probe, t->color, source_bytes);
        if (same_lifetime && e->hash == h && h && e->palette_hash == palette_hash) {
            if (e->stable < 0xFFFFFFFFu)
                e->stable++;
            if (e->stable == 30)
                e->sparse = tex_sparse(s_tex_probe, source_bytes);
            e->gen = s_gen;
            e->last_use = s_gen;
            *mip_levels = tex_levels_out(e);
            return e->srv;
        }
    }
    snapshot = (uint8_t *)malloc(snapshot_bytes);
    if (!snapshot) {
        fprintf(stderr, "[GPU-D3D11] cannot allocate %zu-byte texture snapshot\n", snapshot_bytes);
        fflush(stderr);
        abort();
    }
    if (perf)
        perf_start = bruteforce_perf_clock();
    memcpy(snapshot, s_tex_probe, snapshot_bytes);
    tex_hash_log(t, cube, &layout, snapshot);
    if (perf)
        bruteforce_perf_record_texture_snapshot(snapshot_bytes,
                                                bruteforce_perf_clock() - perf_start);
    if (palette_bytes && !gpu_texture_p8_indices_valid(&layout, snapshot, source_bytes,
                                                        palette_entries, &invalid_index)) {
        if (invalid_index < 256)
            fprintf(stderr, "[GPU-D3D11] P8 index %u exceeds %u-entry palette at stage %u;"
                            " hardware behavior is not established\n",
                    invalid_index, palette_entries, stage);
        else
            fprintf(stderr, "[GPU-D3D11] invalid P8 texture dimensions or layout at stage %u\n",
                    stage);
        free(snapshot);
        fflush(stderr);
        abort();
    }
    if (approximate) {
        s_st.tex_approx_hash_bytes += source_bytes;
        h = hash_bytes(snapshot, source_bytes) ^ ((uint64_t)t->color << 56);
        if (palette_bytes) {
            s_st.tex_approx_hash_bytes += palette_bytes;
            palette_hash = hash_bytes(snapshot + source_bytes, palette_bytes);
        }
    } else {
        h = 0;
    }
    rewritten = e != NULL;
    if (perf && rewritten)
        bruteforce_perf_record_texture_rewrite();
    e = tex_slot(e);
    memset(e, 0, sizeof *e);
    set_tex_key(e, t, cube, palette_entries, layout.levels);
    e->source_identity = source_identity;
    e->palette_identity = palette_identity;
    upload_result = upload(e, t, cube, &layout, snapshot,
                           palette_bytes ? snapshot + source_bytes : NULL,
                           palette_entries, &path);
    if (upload_result != TEX_UPLOAD_OK) {
        free(snapshot);
        if (upload_result == TEX_UPLOAD_UNSUPPORTED) {
            e->unsupported = 1;
            s_st.tex_unsupported++;
#ifdef RECOMP_DEBUG_TOOLS
            debug_note("unsupported_tex", "upload", t->offset, t->width, t->height,
                       t->color, 0);
#endif
        } else {
            discard_tex(e);
            if (s_tex_log)
                fprintf(stderr, "[GPU-TEX] discarded failed upload (%d cache entries)\n", s_ntex);
        }
        return NULL;
    }
    e->palette_hash = palette_hash;
    e->hash = h; e->gen = s_gen;
    if (texpack_enabled() && layout.subresource[0][0].size) {
        const GpuTextureSubresource *s0 = &layout.subresource[0][0];
        ID3D11Texture2D *ptex;
        ID3D11ShaderResourceView *psrv;
        unsigned plevels, pextra;
        if (texpack_lookup(s_dev, hash_bytes(snapshot + s0->offset, s0->size), t->width,
                           t->height, cube, &ptex, &psrv, &plevels, &pextra)) {
            if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
            if (e->tex) ID3D11Texture2D_Release(e->tex);
            e->tex = ptex;
            e->srv = psrv;
            e->hires = 1;
            e->lod_extra = (uint8_t)pextra;
            e->view_levels = plevels;
            s_st.tex_hires++;
        }
    }
    if (approximate)
        free(snapshot);
    else
        retain_tex_snapshot(e, snapshot, snapshot_bytes);
    if (s_tex_log)
        fprintf(stderr, "[GPU-TEX] %s %s va=0x%08X format=0x%02X %ux%u pitch=%u"
                        " faces=%d levels=%u uploaded=%u palette=0x%08X\n",
                rewritten ? "changed" : "new", path, t->offset, t->color, t->width,
                t->height, t->pitch, faces, t->levels, layout.levels, t->palette);
    *mip_levels = e->hires ? tex_levels_out(e) : layout.levels;
    return e->srv;
}

/* ---- state objects --------------------------------------------------------- */

static D3D11_BLEND map_blend(uint32_t f)
{
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return D3D11_BLEND_SRC_COLOR;
    case 0x301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return D3D11_BLEND_DEST_COLOR;
    case 0x307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND alpha_only(D3D11_BLEND b)
{
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return b;
    }
}

static D3D11_BLEND_OP map_blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC map_cmp(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default: return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP map_sop(uint32_t o)
{
    switch (o) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

static int depth_state_can_write(const Nv2aRenderState *rs)
{
    D3D11_COMPARISON_FUNC depth = rs->depth_test_enable ? map_cmp(rs->depth_func)
                                                        : D3D11_COMPARISON_ALWAYS;
    D3D11_COMPARISON_FUNC stencil = rs->stencil_enable ? map_cmp(rs->stencil_func)
                                                       : D3D11_COMPARISON_ALWAYS;

    if (rs->depth_write && stencil != D3D11_COMPARISON_NEVER &&
        depth != D3D11_COMPARISON_NEVER)
        return 1;
    if (!rs->stencil_enable || !(rs->stencil_write_mask & 0xFF))
        return 0;
    if (stencil != D3D11_COMPARISON_ALWAYS &&
        map_sop(rs->stencil_fail) != D3D11_STENCIL_OP_KEEP)
        return 1;
    if (stencil == D3D11_COMPARISON_NEVER)
        return 0;
    if (depth != D3D11_COMPARISON_ALWAYS &&
        map_sop(rs->stencil_zfail) != D3D11_STENCIL_OP_KEEP)
        return 1;
    return depth != D3D11_COMPARISON_NEVER &&
           map_sop(rs->stencil_zpass) != D3D11_STENCIL_OP_KEEP;
}

static void depth_written(Depth *depth)
{
    depth->gpu_dirty = 1;
    if (++depth->revision == 0)
        depth->packed_valid = 0;
}

#define MAX_STATE 256
typedef struct { uint32_t key[5]; ID3D11BlendState *bs; } BlendEntry;
typedef struct { uint32_t key[10]; ID3D11DepthStencilState *ds; } DsEntry;
typedef struct { D3D11_SAMPLER_DESC key; ID3D11SamplerState *ss; } SampEntry;
static BlendEntry s_blend[MAX_STATE];
static int s_nblend;
static DsEntry s_ds[MAX_STATE];
static int s_nds;
#define MAX_SAMP 4096
static SampEntry s_samp[MAX_SAMP];
static int s_nsamp;

static ID3D11BlendState *get_blend(const Nv2aRenderState *rs)
{
    uint32_t key[5] = { rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq,
                        rs->color_mask };
    D3D11_BLEND_DESC bd;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];
    UINT8 mask = 0;
    int i;

    if (scale_test_failure("blend-state"))
        return NULL;
    for (i = 0; i < s_nblend; i++)
        if (!memcmp(s_blend[i].key, key, sizeof key))
            return s_blend[i].bs;
    if (s_nblend == MAX_STATE) {
        fprintf(stderr, "[GPU-D3D11] blend-state cache is full; draw rejected\n");
        return NULL;
    }
    memset(&bd, 0, sizeof bd);
    t->BlendEnable = rs->blend_enable ? TRUE : FALSE;
    t->SrcBlend = map_blend(rs->blend_src);
    t->DestBlend = map_blend(rs->blend_dst);
    t->BlendOp = map_blend_op(rs->blend_eq);
    t->SrcBlendAlpha = alpha_only(t->SrcBlend);
    t->DestBlendAlpha = alpha_only(t->DestBlend);
    t->BlendOpAlpha = t->BlendOp;
    if (rs->color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (rs->color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (rs->color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (rs->color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    t->RenderTargetWriteMask = mask;
    memcpy(s_blend[s_nblend].key, key, sizeof key);
    if (gpu_call_failed(ID3D11Device_CreateBlendState(s_dev, &bd, &s_blend[s_nblend].bs),
                        "creating blend state"))
        return NULL;
    return s_blend[s_nblend++].bs;
}

static ID3D11BlendState *get_required_blend(const Nv2aRenderState *rs)
{
    ID3D11BlendState *state = get_blend(rs);
    if (!state) {
        fprintf(stderr, "[GPU-D3D11] cannot create blend state; refusing to"
                        " draw with a default blend or color mask, stopping\n");
        fflush(stderr);
        abort();
    }
    return state;
}

static ID3D11DepthStencilState *get_ds(const Nv2aRenderState *rs, int has_depth)
{
    uint32_t key[10] = { has_depth && rs->depth_test_enable, rs->depth_func,
                         has_depth && rs->depth_write, has_depth && rs->stencil_enable,
                         rs->stencil_func, rs->stencil_func_mask, rs->stencil_write_mask,
                         rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass };
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;

    if (scale_test_failure("depth-state"))
        return NULL;
    for (i = 0; i < s_nds; i++)
        if (!memcmp(s_ds[i].key, key, sizeof key))
            return s_ds[i].ds;
    if (s_nds == MAX_STATE) {
        fprintf(stderr, "[GPU-D3D11] depth-stencil state cache is full; draw rejected\n");
        return NULL;
    }
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = key[0] || key[2] ? TRUE : FALSE;
    dd.DepthFunc = key[0] ? map_cmp(rs->depth_func) : D3D11_COMPARISON_ALWAYS;
    dd.DepthWriteMask = key[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = key[3] ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
    dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    dd.FrontFace.StencilFunc = map_cmp(rs->stencil_func);
    dd.FrontFace.StencilFailOp = map_sop(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = map_sop(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp = map_sop(rs->stencil_zpass);
    dd.BackFace = dd.FrontFace;
    memcpy(s_ds[s_nds].key, key, sizeof key);
    if (gpu_call_failed(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds[s_nds].ds),
                        "creating depth-stencil state"))
        return NULL;
    return s_ds[s_nds++].ds;
}

static ID3D11DepthStencilState *get_required_ds(const Nv2aRenderState *rs, int has_depth)
{
    ID3D11DepthStencilState *state = get_ds(rs, has_depth);
    if (!state) {
        fprintf(stderr, "[GPU-D3D11] cannot create depth-stencil state; refusing to"
                        " draw with untracked default depth writes, stopping\n");
        fflush(stderr);
        abort();
    }
    return state;
}

static ID3D11SamplerState *get_sampler(uint32_t addr_u, uint32_t addr_v, uint32_t filter,
                                       uint32_t control0, unsigned levels,
                                       uint32_t border_src, uint32_t border)
{
    D3D11_SAMPLER_DESC sd;
    unsigned warnings;
    int i;

    if (scale_test_failure("sampler-state"))
        return NULL;
    warnings = gpu_texture_sampler_desc(&sd, addr_u, addr_v, filter, control0,
                                        levels, s_tex_mips, s_tex_aniso);
    /* perf11: NV2A BORDER (4) with the border source set to colour reads
     * SET_TEXTURE_BORDER_COLOR outside the image. Mapping it to CLAMP smeared
     * the edge texels of projected light/shadow textures across everything
     * outside the projection: the hard lines that move as the player walks.
     * Without the colour source NV2A clamps, as before. */
    if (border_src && (addr_u == 4 || addr_v == 4)) {
        if (addr_u == 4) sd.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        if (addr_v == 4) sd.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        sd.BorderColor[0] = (float)((border >> 16) & 0xFF) / 255.0f;
        sd.BorderColor[1] = (float)((border >>  8) & 0xFF) / 255.0f;
        sd.BorderColor[2] = (float)( border        & 0xFF) / 255.0f;
        sd.BorderColor[3] = (float)((border >> 24) & 0xFF) / 255.0f;
    }
    if (warnings) {
        static unsigned reports;
        if (reports++ < 16) {
            fprintf(stderr, "[GPU-MIPS] sampler filter 0x%08X:", filter);
            if (warnings & GPU_SAMPLER_UNKNOWN_MIN)
                fprintf(stderr, " unknown minification filter, using point;");
            if (warnings & GPU_SAMPLER_UNKNOWN_MAG)
                fprintf(stderr, " unknown magnification filter, using point;");
            if (warnings & GPU_SAMPLER_CONVOLUTION)
                fprintf(stderr, " convolution approximated by linear filtering;");
            if (warnings & GPU_SAMPLER_BIAS_CLAMPED)
                fprintf(stderr, " LOD bias clamped to the D3D11 limit;");
            if (warnings & GPU_SAMPLER_LOD_RANGE)
                fprintf(stderr, " inverted LOD clamps collapsed to the maximum;");
            fprintf(stderr, "%s\n", reports == 16 ? " further reports suppressed" : "");
        }
    }
    for (i = 0; i < s_nsamp; i++)
        if (!memcmp(&s_samp[i].key, &sd, sizeof sd))
            return s_samp[i].ss;
    if (s_nsamp == (s_tex_mips ? MAX_SAMP : MAX_STATE)) {
        static int reported;
        if (!reported++)
            fprintf(stderr, "[GPU-D3D11] sampler cache is full; cannot create sampler\n");
        return NULL;
    }
    if (gpu_call_failed(ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samp[s_nsamp].ss),
                        "creating sampler state"))
        return NULL;
    s_samp[s_nsamp].key = sd;
    if (sd.AddressU == D3D11_TEXTURE_ADDRESS_BORDER || sd.AddressV == D3D11_TEXTURE_ADDRESS_BORDER) {
        static unsigned border_reports;
        if (border_reports++ < 8)
            fprintf(stderr, "[GPU-D3D11] border sampler: address %u/%u colour 0x%08X\n",
                    addr_u, addr_v, border);
    }
    return s_samp[s_nsamp++].ss;
}

static ID3D11SamplerState *get_required_sampler(uint32_t addr_u, uint32_t addr_v,
                                                uint32_t filter, uint32_t control0,
                                                unsigned levels,
                                                uint32_t border_src, uint32_t border)
{
    ID3D11SamplerState *state = get_sampler(addr_u, addr_v, filter, control0, levels,
                                            border_src, border);
    if (!state) {
        fprintf(stderr, "[GPU-D3D11] cannot create sampler state; refusing to"
                        " draw with a missing sampler, stopping\n");
        fflush(stderr);
        abort();
    }
    return state;
}

/* The zeta format's largest value (SURFACE_FORMAT bits 4-7: 1 Z16, 2
 * Z24S8). The CPU path compares the title's screen z with the raw clear
 * value in this range and ignores SET_CLIP_MIN/MAX, so depth is z / zmax. */
static float zeta_max(void)
{
    return ((nv2a_backend_reg(0x0208) >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
}

static void depth_consts(const Nv2aRenderState *rs, float out[4])
{
    (void)rs;
    out[0] = 0.0f;
    out[1] = 1.0f / zeta_max();
    out[2] = 0.0f;              /* no vertex clamp: D3D clamps per pixel to 0..1 */
    out[3] = 0.0f;
}

static void blend_factor(const Nv2aRenderState *rs, float bf[4])
{
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 ||
        rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];
}

static void set_alpha_test(const Nv2aRenderState *rs, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (rs->alpha_test_enable) {
        out[0] = (float)(rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                         ? rs->alpha_func - 0x200 : 7);
        out[1] = (float)(rs->alpha_ref & 0xFF);
        out[2] = 1.0f;
    }
}

/* ---- vertex ring ----------------------------------------------------------- */

static ID3D11Buffer *s_vring;
static UINT s_vring_size, s_vring_pos;

/* The vertex ring, at least `bytes` large. */
static int ring_ensure(UINT bytes)
{
    D3D11_BUFFER_DESC bd;
    UINT want;

    if (s_vring && bytes <= s_vring_size)
        return 1;
    want = bytes * 4 > (32u << 20) ? bytes * 4 : (32u << 20);
    if (s_vring) ID3D11Buffer_Release(s_vring);
    s_vring = NULL;
    s_vring_size = 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = want;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vring),
                        "creating vertex ring"))
        return 0;
    s_vring_size = want;
    s_vring_pos = want;                 /* the first map discards */
    return 1;
}

static UINT ring_push(const void *data, UINT bytes, UINT align)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT off;

    if (!bytes || !ring_ensure(bytes))
        return ~0u;
    off = (s_vring_pos + align - 1) / align * align;
    if (off > s_vring_size || s_vring_size - off < bytes) {
        mode = D3D11_MAP_WRITE_DISCARD;
        off = 0;
    }
    if (gpu_call_failed(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vring,
                                                0, mode, 0, &m), "mapping vertex ring"))
        return ~0u;
    memcpy((uint8_t *)m.pData + off, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vring, 0);
    s_vring_pos = off + bytes;
    return off;
}

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = ((c >> 24) & 0xFF) / 255.0f;
}

static const char s_clear_hlsl[] =
"cbuffer C : register(b2) { float4 color; float4 depth; };\n"
"float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, depth.x, 1.0);\n"
"}\n"
"float4 ps_main() : SV_Target { return color; }\n";

typedef struct {
    float color[4], depth[4];
} ClearConsts;

static ID3D11VertexShader *s_clear_vs;
static ID3D11PixelShader *s_clear_ps;
static ID3D11Buffer *s_clear_cb;
static ID3D11BlendState *s_clear_blend[16];
static ID3D11DepthStencilState *s_clear_ds[4];

static void clear_error(const char *operation, HRESULT hr)
{
    gpu_d3d11_check_device(s_dev, hr, operation);
    fprintf(stderr, "[GPU-D3D11] %s failed 0x%08lX (device removed reason 0x%08lX);"
                    " cannot safely clear the surface\n",
            operation, (unsigned long)hr,
            (unsigned long)ID3D11Device_GetDeviceRemovedReason(s_dev));
    fflush(stderr);
    abort();
}

static void clear_resources(void)
{
    D3D11_BUFFER_DESC bd;
    ID3DBlob *vs = NULL, *ps = NULL;
    HRESULT hr;

    if (s_clear_vs)
        return;
    if (!compile(s_clear_hlsl, sizeof s_clear_hlsl - 1, "clear", "vs_main", "vs_5_0", &vs) ||
        !compile(s_clear_hlsl, sizeof s_clear_hlsl - 1, "clear", "ps_main", "ps_5_0", &ps))
        clear_error("compile rectangular clear shaders", E_FAIL);
    hr = ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vs),
                                         ID3D10Blob_GetBufferSize(vs), NULL, &s_clear_vs);
    if (FAILED(hr))
        clear_error("CreateVertexShader(clear)", hr);
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(ps),
                                        ID3D10Blob_GetBufferSize(ps), NULL, &s_clear_ps);
    if (FAILED(hr))
        clear_error("CreatePixelShader(clear)", hr);
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(ClearConsts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_clear_cb);
    if (FAILED(hr))
        clear_error("CreateBuffer(clear constants)", hr);
}

static ID3D11BlendState *clear_blend(unsigned mask)
{
    D3D11_BLEND_DESC bd;
    D3D11_RENDER_TARGET_BLEND_DESC *target;
    HRESULT hr;

    if (s_clear_blend[mask])
        return s_clear_blend[mask];
    memset(&bd, 0, sizeof bd);
    target = &bd.RenderTarget[0];
    target->SrcBlend = D3D11_BLEND_ONE;
    target->DestBlend = D3D11_BLEND_ZERO;
    target->BlendOp = D3D11_BLEND_OP_ADD;
    target->SrcBlendAlpha = D3D11_BLEND_ONE;
    target->DestBlendAlpha = D3D11_BLEND_ZERO;
    target->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target->RenderTargetWriteMask = (UINT8)mask;
    hr = ID3D11Device_CreateBlendState(s_dev, &bd, &s_clear_blend[mask]);
    if (FAILED(hr))
        clear_error("CreateBlendState(clear)", hr);
    return s_clear_blend[mask];
}

static ID3D11DepthStencilState *clear_depth_stencil(unsigned flags)
{
    D3D11_DEPTH_STENCIL_DESC dd;
    HRESULT hr;

    if (s_clear_ds[flags])
        return s_clear_ds[flags];
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (flags & 1) != 0;
    dd.DepthWriteMask = (flags & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = (flags & 2) != 0;
    dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
    dd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.BackFace = dd.FrontFace;
    hr = ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_clear_ds[flags]);
    if (FAILED(hr))
        clear_error("CreateDepthStencilState(clear)", hr);
    return s_clear_ds[flags];
}

static void clear_draw(const D3D11_RECT *rect, uint32_t flags, uint32_t argb,
                       uint32_t zstencil, float z)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    ClearConsts c;
    ID3D11BlendState *blend;
    ID3D11DepthStencilState *ds;
    HRESULT hr;
    int counting;

    clear_resources();
    blend = clear_blend((flags >> 4) & 0xF);
    ds = clear_depth_stencil(flags & 3);
    memset(&c, 0, sizeof c);
    unpack_argb(argb, c.color);
    c.depth[0] = z;
    hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_clear_cb, 0,
                                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
        clear_error("Map(clear constants)", hr);
    memcpy(mapped.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_clear_cb, 0);

    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_clear_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 2, 1, &s_clear_cb);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    s_fragment.valid = 0;
    ID3D11DeviceContext_PSSetShader(s_ctx, s_clear_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 2, 1, &s_clear_cb);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, rect);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds, (UINT8)zstencil);
    counting = query_suspend_clear();
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    query_resume_clear(counting);
}

/* ---- the back end ---------------------------------------------------------- */

static void d3d_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                      uint32_t argb, uint32_t zstencil)
{
    uint32_t depth_flags = flags & 3;
    uint32_t color_flags = flags & 0xF0;
    D3D11_RECT rect;
    float c[4], z;
    int full_color, full_depth;

    if (!begin(s, rs, flags))
        return;
    surface_clear_rect(s, s_cur, &rect);
    s_st.clears++;
    if (rtlog_on())
        fprintf(stderr, "[RTLOG] g%u clear %08X flags %X argb %08X zstencil %08X (zmax %.0f) rect %ld,%ld-%ld,%ld of %ux%u\n", s_gen,
                s_cur->va, flags, argb, zstencil, zeta_max(), rect.left, rect.top, rect.right, rect.bottom, s_cur->pw, s_cur->ph);
    if (rect.left >= rect.right || rect.top >= rect.bottom)
        return;
    full_color = rect.left == 0 && rect.top == 0 &&
                 (uint32_t)rect.right == s_cur->pw && (uint32_t)rect.bottom == s_cur->ph;
    full_depth = s_cur_depth && rect.left == 0 && rect.top == 0 &&
                 (uint32_t)rect.right == s_cur_depth->w &&
                 (uint32_t)rect.bottom == s_cur_depth->h;
    z = zeta_max() == 65535.0f ? (zstencil & 0xFFFFu) / 65535.0f
                               : (zstencil >> 8) / 16777215.0f;
    if ((flags & 0xF0) == 0xF0 && full_color) {
        unpack_argb(argb, c);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx,
            (s_cur->msaa_rtv && s_cur->msaa_dirty) ? s_cur->msaa_rtv : s_cur->rtv, c);
        flags &= ~0xF0u;
    }
    if ((flags & 3) && full_depth) {
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx,
            (s_cur && s_cur->msaa_rtv && s_cur->msaa_dirty && s_cur_depth->msaa_dsv)
                ? s_cur_depth->msaa_dsv : s_cur_depth->dsv, f, z,
                                                  (UINT8)(zstencil & 0xFF));
        s_cur_depth->initialized |= flags & 3;
        flags &= ~3u;
    }
    if (!s_cur_depth) {
        if ((flags & 3) && rs && rs->zeta_va)
            fprintf(stderr, "[GPU-D3D11] cannot clear unavailable depth surface %08X\n",
                    rs->zeta_va);
        flags &= ~3u;
    }
    if (flags & 0xF3)
        clear_draw(&rect, flags, argb, zstencil, z);
    if (s_cur_depth && depth_flags)
        depth_written(s_cur_depth);
    if (color_flags) {
        if (color_flags == 0xF0 && full_color) {
            s_cur->alias_uncertain = 0;
            s_cur->wserial = ++s_wserial; s_cur->write_gen = s_gen; note_depth_use();
            if (!mirror_full_color_clear(s_cur, argb)) {
                s_cur->gpu_dirty = 1; s_cur->wserial = ++s_wserial; s_cur->write_gen = s_gen; note_depth_use();
                s_cur->guest_exact = 0;
                s_cur->alias_uncertain = 1;
            }
        } else {
            s_cur->gpu_dirty = 1; s_cur->wserial = ++s_wserial; s_cur->write_gen = s_gen; note_depth_use();
            s_cur->guest_exact = 0;
        }
    }
}

/* The vertex shader's depth-range clip (zinfo in GpuVshConsts): SET_CLIP_MIN/MAX
 * applied when SET_ZMIN_MAX_CONTROL's ZCLAMP_EN field is CULL (bits 4-7 zero),
 * in screen w when SET_CONTROL0 bit 16 (Z_PERSPECTIVE_ENABLE, the w-buffer) is
 * set and in screen z otherwise -- the unit the title depth-tests in, as on the
 * NV2A and in xemu. Halo 2's cinematic depth of field draws its far scene with
 * min = the focal depth and its near layer (80C3F000) with max = the focal
 * depth; without the clip the near layer also held the whole room, which hid
 * the far scene's marines and drone. CLAMP mode leaves geometry as before.
 * RECOMP_GPU_ZCLIP=0 turns it off. */
static void zclip_info(float out[4])
{
    static int zclip_on = -1, wbuf_on = -1;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (zclip_on < 0) {
        const char *e = getenv("RECOMP_GPU_ZCLIP");
        zclip_on = !(e && e[0] == '0');
        e = getenv("RECOMP_GPU_WBUFFER");
        wbuf_on = !(e && e[0] == '0');
    }
    /* SET_CONTROL0 Z_PERSPECTIVE_ENABLE: the w-buffer stores w (which the
     * title scales to depth units) instead of z, as xemu does; titles that
     * read depth back as a colour texture (Halo 2's patchy fog) expect it.
     * RECOMP_GPU_WBUFFER=0 keeps z. */
    static unsigned wpoll;
    static int woff;
    if (wbuf_on && !toggle_off("wbuffer_off", &wpoll, &woff) &&
        (nv2a_backend_reg(0x0290) & (1u << 16)))
        out[3] = 1.0f;
    if (zclip_on && ((nv2a_backend_reg(0x1D78) >> 4) & 0xF) == 0) {
        uint32_t umin = nv2a_backend_reg(0x0394), umax = nv2a_backend_reg(0x0398);
        float fmin, fmax;
        memcpy(&fmin, &umin, sizeof fmin);
        memcpy(&fmax, &umax, sizeof fmax);
        if (fmax > fmin) {
            out[0] = fmin;
            out[1] = fmax;
            out[2] = 1.0f;
        }
    }
}

static int set_window_clip(const Nv2aSurface *s)
{
    static struct {
        uint32_t rect[NV2A_WINDOW_CLIP_COUNT][4];
        uint32_t info[4];
    } last;
    static int have_last;
    struct {
        uint32_t rect[NV2A_WINDOW_CLIP_COUNT][4];
        uint32_t info[4];
    } c;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned i;

    memset(&c, 0, sizeof c);
    if (s->window_clip_enabled && s->window_clip.axes == 3) {
        uint32_t sx = s->aa_sx > 1 ? 2 : 1, sy = s->aa_sy > 1 ? 2 : 1;
        c.info[0] = s->window_clip.uniform ? 1 : NV2A_WINDOW_CLIP_COUNT;
        c.info[1] = s->window_clip.exclusive;
        for (i = 0; i < c.info[0]; i++) {
            uint32_t h = s->window_clip.horizontal[i];
            uint32_t v = s->window_clip.vertical[i];
            uint32_t x0 = (h & 0xFFFu) * sx;
            uint32_t x1 = (((h >> 16) & 0xFFFu) + 1) * sx;
            uint32_t y0 = (v & 0xFFFu) * sy;
            uint32_t y1 = (((v >> 16) & 0xFFFu) + 1) * sy;
            c.rect[i][0] = (uint32_t)scale_edge(x0, s_cur->w, s_cur->pw);
            c.rect[i][1] = (uint32_t)scale_edge(x1, s_cur->w, s_cur->pw) - 1;
            c.rect[i][2] = (uint32_t)scale_edge(y0, s_cur->h, s_cur->ph);
            c.rect[i][3] = (uint32_t)scale_edge(y1, s_cur->h, s_cur->ph) - 1;
        }
    }
    if (!have_last || memcmp(&last, &c, sizeof c)) {
        HRESULT hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_window_cb, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (FAILED(hr)) {
            gpu_d3d11_check_device(s_dev, hr, "mapping window clip constants");
            fprintf(stderr, "[GPU-D3D11] window clip constants map failed 0x%08lX\n",
                    (unsigned long)hr);
            return 0;
        }
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_window_cb, 0);
        memcpy(&last, &c, sizeof c);
        have_last = 1;
    }
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_window_cb);
    return 1;
}

static void d3d_draw_impl(const Nv2aSurface *s, const Nv2aBatch *b);
static void d3d_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    RPZ_BEGIN(rpz_t);
    d3d_draw_impl(s, b);
    RPZ_END(RPZ_DRAW, rpz_t);
}
static void d3d_draw_impl(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs;
    D3D11_MAPPED_SUBRESOURCE m;
    BaseConsts c;
    UINT stride = sizeof(Nv2aVertex), offs;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *ss = NULL;
    ID3D11DepthStencilState *ds;
    float bf[4], tw = 1.0f, th = 1.0f;
    unsigned mip_levels = 1;

    if (!b || !b->count || !b->state || !begin(s, b->state, 0) ||
        !sync_guest_surface(s_cur, 0) || !set_window_clip(s))
        return;
    rs = b->state;
    if (b->texture) {
        if (s_depth_texture && b->texture->color == 0x2E) {
            uint32_t mode = nv2a_backend_reg(0x1E70) & 0x1F;
            int cube = (nv2a_backend_reg(0x1B04) & 4) != 0;
            if (cube || mode != 1) {
                reject_depth_mode(mode, cube, 1);
                return;
            }
        }
        srv = get_texture(b->texture, 0, 0, &tw, &th, &mip_levels);
        if (!srv && required_depth_alias(b->texture, 0))
            return;
        if (srv)
            ss = get_required_sampler(b->texture->addr_u, b->texture->addr_v,
                                      b->texture->filter,
                                      tex_lod_control(nv2a_backend_reg(0x1B0C), s_tex_lod_extra),
                                      mip_levels, b->texture->border_src,
                                      b->texture->border);
    }
    memset(&c, 0, sizeof c);
    c.surf[0] = (float)s_cur->w; c.surf[1] = (float)s_cur->h;
    c.surf[2] = 1.0f / s_cur->w; c.surf[3] = 1.0f / s_cur->h;
    if (srv) {
        c.tex[0] = 1.0f / tw;
        c.tex[1] = 1.0f / th;
        c.tex[2] = 1.0f;
        if (b->texture->color == 0x2E && s_depth_texture)
            c.tex[3] = (float)((nv2a_backend_reg(0x1E6C) & 7) + 1);
    }
    set_alpha_test(rs, c.alpha);
    depth_consts(rs, c.depth);
    if (gpu_call_failed(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_base_cb, 0,
                                                D3D11_MAP_WRITE_DISCARD, 0, &m),
                        "mapping base constants"))
        return;
    memcpy(m.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_base_cb, 0);
    offs = ring_push(b->vertices, b->count * stride, stride);
    if (offs == ~0u)
        return;
    ds = get_required_ds(rs, s_cur_depth != NULL);
    blend_factor(rs, bf);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_base_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring, &stride, &offs);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_base_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_base_cb);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    s_fragment.valid = 0;
    ID3D11DeviceContext_PSSetShader(s_ctx, s_base_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_base_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &ss);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, get_required_blend(rs), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds, rs->stencil_ref & 0xFF);
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    s_cur->gpu_dirty = 1; s_cur->wserial = ++s_wserial; s_cur->write_gen = s_gen; note_depth_use();
    s_cur->guest_exact = 0;
    if (s_cur_depth && depth_state_can_write(rs))
        depth_written(s_cur_depth);
    s_st.draws++;
    s_st.tris += b->count / 3;
}

/* Without programmed combiners the CPU path draws stage 0 times diffuse; a
 * one-stage combiner computing T0 * D0 (or D0 alone) is the same thing. */
static void simple_combiner(const Nv2aXBatch *b, Nv2aCombiner *rc)
{
    memset(rc, 0, sizeof *rc);
    rc->control = 1;
    rc->stage_program = b->tex_valid[0] ? 1 : 0;
    /* A = T0 (reg 8) or 1 (reg 0 inverted), B = D0 (reg 4); out to R12
     * (spare0), which the final combiner absent reads as the result. */
    rc->color_icw[0] = (b->tex_valid[0] ? 0x08u : 0x20u) << 24 | 0x04u << 16;
    rc->alpha_icw[0] = (b->tex_valid[0] ? 0x18u : 0x30u) << 24 | 0x14u << 16;
    rc->color_ocw[0] = 0x0Cu << 4;
    rc->alpha_ocw[0] = 0x0Cu << 4;
}

/* The rasteriser state for a batch: no culling, unless the batch asks the
 * back end to cull as the CPU path would (draw_vsh with RECOMP_CULL_FACE).
 * NULL when both faces are culled. The executor's surface space has y
 * down, where D3D judges winding too; a positive area is clockwise. */
static int s_rs_bias_active;    /* batch_rs chose a decal-biased state */
static int s_prep_vsh;          /* xdraw_prepare is for a GPU vertex-program draw */
static ID3D11RasterizerState *batch_rs(const Nv2aXBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    static unsigned poll;
    static int bias_on = 1, file_init;
    static const char *bias_file;
    int bias = 0;
    if (!file_init) {
        bias_file = getenv("RECOMP_GPU_DECAL_BIAS_FILE");
        file_init = 1;
    }
    if (s_rs_bias) {
        if (bias_file && (poll++ & 1023u) == 0) {
            int now = GetFileAttributesA(bias_file) != INVALID_FILE_ATTRIBUTES;
            if (now != bias_on)
                fprintf(stderr, "[DECALBIAS] %s\n", now ? "on" : "off");
            bias_on = now;
        }
        static unsigned bpoll;
        static int boff;
        bias = bias_on && !toggle_off("bias_off", &bpoll, &boff) && rs->depth_test_enable && rs->depth_func == 0x203 &&
               !rs->depth_write && rs->blend_enable;
    }
    s_rs_bias_active = bias;
    if (!b->cull || !rs->cull_enable)
        return bias ? s_rs_bias : s_rs;
    if (rs->cull_face == 0x408u)
        return NULL;
    return (bias ? s_rs_cull_bias : s_rs_cull)[rs->cull_face != 0x404u][rs->front_face == 0x901u];
}

static GpuRcConsts s_xlast;         /* the last batch's constants, for vsh_readback */
static int s_xlast_valid;

static void bind_fragment_pipeline(ID3D11PixelShader *ps, ID3D11RasterizerState *raster,
                                   ID3D11BlendState *blend, ID3D11DepthStencilState *depth,
                                   UINT stencil, const float factor[4],
                                   ID3D11SamplerState *const samplers[4])
{
    if (!s_fragment.valid || s_fragment.ps != ps)
        ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    if (!s_fragment.valid)
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_xcb);
    if (!s_fragment.valid || memcmp(s_fragment.samplers, samplers, sizeof s_fragment.samplers))
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, samplers);
    if (!s_fragment.valid || s_fragment.raster != raster)
        ID3D11DeviceContext_RSSetState(s_ctx, raster);
    if (!s_fragment.valid || s_fragment.blend != blend ||
        memcmp(s_fragment.factor, factor, sizeof s_fragment.factor))
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend, factor, UINT_MAX);
    if (!s_fragment.valid || s_fragment.depth != depth || s_fragment.stencil != stencil)
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth, stencil);

    s_fragment.ps = ps;
    s_fragment.raster = raster;
    s_fragment.blend = blend;
    s_fragment.depth = depth;
    s_fragment.stencil = stencil;
    memcpy(s_fragment.samplers, samplers, sizeof s_fragment.samplers);
    memcpy(s_fragment.factor, factor, sizeof s_fragment.factor);
    s_fragment.valid = 1;
}

/* Everything a transformed batch needs but its vertices: targets, combiner
 * shader, textures, samplers and output-merger state. 1 ready, 0 failed, -1
 * nothing to draw (both faces culled). */
static int xdraw_prepare_impl(const Nv2aSurface *s, const Nv2aXBatch *b);
static int xdraw_prepare(const Nv2aSurface *s, const Nv2aXBatch *b)
{
    RPZ_BEGIN(rpz_t);
    int r = xdraw_prepare_impl(s, b);
    RPZ_END(RPZ_DRAW_PIXEL, rpz_t);
#ifdef RECOMP_DEBUG_TOOLS
    if (r > 0)
        dl_outcome("drawn", NULL);
    else if (r < 0)
        dl_outcome("culled", "both faces culled");
    else
        dl_outcome("declined", "prepare failed");
#endif
    return r;
}
static int xdraw_prepare_impl(const Nv2aSurface *s, const Nv2aXBatch *b)
{
    const Nv2aRenderState *rs;
    const Nv2aCombiner *rc;
    Nv2aCombiner simple;
    GpuRcKey key;
    GpuRcConsts c;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11PixelShader *ps;
    ID3D11RasterizerState *rst;
    ID3D11DepthStencilState *ds;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *ss[4] = { NULL, NULL, NULL, NULL };
    float bf[4];
    int st;

    s_xprep_texrej = 0;
    if (!b || !b->state || !s || !s->color_va) {
        s_xrej[0]++;
#ifdef RECOMP_DEBUG_TOOLS
        debug_note("declined", "no_color_surface", s ? s->color_va : 0, 0, 0, 0, 0);
#endif
        return 0;
    }
    if (!begin(s, b->state, 0)) {
        s_xrej[1]++;
#ifdef RECOMP_DEBUG_TOOLS
        debug_note("declined", "begin_failed", s->color_va, 0, 0, 0, 1);
#endif
        return 0;
    }
    if (!sync_guest_surface(s_cur, 0) || !set_window_clip(s)) {
        s_xrej[2]++;
#ifdef RECOMP_DEBUG_TOOLS
        debug_note("declined", "guest_sync_clip", s->color_va, 0, 0, 0, 2);
#endif
        return 0;
    }
    rs = b->state;
    rst = batch_rs(b);
    if (!rst)
        return -1;
    rc = b->rc;
    if (!rc) {
        simple_combiner(b, &simple);
        rc = &simple;
    }
    memset(&key, 0, sizeof key);
    memcpy(key.color_icw, rc->color_icw, sizeof key.color_icw);
    memcpy(key.alpha_icw, rc->alpha_icw, sizeof key.alpha_icw);
    memcpy(key.color_ocw, rc->color_ocw, sizeof key.color_ocw);
    memcpy(key.alpha_ocw, rc->alpha_ocw, sizeof key.alpha_ocw);
    key.final0 = rc->final0;
    key.final1 = rc->final1;
    key.control = rc->control;
    key.stage_program = rc->stage_program;
    key.clip_plane_mode = b->clip_plane_mode;
    if (s_prep_vsh) {
        float zi[4];
        zclip_info(zi);
        key.wbuffer = zi[3] > 0.5f;
    }
    memset(&c, 0, sizeof c);
    {
        /* Bump-env / dot-product stages (modes 6-18) read the dot mapping,
         * the input stage, the bump matrices and the eye vector. Only keyed
         * when used so ordinary shaders keep their cache entries. */
        int dep = 0;
        for (st = 0; st < 4; st++)
            if (((rc->stage_program >> (st * 5)) & 0x1F) >= 6)
                dep = 1;
        if (dep) {
            key.dot_mapping = nv2a_backend_reg(0x1E74) & 0xFFF;
            key.other_input = nv2a_backend_reg(0x1E78) & 0x00FF0000u;
            for (st = 0; st < 4; st++) {
                uint32_t r[6];
                int j;
                for (j = 0; j < 4; j++)
                    r[j] = nv2a_backend_reg(0x1B38 + st * 0x40 + j * 4);
                r[4] = nv2a_backend_reg(0x1B48 + st * 0x40);
                r[5] = nv2a_backend_reg(0x1B4C + st * 0x40);
                memcpy(c.bumpmat[st], r, 16);
                memcpy(&c.bumplum[st][0], &r[4], 4);
                memcpy(&c.bumplum[st][1], &r[5], 4);
            }
            {
                uint32_t e[3] = { nv2a_backend_reg(0x181C), nv2a_backend_reg(0x1820),
                                  nv2a_backend_reg(0x1824) };
                memcpy(c.eyevec, e, sizeof e);
            }
            s_st.texmode_dep++;
        }
    }
    uint64_t texture_started = g_bruteforce_perf_counters_enabled ? bruteforce_perf_clock() : 0;
    RPZ_BEGIN(rpz_tex);
    for (st = 0; st < 4; st++) {
        uint32_t mode = (rc->stage_program >> (st * 5)) & 0x1F;
        /* Modes that sample this stage's texture: 2D/3D-projective, cube,
         * bump-env, DOT_ST, the dot-reflect / dot-STR cube lookups and the
         * dependent AR/GB reads (xemu psh.c). */
        int samples = (mode >= 1 && mode <= 3) || mode == 6 || mode == 7 || mode == 9 ||
                      mode == 11 || mode == 12 || mode == 14 || mode == 15 ||
                      mode == 16 || mode == 18;
        int cube_mode = mode == 3 || mode == 11 || mode == 12 || mode == 14 || mode == 18;
        int cube = b->tex_cube[st] && cube_mode;
        float tw = 1.0f, th = 1.0f;
        unsigned mip_levels = 1;
        if (s_depth_texture && b->tex_valid[st] && b->tex[st].color == 0x2E &&
            mode != 0 && (b->tex_cube[st] || (mode != 1 && mode != 2))) {
            reject_depth_mode(mode, b->tex_cube[st], 0);
            return 0;
        }
        if (!b->tex_valid[st] || !samples)
            continue;
        srv[st] = get_texture(&b->tex[st], cube, st, &tw, &th, &mip_levels);
#ifdef RECOMP_DEBUG_TOOLS
        dl_stage(st, srv[st] != NULL, s_tex_fail);
#endif
        if (!srv[st]) {
            if (required_depth_alias(&b->tex[st], cube)) {
                s_xrej[3]++;
                s_xprep_texrej = 1;
#ifdef RECOMP_DEBUG_TOOLS
                debug_note("declined", "texture", b->tex[st].offset,
                           b->tex[st].width, b->tex[st].height, b->tex[st].color, 3);
#endif
                return 0;
            }
            /* Color-texture rejects stay soft: the stage samples the
             * transparent fallback instead of dropping the mesh. */
            s_texture_hard_reject = 0;
#ifdef RECOMP_DEBUG_TOOLS
            debug_note("soft", s_tex_fail ? s_tex_fail : "invalid",
                       b->tex[st].offset, b->tex[st].width, b->tex[st].height,
                       b->tex[st].color, 3);
#endif
            continue;
        }
        key.tex_valid[st] = 1;
        key.tex_normalized[st] = b->tex_normalized[st];
        key.tex_cube[st] = (uint8_t)cube;
        if (s_depth_texture && b->tex[st].color == 0x2E) {
            key.tex_depth[st] = 1;
            key.shadow_func = nv2a_backend_reg(0x1E6C) & 7;
        }
        else if (b->tex[st].color == 0x2E && shadow_alias_lit()) {
            /* A/B (RECOMP_SHADOW_ALIAS=lit): without a GPU depth alias the
             * D24 shadow map samples its colour alias; compare ALWAYS = lit. */
            key.tex_depth[st] = 1;
            key.shadow_func = 7;
        }
        ss[st] = get_required_sampler(b->tex[st].addr_u, b->tex[st].addr_v,
                                      b->tex[st].filter,
                                      tex_lod_control(nv2a_backend_reg(0x1B0C + st * 0x40),
                                                      s_tex_lod_extra), mip_levels,
                                      b->tex[st].border_src, b->tex[st].border);
        c.texinfo[st][0] = 1.0f / tw;
        c.texinfo[st][1] = 1.0f / th;
    }
    if (texture_started)
        s_st.texture_prepare_ticks += bruteforce_perf_clock() - texture_started;
    RPZ_END(RPZ_DRAW_TEX, rpz_tex);
    ps = get_rc_shader(&key);
    if (!ps) {
        s_xrej[4]++;
#ifdef RECOMP_DEBUG_TOOLS
        debug_note("declined", "combiner_shader", s_cur ? s_cur->va : 0, 0, 0, 0, 4);
#endif
        return 0;
    }
    ds = get_required_ds(rs, s_cur_depth != NULL);
    c.surf[0] = (float)s_cur->w; c.surf[1] = (float)s_cur->h;
    c.surf[2] = 1.0f / s_cur->w; c.surf[3] = 1.0f / s_cur->h;
    depth_consts(rs, c.depth);
    if (key.wbuffer && s_rs_bias_active)
        c.depth[3] = 64.0f / 16777215.0f;   /* the raster bias does not reach SV_Depth */
    set_alpha_test(rs, c.alpha);
    c.fogc[0] = b->fogc[0]; c.fogc[1] = b->fogc[1]; c.fogc[2] = b->fogc[2];
    for (st = 0; st < 8; st++) {
        unpack_argb(rc->factor0[st], c.c0[st]);
        unpack_argb(rc->factor1[st], c.c1[st]);
    }
    unpack_argb(rc->final_c0, c.fc0);
    unpack_argb(rc->final_c1, c.fc1);
    if (!s_xlast_valid || memcmp(&s_xlast, &c, sizeof c)) {
        if (gpu_call_failed(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_xcb, 0,
                                                   D3D11_MAP_WRITE_DISCARD, 0, &m),
                            "mapping extended constants"))
            return 0;
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_xcb, 0);
        s_xlast = c;
        s_xlast_valid = 1;
    }
    blend_factor(rs, bf);
    bind_fragment_pipeline(ps, rst, get_required_blend(rs), ds,
                           rs->stencil_ref & 0xFF, bf, ss);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    return 1;
}

static void d3d_xdraw_impl(const Nv2aSurface *s, const Nv2aXBatch *b);
static void d3d_xdraw(const Nv2aSurface *s, const Nv2aXBatch *b)
{
    RPZ_BEGIN(rpz_t);
    d3d_xdraw_impl(s, b);
    RPZ_END(RPZ_DRAW, rpz_t);
}
static void d3d_xdraw_impl(const Nv2aSurface *s, const Nv2aXBatch *b)
{
    UINT stride = sizeof(Nv2aXVertex), offs;

#ifdef RECOMP_DEBUG_TOOLS
    debug_note_set_draw(0, b ? b->count : 0, b);
    dl_begin('X', 0, b ? b->count : 0, s, b);
#endif
    if (!b || !b->count || xdraw_prepare(s, b) <= 0)
        return;
    offs = ring_push(b->vertices, b->count * stride, stride);
    if (offs == ~0u)
        return;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_xlayout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring, &stride, &offs);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_xvs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_xcb);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    s_cur->gpu_dirty = 1; s_cur->wserial = ++s_wserial; s_cur->write_gen = s_gen; note_depth_use();
    s_cur->guest_exact = 0;
    if (s_cur_depth && depth_state_can_write(b->state))
        depth_written(s_cur_depth);
    s_st.draws++;
    s_st.xdraws++;
    s_st.tris += b->count / 3;
}

/* ---- vertex programs on the GPU (draw_vsh) ----------------------------------- */

#define MAX_VSH 1024
#define VSH_MAX_INSTR 136
#define VSH_RAW_IL 8
typedef struct {
    uint64_t key;
    D3D11_INPUT_ELEMENT_DESC el[16];
    ID3D11InputLayout *il;
} RawIl;
typedef struct {
    uint64_t hash;
    uint32_t n, mask, nin;
    uint8_t conv[16];                   /* the shader's conversions; 0 = floats */
    RawIl raw[VSH_RAW_IL];              /* layouts for raw streams */
    uint32_t raw_next;
    uint32_t words[VSH_MAX_INSTR][4];
    char *src;                          /* worker input */
    size_t len;
    ID3DBlob *blob;                     /* worker output */
    volatile LONG state;                /* 0 queued, 1 compiled, 2 ready, -1 failed */
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;              /* ATTR0..nin-1 as float4s in one stream */
    void *code;                         /* bytecode, for layouts and readback */
    SIZE_T code_size;
    ID3D11GeometryShader *so;           /* stream-output twin, for readback */
    DWORD queued_tick;
    volatile LONG slow_reported;
} Vsh;

static Vsh *s_vsh;
static volatile LONG s_nvsh, s_vsh_compiles, s_vsh_failures;
static HANDLE s_vsh_wake, s_vsh_thread;
static ID3D11Buffer *s_kcb, *s_iring;
static UINT s_iring_size, s_iring_pos;
static GpuVshConsts s_kshadow;
static int s_kvalid;
static Vsh *s_vlast;                    /* the last draw_vsh, for vsh_readback */
static int s_vlast_culled;
static UINT s_vlast_voff, s_vlast_stride, s_vlast_nverts, s_vlast_ns;
static UINT s_vlast_strides[NV2A_XVS_MAX_STREAMS], s_vlast_offs[NV2A_XVS_MAX_STREAMS];
static int s_vlast_raw;
static ID3D11InputLayout *s_vlast_il;
static uint32_t s_vsh_flags;            /* GPU_VSH_* for every shader */
static int s_vsh_sync;                  /* RECOMP_GPU_VSH_SYNC */
static int s_shader_diag;
static volatile LONG s_shader_diag_reports;

static void shader_diagnostic(const char *format, ...)
{
    LONG count;
    char message[384];
    va_list args;

    if (!s_shader_diag)
        return;
    count = InterlockedCompareExchange(&s_shader_diag_reports, 0, 0);
    for (;;) {
        LONG previous;
        if (count >= 32)
            return;
        previous = InterlockedCompareExchange(&s_shader_diag_reports, count + 1, count);
        if (previous == count)
            break;
        count = previous;
    }
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    fprintf(stderr, "[GPU-SHADER] %s%s\n", message,
            count == 31 ? " (further shader diagnostics suppressed)" : "");
}

static void vsh_note_pending(Vsh *v)
{
    DWORD now;
    if (!s_shader_diag || v->state != 0)
        return;
    now = GetTickCount();
    if ((DWORD)(now - v->queued_tick) >= 2000 &&
        InterlockedCompareExchange(&v->slow_reported, 1, 0) == 0)
        shader_diagnostic("vertex program %016llX still queued or compiling after %lu ms",
                          (unsigned long long)v->hash,
                          (unsigned long)(DWORD)(now - v->queued_tick));
}

static DWORD WINAPI vsh_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        LONG i, n = s_nvsh;
        int did = 0;
        for (i = 0; i < n; i++) {
            Vsh *v = &s_vsh[i];
            ID3DBlob *blob = NULL, *err = NULL;
            DWORD started;
            HRESULT result;
            if (v->state != 0)
                continue;
            started = s_shader_diag ? GetTickCount() : 0;
            shader_diagnostic("compile-start key=%016llX instructions=%u",
                              (unsigned long long)v->hash, v->n);
            result = D3DCompile(v->src, v->len, "vsh", NULL, NULL, "vs_main", "vs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                                0, &blob, &err);
            if (FAILED(result)) {
                if (s_shader_diag)
                    shader_diagnostic("compile-failed key=%016llX result=0x%08lX elapsed=%lu ms",
                                      (unsigned long long)v->hash, (unsigned long)result,
                                      (unsigned long)(DWORD)(GetTickCount() - started));
                if (InterlockedIncrement(&s_vsh_failures) <= 3)
                    fprintf(stderr, "[GPU-D3D11] vertex shader %ld failed: %s\n", (long)i,
                            err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
                if (err)
                    ID3D10Blob_Release(err);
                InterlockedExchange(&v->state, -1);
            } else {
                if (s_shader_diag)
                    shader_diagnostic("compile-finished key=%016llX elapsed=%lu ms",
                                      (unsigned long long)v->hash,
                                      (unsigned long)(DWORD)(GetTickCount() - started));
                if (err)
                    ID3D10Blob_Release(err);
                v->blob = blob;
                InterlockedIncrement(&s_vsh_compiles);
                InterlockedExchange(&v->state, 1);
            }
            did = 1;
        }
        if (!did)
            WaitForSingleObject(s_vsh_wake, INFINITE);
    }
}

static uint64_t prog_hash(const uint32_t (*p)[4], uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < 4; j++)
            h = (h ^ p[i][j]) * 1099511628211ull;
    return h ^ n;
}

/* On the NV2A thread: the compiled shader to a vertex shader and its float
 * input layout. */
static void vsh_finish(Vsh *v)
{
    static const uint8_t no_conv[16];
    D3D11_INPUT_ELEMENT_DESC il[16];
    const void *code = ID3D10Blob_GetBufferPointer(v->blob);
    SIZE_T size = ID3D10Blob_GetBufferSize(v->blob);
    uint32_t k;
    int ok;

    ok = !gpu_call_failed(ID3D11Device_CreateVertexShader(s_dev, code, size, NULL, &v->vs),
                          "creating vertex-program shader");
    if (ok && v->nin && !memcmp(v->conv, no_conv, sizeof no_conv)) {
        for (k = 0; k < v->nin; k++) {
            il[k].SemanticName = "ATTR";
            il[k].SemanticIndex = k;
            il[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            il[k].InputSlot = 0;
            il[k].AlignedByteOffset = 16 * k;
            il[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            il[k].InstanceDataStepRate = 0;
        }
        ok = !gpu_call_failed(ID3D11Device_CreateInputLayout(s_dev, il, v->nin, code, size, &v->il),
                              "creating vertex-program input layout");
    }
    if (ok && (v->code = malloc(size)) != NULL) {
        memcpy(v->code, code, size);
        v->code_size = size;
    }
    ID3D10Blob_Release(v->blob);
    v->blob = NULL;
    free(v->src);
    v->src = NULL;
    v->state = ok ? 2 : -1;
    shader_diagnostic("prepare-%s key=%016llX bytecode=%s",
                      ok ? "ready" : "failed", (unsigned long long)v->hash,
                      v->code ? "retained" : "unavailable");
}

static void vsh_conv_key(const Nv2aXVsBatch *b, uint8_t conv[16])
{
    uint32_t k;
    memset(conv, 0, 16);
    if (!b->nstreams)
        return;
    for (k = 0; k < b->ninputs && k < 16; k++) {
        uint8_t c = b->in[k].conv;
        switch (c >> 4) {
        case NV2A_XVS_COLOR: case NV2A_XVS_UBYTE: case NV2A_XVS_SHORT: case NV2A_XVS_CMP:
        case NV2A_XVS_SHORT_NORM:
            conv[k] = c;
            break;
        default:
            break;
        }
    }
}

/* The ready shader for a batch's program and input formats, or NULL while it
 * compiles (the executor then transforms the batch on the CPU). */
static Vsh *get_vsh(const Nv2aXVsBatch *b)
{
    static char src[96 * 1024];
    static LONG last = -1;
    uint8_t conv[16];
    uint64_t h = prog_hash(b->program, b->ninstr);
    LONG i, n = s_nvsh;
    Vsh *v;

    vsh_conv_key(b, conv);
    for (i = -1; i < n; i++) {
        LONG j = i < 0 ? last : i;
        if (j < 0 || (i >= 0 && i == last))
            continue;
        v = &s_vsh[j];
        if (v->hash != h || v->n != b->ninstr || memcmp(v->conv, conv, sizeof conv)
            || memcmp(v->words, b->program, b->ninstr * sizeof v->words[0]))
            continue;
        last = j;
        if (v->state == 1)
            vsh_finish(v);
        if (v->state == 0)
            vsh_note_pending(v);
        return v->state == 2 ? v : NULL;
    }
    if (n == MAX_VSH || b->ninstr > VSH_MAX_INSTR) {
        shader_diagnostic("vertex program rejected: cache limit or instruction count");
        return NULL;
    }
    if (!s_vsh && !(s_vsh = (Vsh *)calloc(MAX_VSH, sizeof *s_vsh))) {
        shader_diagnostic("vertex program cache allocation failed");
        return NULL;
    }
    if (!s_vsh_thread) {
        if (scale_test_failure("vsh-event")) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            s_vsh_wake = NULL;
        } else {
            s_vsh_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        }
        if (!s_vsh_wake) {
            shader_diagnostic("vertex worker event creation failed: Windows error %lu",
                              (unsigned long)GetLastError());
            return NULL;
        }
        s_vsh_thread = CreateThread(NULL, 0, vsh_worker, NULL, 0, NULL);
        if (!s_vsh_thread) {
            DWORD error = GetLastError();
            CloseHandle(s_vsh_wake);
            s_vsh_wake = NULL;
            shader_diagnostic("vertex worker thread creation failed: Windows error %lu",
                              (unsigned long)error);
            return NULL;
        }
    }
    v = &s_vsh[n];
    memset(v, 0, sizeof *v);
    v->hash = h;
    v->n = b->ninstr;
    v->mask = b->input_mask;
    v->nin = b->ninputs;
    v->queued_tick = s_shader_diag ? GetTickCount() : 0;
    memcpy(v->conv, conv, sizeof conv);
    memcpy(v->words, b->program, b->ninstr * sizeof v->words[0]);
    v->len = gpu_vsh_hlsl(b->program, b->ninstr, b->input_mask, conv, s_vsh_flags,
                          src, sizeof src);
    if (!v->len || !(v->src = (char *)malloc(v->len + 1))) {
        v->state = -1;
        shader_diagnostic("vertex program translation or source allocation failed key=%016llX",
                          (unsigned long long)v->hash);
    } else {
        memcpy(v->src, src, v->len + 1);
        {   /* debug: RECOMP_DEBUG_VSH_SRC=<dir> writes each translated program */
            const char *dd = getenv("RECOMP_DEBUG_VSH_SRC");
            if (dd && *dd) {
                char p[512];
                FILE *df;
                snprintf(p, sizeof p, "%s\\vsh_%016llX.hlsl", dd, (unsigned long long)v->hash);
                if ((df = fopen(p, "wb")) != NULL) {
                    uint32_t wi;
                    fprintf(df, "// %u instructions, input mask %08X\n// words:", v->n, v->mask);
                    for (wi = 0; wi < v->n; ++wi)
                        fprintf(df, "%s %08X %08X %08X %08X", wi % 2 ? "" : "\n//",
                                v->words[wi][0], v->words[wi][1], v->words[wi][2], v->words[wi][3]);
                    fprintf(df, "\n%s", v->src);
                    fclose(df);
                }
            }
        }
        shader_diagnostic("queued key=%016llX instructions=%u",
                          (unsigned long long)v->hash, v->n);
    }
    InterlockedExchange(&s_nvsh, n + 1);
    if (!SetEvent(s_vsh_wake)) {
        shader_diagnostic("vertex worker wake failed: Windows error %lu",
                          (unsigned long)GetLastError());
        InterlockedCompareExchange(&v->state, -1, 0);
    }
    if (s_vsh_sync) {
        /* RECOMP_GPU_VSH_SYNC=1 (tests): wait for the compile rather than
         * handing this batch to the CPU. */
        while (v->state == 0) {
            vsh_note_pending(v);
            Sleep(1);
        }
        if (v->state == 1)
            vsh_finish(v);
        return v->state == 2 ? v : NULL;
    }
    return NULL;
}

static ID3D11GeometryShader *vsh_filter_gs(void)
{
    static ID3D11GeometryShader *gs;
    static int tried;
    ID3DBlob *blob = NULL;

    /* Debug: RECOMP_GPU_GS_FILTER=0 disables the non-finite triangle filter;
     * RECOMP_GPU_GS_FILTER_OFF_FILE=<path> disables it while that file exists
     * (polled every 2048 draws) so it can be toggled live. */
    static int mode = -1;
    static const char *off_file;
    static unsigned poll;
    static int file_off;
    if (mode < 0) {
        const char *e = getenv("RECOMP_GPU_GS_FILTER");
        mode = !(e && e[0] == '0');
        off_file = getenv("RECOMP_GPU_GS_FILTER_OFF_FILE");
    }
    if (!mode)
        return NULL;
    if (off_file && (poll++ & 2047u) == 0) {
        int now = GetFileAttributesA(off_file) != INVALID_FILE_ATTRIBUTES;
        if (now != file_off)
            fprintf(stderr, "[GSFILTER] %s\n", now ? "off" : "on");
        file_off = now;
    }
    if (file_off)
        return NULL;
    if (tried)
        return gs;
    tried = 1;
    if (!compile(gpu_vsh_gs_hlsl, strlen(gpu_vsh_gs_hlsl), "vsh_gs", "gs_main", "gs_5_0", &blob))
        return NULL;
    if (gpu_call_failed(ID3D11Device_CreateGeometryShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                                          ID3D10Blob_GetBufferSize(blob), NULL, &gs),
                        "creating vertex filter shader"))
        gs = NULL;
    ID3D10Blob_Release(blob);
    return gs;
}

/* fetch_attr's integer conversions as a table: x/1023 at x + 1024 for x in
 * -1024..1023, z/511 at z + 2560 for z in -512..511, c/255 at c + 3072. */
static ID3D11ShaderResourceView *vfetch_table(void)
{
    static ID3D11ShaderResourceView *srv;
    static int tried;
    static float t[3328];
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ID3D11Buffer *buf = NULL;
    int i;

    if (tried)
        return srv;
    tried = 1;
    for (i = -1024; i < 1024; i++)
        t[i + 1024] = (float)i / 1023.0f;
    for (i = -512; i < 512; i++)
        t[i + 2560] = (float)i / 511.0f;
    for (i = 0; i < 256; i++)
        t[i + 3072] = (float)i / 255.0f;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof t;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&sd, 0, sizeof sd);
    sd.pSysMem = t;
    if (gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, &sd, &buf),
                        "creating vertex conversion table"))
        return NULL;
    memset(&vd, 0, sizeof vd);
    vd.Format = DXGI_FORMAT_R32_FLOAT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.FirstElement = 0;
    vd.Buffer.NumElements = 3328;
    if (gpu_call_failed(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)buf, &vd, &srv),
                        "creating vertex conversion view"))
        srv = NULL;
    ID3D11Buffer_Release(buf);
    return srv;
}

static DXGI_FORMAT vfetch_format(uint8_t conv)
{
    uint32_t s = conv & 15u;
    switch (conv >> 4) {
    case NV2A_XVS_FLOAT:
        return s == 1 ? DXGI_FORMAT_R32_FLOAT : s == 2 ? DXGI_FORMAT_R32G32_FLOAT
             : s == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    case NV2A_XVS_CONST: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case NV2A_XVS_COLOR: return DXGI_FORMAT_R8G8B8A8_UINT;
    case NV2A_XVS_UBYTE:
        return s == 1 ? DXGI_FORMAT_R8_UINT : s == 2 ? DXGI_FORMAT_R8G8_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
    case NV2A_XVS_SHORT: case NV2A_XVS_SHORT_NORM:
        return s == 1 ? DXGI_FORMAT_R16_SINT : s == 2 ? DXGI_FORMAT_R16G16_SINT
                                                      : DXGI_FORMAT_R16G16B16A16_SINT;
    case NV2A_XVS_CMP:   return DXGI_FORMAT_R32_UINT;
    default:             return DXGI_FORMAT_UNKNOWN;
    }
}

static ID3D11InputLayout *vfetch_layout(Vsh *v, const Nv2aXVsBatch *b)
{
    D3D11_INPUT_ELEMENT_DESC el[16];
    uint64_t key = 1469598103934665603ull;
    uint32_t k, i;
    RawIl *e;

    if (!v->code || !v->nin || v->nin > 16)
        return NULL;
    memset(el, 0, sizeof el);
    for (k = 0; k < v->nin; k++) {
        const Nv2aXVsInput *in = &b->in[k];
        el[k].SemanticName = "ATTR";
        el[k].SemanticIndex = k;
        el[k].Format = vfetch_format(in->conv);
        el[k].InputSlot = in->stream;
        el[k].AlignedByteOffset = in->offset;
        el[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        if (el[k].Format == DXGI_FORMAT_UNKNOWN)
            return NULL;
        key = (key ^ ((uint64_t)el[k].Format | (uint64_t)in->stream << 8
                      | (uint64_t)in->offset << 16)) * 1099511628211ull;
    }
    for (i = 0; i < VSH_RAW_IL; i++) {
        e = &v->raw[i];
        if (e->il && e->key == key && !memcmp(e->el, el, v->nin * sizeof el[0]))
            return e->il;
    }
    e = &v->raw[v->raw_next++ % VSH_RAW_IL];
    if (e->il)
        ID3D11InputLayout_Release(e->il);
    e->il = NULL;
    if (gpu_call_failed(ID3D11Device_CreateInputLayout(s_dev, el, v->nin, v->code,
                                                       v->code_size, &e->il),
                        "creating raw vertex input layout"))
        return NULL;
    e->key = key;
    memcpy(e->el, el, sizeof el);
    return e->il;
}

static int d3d_vsh_raw_ok(void)
{
    return 1;
}

/* Indices: a second dynamic ring, mapped as the vertex ring is. */
static UINT index_push(const uint32_t *data, UINT count)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT bytes = count * 4u, off;

    if (!bytes)
        return ~0u;
    if (!s_iring || bytes > s_iring_size) {
        D3D11_BUFFER_DESC bd;
        UINT want = bytes * 4 > (16u << 20) ? bytes * 4 : (16u << 20);
        if (s_iring) ID3D11Buffer_Release(s_iring);
        s_iring = NULL;
        s_iring_size = 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = want;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_iring),
                            "creating index ring"))
            return ~0u;
        s_iring_size = want;
        s_iring_pos = want;
    }
    off = (s_iring_pos + 3u) & ~3u;
    if (off > s_iring_size || s_iring_size - off < bytes) {
        mode = D3D11_MAP_WRITE_DISCARD;
        off = 0;
    }
    if (gpu_call_failed(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_iring, 0, mode, 0, &m),
                        "mapping index ring"))
        return ~0u;
    memcpy((uint8_t *)m.pData + off, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_iring, 0);
    s_iring_pos = off + bytes;
    return off;
}

/* Several streams into the vertex ring at once, 16-byte aligned each. */
static int vertex_streams_push(const Nv2aXVsBatch *b)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT pos[NV2A_XVS_MAX_STREAMS], total = 0, base, k;

    for (k = 0; k < b->nstreams; k++) {
        total = (total + 15u) & ~15u;
        pos[k] = total;
        total += b->streams[k].bytes;
    }
    if (!ring_ensure(total))
        return 0;
    base = (s_vring_pos + 15u) & ~15u;
    if (base > s_vring_size || s_vring_size - base < total) {
        mode = D3D11_MAP_WRITE_DISCARD;
        base = 0;
    }
    if (gpu_call_failed(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vring, 0, mode, 0, &m),
                        "mapping vertex streams"))
        return 0;
    for (k = 0; k < b->nstreams; k++) {
        memcpy((uint8_t *)m.pData + base + pos[k], b->streams[k].data, b->streams[k].bytes);
        s_vlast_strides[k] = b->streams[k].stride;
        s_vlast_offs[k] = base + pos[k];
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vring, 0);
    s_vring_pos = base + total;
    return 1;
}

static int d3d_submit_vertex_draw(const Nv2aSurface *s, const Nv2aXVsBatch *b)
{
    Vsh *v;
    ID3D11Buffer *cbs[2];
    UINT voff = 0, ioff, stride;
    ID3D11InputLayout *il;
    int r;

    s_vlast = NULL;
    s_vlast_culled = 0;
    if (!b || !b->nindices || !ensure_device())
        return 0;
    if (!(v = get_vsh(b))) {
        s_st.vfall++;
#ifdef RECOMP_DEBUG_TOOLS
        dl_begin('F', 0, b->nindices, s, &b->xb);
        dl_outcome("cpu_fallback", "vertex program not translated");
#endif
        return 0;
    }
#ifdef RECOMP_DEBUG_TOOLS
    debug_note_set_draw(v->hash, b->nindices, &b->xb);
    dl_begin('V', v->hash, b->nindices, s, &b->xb);
#endif
    uint64_t prepare_started = g_bruteforce_perf_counters_enabled ? bruteforce_perf_clock() : 0;
    s_prep_vsh = 1;
    r = xdraw_prepare(s, &b->xb);
    s_prep_vsh = 0;
    if (prepare_started)
        s_st.vertex_prepare_ticks += bruteforce_perf_clock() - prepare_started;
    if (r < 0) {
        s_vlast_culled = 1;
        return 1;               /* both faces culled: nothing to draw */
    }
    if (r == 0) {
        static int cpu_fallback = -1;
        if (cpu_fallback < 0) {
            const char *e = getenv("RECOMP_GPU_TEXREJECT_CPU");
            cpu_fallback = e && e[0] == '1';
        }
        if (s_xprep_texrej && !cpu_fallback && !rpz_ab_off(8)) {
            s_texrej_skips++;
            s_vlast_culled = 1;
#ifdef RECOMP_DEBUG_TOOLS
            dl_outcome("skipped", "texture reject (depth alias)");
#endif
            return 1;           /* the CPU route would be rejected the same way */
        }
        return 0;
    }
    if (!s_kcb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (sizeof(GpuVshConsts) + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_kcb),
                            "creating vertex-program constants"))
            return 0;
    }
    RPZ_BEGIN(rpz_vtx);
    {
        /* The constants: skipped when neither the constant file (by its
         * generation) nor the per-batch values changed. */
        static GpuVshConsts k;
        static uint32_t kgen;
        static int kgen_ok;
        float vsinfo[4], foginfo[4], zinfo[4], wideinfo[4];
        /* Widescreen: a draw into the 640x480 screen with depth testing off
         * may be the 2D layer, which the geometry shader squeezes into the
         * middle 4:3 (gpu_vsh_gs_hlsl). "Off" includes a test that always
         * passes and writes no depth: the motion tracker pastes its 64x64
         * radar picture that way, and was left at the 4:3 spot (to the left
         * of its frame) before. */
        wideinfo[0] = s_ui_squeeze;
        wideinfo[1] = s_ui_squeeze < 0.999f && s_cur && s_cur->w == 640 && s_cur->h == 480 &&
                      b->xb.state && (!b->xb.state->depth_test_enable ||
                                      (b->xb.state->depth_func == 0x207 && !b->xb.state->depth_write))
                      ? 1.0f : 0.0f;
        wideinfo[2] = wideinfo[3] = 0.0f;
        vsinfo[0] = b->aa[0]; vsinfo[1] = b->aa[1];
        vsinfo[2] = b->xb.near_clip ? 1.0f : 0.0f; vsinfo[3] = b->xb.near_z;
        foginfo[0] = b->fog_enable ? 1.0f : 0.0f;
        foginfo[1] = (float)b->fog_mode;
        foginfo[2] = b->fog_param[0]; foginfo[3] = b->fog_param[1];
        zclip_info(zinfo);
        if (s_kvalid && kgen_ok && b->consts_gen == kgen
                && !memcmp(vsinfo, s_kshadow.vsinfo, sizeof vsinfo)
                && !memcmp(foginfo, s_kshadow.foginfo, sizeof foginfo)
                && !memcmp(zinfo, s_kshadow.zinfo, sizeof zinfo)
                && !memcmp(wideinfo, s_kshadow.wideinfo, sizeof wideinfo)) {
            s_st.kskip++;
        } else {
            memcpy(k.k, b->consts, sizeof k.k);
            memcpy(k.vsinfo, vsinfo, sizeof vsinfo);
            memcpy(k.foginfo, foginfo, sizeof foginfo);
            memcpy(k.zinfo, zinfo, sizeof zinfo);
            memcpy(k.wideinfo, wideinfo, sizeof wideinfo);
            if (!s_kvalid || memcmp(&k, &s_kshadow, sizeof k)) {
                D3D11_MAPPED_SUBRESOURCE m;
                if (gpu_call_failed(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_kcb, 0,
                                                            D3D11_MAP_WRITE_DISCARD, 0, &m),
                                    "mapping vertex-program constants"))
                    return 0;
                memcpy(m.pData, &k, sizeof k);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_kcb, 0);
                s_kshadow = k;
                s_kvalid = 1;
                s_st.kup++;
            }
            kgen = b->consts_gen;
            kgen_ok = 1;
        }
    }
    stride = 16u * v->nin;
    il = v->il;
    if (b->nstreams && v->nin) {
        ID3D11Buffer *bufs[NV2A_XVS_MAX_STREAMS];
        static int conv_bound;
        UINT k;
        if (b->nstreams > NV2A_XVS_MAX_STREAMS || !(il = vfetch_layout(v, b)))
            return 0;
        if (!vertex_streams_push(b))
            return 0;
        for (k = 0; k < b->nstreams; k++)
            bufs[k] = s_vring;
        if (!conv_bound) {
            ID3D11ShaderResourceView *t = vfetch_table();
            if (!t)
                return 0;
            ID3D11DeviceContext_VSSetShaderResources(s_ctx, 0, 1, &t);
            conv_bound = 1;
        }
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, b->nstreams, bufs, s_vlast_strides,
                                               s_vlast_offs);
        s_vlast_ns = b->nstreams;
        s_vlast_il = il;
        s_vlast_raw = 1;
        s_st.vraw++;
    } else {
        if (v->nin) {
            voff = ring_push(b->inputs, b->nverts * stride, stride);
            if (voff == ~0u)
                return 0;
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring, &stride, &voff);
        }
        s_vlast_raw = 0;
    }
    ioff = index_push(b->indices, b->nindices);
    RPZ_END(RPZ_DRAW_VERTEX, rpz_vtx);
    if (ioff == ~0u)
        return 0;
    cbs[0] = s_xcb;
    cbs[1] = s_kcb;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_iring, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShader(s_ctx, v->vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 2, cbs);
    ID3D11DeviceContext_GSSetShader(s_ctx, vsh_filter_gs(), NULL, 0);
    ID3D11DeviceContext_GSSetConstantBuffers(s_ctx, 1, 1, &cbs[1]);   /* wideinfo */
    if (zeq_enabled() && s_probe_armed && s_gen >= s_probe_armed && s_cur_depth && b->xb.state && b->xb.state->depth_test_enable &&
        b->xb.state->depth_func == 0x202)
        zeq_measure(b->xb.state, b->nindices, ioff / 4u);
    if (s_probe_armed && s_gen >= s_probe_armed && getenv("RECOMP_DEBUG_PROBE_SRV"))
        probe_srv(s_dl_seq ? s_dl_seq - 1 : 0);
    if (s_probe_armed && s_gen >= s_probe_armed && getenv("RECOMP_DEBUG_PROBE_SAMPLES")) {
        /* debug: samples each draw in the probe window writes */
        ID3D11Query *pq = query_create(D3D11_QUERY_OCCLUSION);
        UINT64 pv = 0;
        if (pq) {
            ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)pq);
            ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)pq);
            while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)pq, &pv, sizeof pv, 0) == S_FALSE)
                ;
            ID3D11Query_Release(pq);
            fprintf(stderr, "[PROBE] frame %u draw %u rt %08X: %llu samples" " (%u indices)\n",
                    s_gen, s_dl_seq ? s_dl_seq - 1 : 0, s_cur ? s_cur->va : 0,
                    (unsigned long long)pv, b->nindices);
        } else {
            ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
        }
    } else
    ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
    s_cur->gpu_dirty = 1; s_cur->wserial = ++s_wserial; s_cur->write_gen = s_gen; note_depth_use();
    s_cur->guest_exact = 0;
    if (s_cur_depth && depth_state_can_write(b->xb.state))
        depth_written(s_cur_depth);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    s_vlast = v;
    s_vlast_voff = voff;
    s_vlast_stride = stride;
    s_vlast_nverts = b->nverts;
    s_st.draws++;
    s_st.vdraws++;
    s_st.tris += b->nindices / 3;
    return 1;
}

static int d3d_xdraw_vsh(const Nv2aSurface *s, const Nv2aXVsBatch *b)
{
    if (g_rpz_on > 0) {
        RPZ_BEGIN(rpz_t);
        int r = d3d_submit_vertex_draw(s, b);
        RPZ_END(RPZ_DRAW, rpz_t);
        return r;
    }
    if (!g_bruteforce_perf_counters_enabled)
        return d3d_submit_vertex_draw(s, b);
    uint64_t started = bruteforce_perf_clock();
    int result = d3d_submit_vertex_draw(s, b);
    s_st.vertex_draw_ticks += bruteforce_perf_clock() - started;
    return result;
}

/* RECOMP_GPU_VSH_VERIFY: the last draw_vsh batch's vertices through its
 * shader again, captured with stream output, mapped back to the executor's
 * terms (surface pixels and the title's z). */
static int d3d_vsh_readback(Nv2aXVsOut *out, uint32_t n)
{
    static ID3D11Buffer *sob, *stg;
    static UINT so_cap;
    static const D3D11_SO_DECLARATION_ENTRY decl[] = {
        { 0, "SV_POSITION", 0, 0, 4, 0 },
        { 0, "COLOR", 0, 0, 4, 0 },
        { 0, "COLOR", 1, 0, 4, 0 },
        { 0, "FOG", 0, 0, 1, 0 },
        { 0, "SV_ClipDistance", 0, 0, 2, 0 },
        { 0, NULL, 0, 0, 1, 0 },                  /* pad */
        { 0, "TEXCOORD", 0, 0, 4, 0 },
        { 0, "TEXCOORD", 1, 0, 4, 0 },
        { 0, "TEXCOORD", 2, 0, 4, 0 },
        { 0, "TEXCOORD", 3, 0, 4, 0 },
    };
    UINT stride = sizeof(Nv2aXVsOut), zero = 0, need = n * (UINT)sizeof(Nv2aXVsOut), i;
    Vsh *v = s_vlast;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer *nullb = NULL;

    if (s_vlast_culled) {
        fprintf(stderr, "[VSH-VERIFY] skipped fully culled batch: no GPU vertex output\n");
        return 0;
    }
    if (!v || !v->code || n != s_vlast_nverts || !n)
        return 0;
    gpu_d3d11_check_device(s_dev, S_OK, "starting vertex verification readback");
    if (!v->so && gpu_call_failed(ID3D11Device_CreateGeometryShaderWithStreamOutput(
                    s_dev, v->code, v->code_size, decl, 10, &stride, 1,
                    D3D11_SO_NO_RASTERIZED_STREAM, NULL, &v->so),
                    "creating vertex verification shader")) {
        v->so = NULL;
        return 0;
    }
    if (need > so_cap) {
        D3D11_BUFFER_DESC bd;
        if (sob) ID3D11Buffer_Release(sob);
        if (stg) ID3D11Buffer_Release(stg);
        sob = stg = NULL;
        so_cap = 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = need;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &sob),
                            "creating vertex verification output"))
            return 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (gpu_call_failed(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &stg),
                            "creating vertex verification staging buffer"))
            return 0;
        so_cap = need;
    }
    if (s_vlast_raw) {
        ID3D11Buffer *bufs[NV2A_XVS_MAX_STREAMS];
        for (i = 0; i < s_vlast_ns; i++)
            bufs[i] = s_vring;
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_vlast_il);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, s_vlast_ns, bufs, s_vlast_strides,
                                               s_vlast_offs);
    } else {
        ID3D11DeviceContext_IASetInputLayout(s_ctx, v->il);
        if (v->nin)
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring, &s_vlast_stride,
                                                   &s_vlast_voff);
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, v->vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(s_ctx, v->so, NULL, 0);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &sob, &zero);
    ID3D11DeviceContext_Draw(s_ctx, n, 0);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &nullb, &zero);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)sob);
    if (gpu_call_failed(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0,
                                                D3D11_MAP_READ, 0, &m),
                        "mapping vertex verification readback"))
        return 0;
    memcpy(out, m.pData, need);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
    for (i = 0; i < n; i++) {
        float *p = out[i].pos, w = p[3];
        if (w != 0.0f) {
            float zd = p[2] / w;
            p[0] = (p[0] / w + 1.0f) * 0.5f * s_xlast.surf[0];
            p[1] = (1.0f - p[1] / w) * 0.5f * s_xlast.surf[1];
            p[2] = s_xlast.depth[1] != 0.0f ? zd / s_xlast.depth[1] + s_xlast.depth[0] : zd;
        }
    }
    return 1;
}

/* ---- visibility tests -------------------------------------------------------- */

#define MAX_QUERIES 64
#define MAX_REPORTS 1024
static ID3D11Query *s_q_pool[MAX_QUERIES];
static int s_q_free;                    /* pool entries not in use */
static ID3D11Query *s_q_pending[MAX_QUERIES];
static double s_q_pending_divisor[MAX_QUERIES];
static int s_q_npending;
static ID3D11Query *s_q_active;
static double s_q_active_divisor;
static uint64_t s_q_total;
static ID3D11Query *s_event_pool[MAX_REPORTS];
static int s_event_free;

static void query_error(const char *operation, HRESULT hr)
{
    gpu_d3d11_check_device(s_dev, hr, operation);
    fprintf(stderr, "[GPU-D3D11] %s failed 0x%08lX (device removed reason 0x%08lX);"
                    " cannot safely complete outstanding GPU work\n",
            operation, (unsigned long)hr,
            (unsigned long)ID3D11Device_GetDeviceRemovedReason(s_dev));
    fflush(stderr);
    abort();
}

static ID3D11Query *query_create(D3D11_QUERY type)
{
    HRESULT hr;
    D3D11_QUERY_DESC qd;
    ID3D11Query *q = NULL;
    qd.Query = type;
    qd.MiscFlags = 0;
    hr = ID3D11Device_CreateQuery(s_dev, &qd, &q);
    if (FAILED(hr))
        query_error("CreateQuery", hr);
    return q;
}

/* perf12 debug, RECOMP_DEBUG_ZEQ=1: before each depth-EQUAL vertex-program
 * draw, count its samples under EQUAL, LESS_EQUAL and GREATER_EQUAL with
 * colour, depth and stencil writes off. LE-EQ is the number of pixels whose
 * depth comes out strictly nearer than the depth already stored, which for a
 * multi-pass light/decal over the same geometry is a mismatch (z-fight);
 * GE-EQ also counts honestly occluded pixels. Runs only in the two frames a
 * RECOMP_DEBUG_PROBE trigger arms (needs RECOMP_DEBUG_PROBE set). Slow (synchronous queries);
 * also adds to any guest occlusion query open at the time. */
static int zeq_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DEBUG_ZEQ");
        on = e && e[0] == '1';
    }
    return on;
}

static ID3D11BlendState *s_zeq_blend;
static uint64_t s_zeq_tot[3];
static unsigned s_zeq_draws, s_zeq_bad, s_zeq_gen;

static uint64_t zeq_count(const Nv2aRenderState *rs, uint32_t func, UINT n, UINT start)
{
    Nv2aRenderState c = *rs;
    ID3D11DepthStencilState *ds;
    ID3D11Query *q;
    UINT64 v = 0;
    c.depth_func = func;
    c.depth_write = 0;
    c.stencil_write_mask = 0;
    ds = get_ds(&c, 1);
    if (!ds) return 0;
    q = query_create(D3D11_QUERY_OCCLUSION);
    if (!q) return 0;
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds, s_fragment.stencil);
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_DrawIndexed(s_ctx, n, start, 0);
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)q);
    while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)q, &v, sizeof v, 0) == S_FALSE)
        ;
    ID3D11Query_Release(q);
    return v;
}

static void zeq_measure(const Nv2aRenderState *rs, UINT n, UINT start)
{
    uint64_t eq, le, ge;
    if (!s_zeq_blend) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &s_zeq_blend)))
            return;
    }
    if (s_zeq_gen != s_gen) {
        if (s_zeq_draws)
            fprintf(stderr, "[ZEQ] frame %u: %u equal-depth draws, %u with near misses;"
                            " samples eq %llu, nearer %llu, farther-or-hidden %llu\n",
                    s_zeq_gen, s_zeq_draws, s_zeq_bad, (unsigned long long)s_zeq_tot[0],
                    (unsigned long long)s_zeq_tot[1], (unsigned long long)s_zeq_tot[2]);
        s_zeq_gen = s_gen;
        s_zeq_draws = s_zeq_bad = 0;
        memset(s_zeq_tot, 0, sizeof s_zeq_tot);
    }
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_zeq_blend, s_fragment.factor, UINT_MAX);
    eq = zeq_count(rs, 0x202, n, start);
    le = zeq_count(rs, 0x203, n, start);
    ge = zeq_count(rs, 0x206, n, start);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_fragment.blend, s_fragment.factor, UINT_MAX);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_fragment.depth, s_fragment.stencil);
    s_zeq_draws++;
    s_zeq_tot[0] += eq;
    s_zeq_tot[1] += le > eq ? le - eq : 0;
    s_zeq_tot[2] += ge > eq ? ge - eq : 0;
    if (le > eq) s_zeq_bad++;
#ifdef RECOMP_DEBUG_TOOLS
    if (s_dl_rec) {
        s_dl_rec->has_zeq = 1;
        s_dl_rec->zeq_eq = eq;
        s_dl_rec->zeq_le = le > eq ? le - eq : 0;
        s_dl_rec->zeq_ge = ge > eq ? ge - eq : 0;
    }
#endif
}
/* debug: in the probe window, describe (and dump level 0 of) the view bound
 * to pixel-shader slot 0 before a draw. RECOMP_DEBUG_PROBE_SRV=1. */
static void probe_srv(uint32_t seq)
{
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *tex = NULL, *st = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 1, &srv);
    if (!srv) { fprintf(stderr, "[PROBE] draw %u srv0 none\n", seq); return; }
    ID3D11ShaderResourceView_GetDesc(srv, &vd);
    ID3D11ShaderResourceView_GetResource(srv, &res);
    if (res && SUCCEEDED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
        ID3D11Texture2D_GetDesc(tex, &td);
        fprintf(stderr, "[PROBE] draw %u srv0 %ux%u fmt %u view fmt %u mips %u/%u most_detailed %u\n",
                seq, td.Width, td.Height, td.Format, vd.Format, td.MipLevels,
                vd.Texture2D.MipLevels, vd.Texture2D.MostDetailedMip);
        td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) {
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0,
                                                      res, vd.Texture2D.MostDetailedMip, NULL);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                                  D3D11_MAP_READ, 0, &m))) {
                const char *dir = getenv("RECOMP_DEBUG_PROBE_DIR");
                char p[512];
                FILE *f;
                UINT rows = td.Format >= 70 && td.Format <= 84 ? (td.Height + 3) / 4 : td.Height;
                snprintf(p, sizeof p, "%s\\srv_%u_%05u_f%u_%ux%u_p%u.bin", dir ? dir : ".",
                         s_gen, seq, td.Format, td.Width, td.Height, m.RowPitch);
                f = fopen(p, "wb");
                if (f) { fwrite(m.pData, 1, (size_t)m.RowPitch * rows, f); fclose(f); }
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
            }
            ID3D11Texture2D_Release(st);
        }
        ID3D11Texture2D_Release(tex);
    }
    if (res) ID3D11Resource_Release(res);
    ID3D11ShaderResourceView_Release(srv);
}
static ID3D11Query *query_get(void)
{
    return s_q_free ? s_q_pool[--s_q_free] : query_create(D3D11_QUERY_OCCLUSION);
}

static void query_put(ID3D11Query *q)
{
    if (s_q_free < MAX_QUERIES)
        s_q_pool[s_q_free++] = q;
    else
        ID3D11Query_Release(q);
}

static ID3D11Query *event_get(void)
{
    return s_event_free ? s_event_pool[--s_event_free] : query_create(D3D11_QUERY_EVENT);
}

static void event_put(ID3D11Query *q)
{
    if (s_event_free < MAX_REPORTS)
        s_event_pool[s_event_free++] = q;
    else
        ID3D11Query_Release(q);
}

#define MAX_GPU_WORK_BATCHES 8
#define MAX_GPU_WORK_OPERATIONS 128
static ID3D11Query *s_work_events[MAX_GPU_WORK_BATCHES];
static struct {
    uint32_t address, value;
} s_work_release[MAX_GPU_WORK_BATCHES];
static unsigned s_work_head, s_work_count, s_work_operations;
static unsigned s_work_submitted, s_work_wait_count, s_work_peak;
#ifdef BRUTEFORCE_D3D11_TESTING
static unsigned s_work_test_pending, s_work_test_pending_seen;
#endif

static void d3d_poll_reports(void);

static void publish_semaphore(uint32_t address, uint32_t value)
{
    InterlockedExchange((volatile LONG *)GUEST(address), (LONG)value);
}

static int gpu_work_retire(int wait)
{
    ID3D11Query *event = s_work_events[s_work_head];
    BOOL complete;
    HRESULT hr;

    RPZ_BEGIN(rpz_wait);
    if (wait) {
        s_st.work_waits++;
        s_work_wait_count++;
    }
    do {
        complete = FALSE;
        if (scale_test_failure("queue-wait"))
            query_error("GetData(GPU work)", E_FAIL);
        if (scale_test_failure("queue-device"))
            gpu_d3d11_check_device(s_dev, DXGI_ERROR_DEVICE_REMOVED, "waiting for GPU work");
        hr = gpu_d3d11_get_data(s_dev, s_ctx, (ID3D11Asynchronous *)event,
                                &complete, sizeof complete, 0, "waiting for GPU work");
#ifdef BRUTEFORCE_D3D11_TESTING
        if (hr == S_OK && complete && s_work_test_pending) {
            --s_work_test_pending;
            ++s_work_test_pending_seen;
            hr = S_FALSE;
        }
#endif
        if (hr == S_FALSE || (hr == S_OK && !complete)) {
            if (!wait)
                return 0;
            d3d_poll_reports();
            SwitchToThread();
        } else if (hr != S_OK) {
            query_error("GetData(GPU work)", hr);
        }
    } while (hr == S_FALSE || !complete);
    if (wait)
        RPZ_END(RPZ_GPU_WAIT, rpz_wait);
    if (s_work_release[s_work_head].address) {
        publish_semaphore(s_work_release[s_work_head].address, s_work_release[s_work_head].value);
        s_work_release[s_work_head].address = 0;
    }
    event_put(event);
    s_work_events[s_work_head] = NULL;
    s_work_head = (s_work_head + 1) % MAX_GPU_WORK_BATCHES;
    --s_work_count;
    return 1;
}

static void gpu_work_checkpoint(void)
{
    ID3D11Query *event;
    unsigned slot;

    if (!s_work_operations)
        return;
    while (s_work_count == MAX_GPU_WORK_BATCHES)
        gpu_work_retire(1);
    if (scale_test_failure("queue-query"))
        query_error("CreateQuery(GPU work)", E_OUTOFMEMORY);
    event = event_get();
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)event);
    slot = (s_work_head + s_work_count) % MAX_GPU_WORK_BATCHES;
    s_work_events[slot] = event;
    ++s_work_count;
    ++s_work_submitted;
    ++s_st.work_batches;
    if (s_work_peak < s_work_count)
        s_work_peak = s_work_count;
    s_work_operations = 0;
}

static int s_flush_draws = -1;
static unsigned s_flush_count;

static void gpu_work_begin(void)
{
    gpu_d3d11_check_device(s_dev, S_OK, "submitting GPU work");
    if (s_flush_draws < 0) {
        const char *e = getenv("RECOMP_GPU_FLUSH_DRAWS");
        s_flush_draws = e && *e ? atoi(e) : 128;
    }
    if (s_flush_draws > 0 && ++s_flush_count >= (unsigned)s_flush_draws) {
        s_flush_count = 0;
        if (!rpz_ab_off(32))
            ID3D11DeviceContext_Flush(s_ctx);
    }
    if (s_work_operations >= MAX_GPU_WORK_OPERATIONS)
        gpu_work_checkpoint();
    if (!s_work_operations && s_work_count == MAX_GPU_WORK_BATCHES)
        gpu_work_retire(1);
    ++s_work_operations;
}

static void gpu_work_finish(void)
{
    gpu_work_checkpoint();
    while (s_work_count)
        gpu_work_retire(1);
}

void bruteforce_d3d11_finish(void)
{
    if (s_dev)
        gpu_work_finish();
}

void bruteforce_d3d11_release(uint32_t address, uint32_t value)
{
    if (s_dev) {
        static int async = -1;
        if (async < 0) {
            const char *setting = getenv("RECOMP_GPU_ASYNC_FENCE");
            async = !setting || strcmp(setting, "0") != 0;
        }
        if (async) {
            while (s_work_count && gpu_work_retire(0))
                ;
            gpu_work_checkpoint();
            if (s_work_count) {
                unsigned last = (s_work_head + s_work_count - 1) % MAX_GPU_WORK_BATCHES;
                if (s_work_release[last].address) {
                    /* Keep releases ordered even when two share a draw boundary. */
                    s_work_operations = 1;
                    gpu_work_checkpoint();
                    last = (s_work_head + s_work_count - 1) % MAX_GPU_WORK_BATCHES;
                }
                s_work_release[last].address = address;
                s_work_release[last].value = value;
                return;
            }
        } else {
            gpu_work_finish();
        }
    }
    publish_semaphore(address, value);
}

static double query_divisor(void)
{
    double logical, physical;

    if (!s_cur || !s_cur->w || !s_cur->h)
        return 1.0;
    logical = (double)s_cur->w * s_cur->h;
    physical = (double)s_cur->pw * s_cur->ph;
    return physical > 0.0 ? physical / logical : 1.0;
}

static uint64_t normalize_query_count(UINT64 count, double divisor)
{
    return divisor < 0.999999 || divisor > 1.000001
               ? (uint64_t)((double)count / divisor + 0.5)
               : count;
}

static void query_begin(void)
{
    s_q_active = query_get();
    s_q_active_divisor = query_divisor();
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_q_active);
}

/* Wait for every ended query and add its count. */
static void query_drain(void)
{
    int i;
    double t0 = qpc_ms();
    RPZ_BEGIN(rpz_q);
    for (i = 0; i < s_q_npending; i++) {
        UINT64 n = 0;
        HRESULT hr;
        do {
            hr = gpu_d3d11_get_data(s_dev, s_ctx, (ID3D11Asynchronous *)s_q_pending[i],
                                    &n, sizeof n, 0, "waiting for visibility query");
            if (hr == S_FALSE)
                SwitchToThread();
        } while (hr == S_FALSE);
        if (hr != S_OK)
            query_error("GetData(occlusion)", hr);
        s_q_total += normalize_query_count(n, s_q_pending_divisor[i]);
        query_put(s_q_pending[i]);
    }
    s_q_npending = 0;
    s_q_wait_ms += qpc_ms() - t0;
    RPZ_END(RPZ_GPU_WAIT, rpz_q);
}

static void query_end(void)
{
    if (!s_q_active)
        return;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_q_active);
    if (s_q_npending == MAX_QUERIES)
        query_drain();
    s_q_pending[s_q_npending] = s_q_active;
    s_q_pending_divisor[s_q_npending++] = s_q_active_divisor;
    s_q_active = NULL;
}

static int query_suspend_clear(void)
{
    int counting = s_q_active != NULL;
    if (counting)
        query_end();
    return counting;
}

static void query_resume_clear(int counting)
{
    if (counting)
        query_begin();
}

/* RECOMP_GPU_ZPASS=async: a report is written into guest memory when its
 * queries complete, polled from the NV2A thread's loop, instead of waiting
 * for the GPU at GET_REPORT. The title sees the result later in time, as it
 * would from the console's GPU, and D3D's own polling waits for it. Off by
 * default while it is measured. */
typedef struct {
    uint32_t va;
    uint64_t stamp;
    uint64_t prefix;
    int reset;                          /* a clear came before these queries */
    int nq;
    ID3D11Query *event;
    ID3D11Query *q[MAX_QUERIES];
    double divisor[MAX_QUERIES];
} Report;
static Report *s_rep;                   /* ring of MAX_REPORTS */
static uint32_t s_rep_head, s_rep_tail;
static uint64_t s_rep_running;          /* the count since the last clear, resolved */
static int s_rep_reset_next;
static int s_zpass_async;
static double s_rep_last_flush;

static void report_write(const Report *r, uint64_t count)
{
    uint8_t *at = GUEST(r->va);
    uint32_t c = count > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)count, done = 0;
    memcpy(at, &r->stamp, 8);
    memcpy(at + 8, &c, 4);
    MemoryBarrier();
    memcpy(at + 12, &done, 4);
    s_rep_written++;
}

static void report_pending(void)
{
    double now = qpc_ms();
    if (now - s_rep_last_flush > 0.25) {
        ID3D11DeviceContext_Flush(s_ctx);
        s_rep_last_flush = now;
    }
    s_rep_flushes++;
}

/* Complete reports in command order, only after all preceding GPU work. */
static void d3d_poll_reports(void)
{
    LONG owner = InterlockedCompareExchange(&s_device_thread, 0, 0);
    if (!owner || (DWORD)owner != GetCurrentThreadId() || !s_rep)
        return;
    while (s_rep_head != s_rep_tail) {
        Report *r = &s_rep[s_rep_head % MAX_REPORTS];
        uint64_t sum = r->prefix;
        BOOL done = FALSE;
        HRESULT hr;
        int i;
        hr = gpu_d3d11_get_data(s_dev, s_ctx, (ID3D11Asynchronous *)r->event, &done,
                                sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH,
                                "polling visibility report event");
        if (hr == S_FALSE || (hr == S_OK && !done)) {
            report_pending();
            return;
        }
        if (hr != S_OK)
            query_error("GetData(report event)", hr);
        for (i = 0; i < r->nq; i++) {
            UINT64 n = 0;
            hr = gpu_d3d11_get_data(s_dev, s_ctx, (ID3D11Asynchronous *)r->q[i], &n,
                                    sizeof n, D3D11_ASYNC_GETDATA_DONOTFLUSH,
                                    "polling visibility query");
            if (hr == S_FALSE) {
                report_pending();
                return;
            }
            if (hr != S_OK)
                query_error("GetData(occlusion)", hr);
            sum += normalize_query_count(n, r->divisor[i]);
        }
        if (r->reset)
            s_rep_running = 0;
        s_rep_running += sum;
        report_write(r, s_rep_running);
        for (i = 0; i < r->nq; i++)
            query_put(r->q[i]);
        event_put(r->event);
        s_rep_head++;
    }
}

static void d3d_poll(void)
{
    LONG owner = InterlockedCompareExchange(&s_device_thread, 0, 0);
    if (!owner || (DWORD)owner != GetCurrentThreadId())
        return;
    while (s_work_count && gpu_work_retire(0))
        ;
    d3d_poll_reports();
}

static int d3d_zpass_report(uint32_t report_va, uint64_t stamp)
{
    Report *r;
    int counting = s_q_active != NULL, i;

    if (!s_zpass_async || !s_dev)
        return 0;
    gpu_d3d11_check_device(s_dev, S_OK, "starting visibility report");
    gpu_work_begin();
    if (!s_rep && !(s_rep = (Report *)calloc(MAX_REPORTS, sizeof *s_rep))) {
        fprintf(stderr, "[GPU-D3D11] cannot allocate asynchronous reports; using synchronous queries\n");
        s_zpass_async = 0;
        return 0;
    }
    if (s_rep_tail - s_rep_head == MAX_REPORTS)
        while (s_rep_tail - s_rep_head == MAX_REPORTS) {
            d3d_poll();
            SwitchToThread();
        }
    query_end();
    r = &s_rep[s_rep_tail % MAX_REPORTS];
    r->va = report_va;
    r->stamp = stamp;
    r->prefix = s_q_total;
    r->reset = s_rep_reset_next;
    r->nq = s_q_npending;
    r->event = event_get();
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)r->event);
    for (i = 0; i < s_q_npending; i++) {
        r->q[i] = s_q_pending[i];
        r->divisor[i] = s_q_pending_divisor[i];
    }
    s_q_npending = 0;
    s_q_total = 0;
    s_rep_reset_next = 0;
    s_rep_tail++;
    if (counting)
        query_begin();
    d3d_poll();
    return 1;
}

static void d3d_zpass(int op)
{
    if (!ensure_device())
        return;
    gpu_work_begin();
    if (s_zpass_async && op == NV2A_ZPASS_CLEAR) {
        /* Discarded queries may still be in flight, so never reuse them. */
        int counting = s_q_active != NULL, i;
        query_end();
        for (i = 0; i < s_q_npending; i++)
            ID3D11Query_Release(s_q_pending[i]);
        s_q_npending = 0;
        s_q_total = 0;
        s_rep_reset_next = 1;
        if (counting)
            query_begin();
        return;
    }
    if (op == NV2A_ZPASS_BEGIN) {
        query_end();
        query_begin();
    } else if (op == NV2A_ZPASS_END) {
        query_end();
    } else {                                /* CLEAR_REPORT_VALUE */
        int counting = s_q_active != NULL;
        query_end();
        query_drain();
        s_q_total = 0;
        if (counting)
            query_begin();
    }
}

static uint32_t d3d_zpass_count(void)
{
    int counting = s_q_active != NULL;
    if (!s_dev)
        return 0;
    gpu_d3d11_check_device(s_dev, S_OK, "reading visibility count");
    gpu_work_begin();
    query_end();
    query_drain();
    if (counting)
        query_begin();
    s_q_reads++;
    return s_q_total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)s_q_total;
}

/* ---- readback and flip ----------------------------------------------------- */

static ID3D11Texture2D *s_staging;
static uint32_t s_staging_w, s_staging_h, s_staging_bpp;
static ID3D11Texture2D *s_readback_resolve;
static ID3D11RenderTargetView *s_readback_resolve_rtv;
static uint32_t s_readback_resolve_w, s_readback_resolve_h, s_readback_resolve_bpp;
static ID3D11Texture2D *s_present_shared;
static ID3D11RenderTargetView *s_present_rtv;
static IDXGIKeyedMutex *s_present_mutex;
static HANDLE s_present_handle;
static uint32_t s_present_w, s_present_h;
static int s_present_unsubmitted, s_present_failed;

typedef struct {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    IDXGIKeyedMutex *mutex;
    HANDLE handle;
    uint32_t width, height;
} SharedPresent;

static void release_shared_candidate(SharedPresent *present)
{
    if (present->mutex) IDXGIKeyedMutex_Release(present->mutex);
    if (present->rtv) ID3D11RenderTargetView_Release(present->rtv);
    if (present->texture) ID3D11Texture2D_Release(present->texture);
    memset(present, 0, sizeof *present);
}

static void release_shared_present(void)
{
    if (s_present_mutex) IDXGIKeyedMutex_Release(s_present_mutex);
    if (s_present_rtv) ID3D11RenderTargetView_Release(s_present_rtv);
    if (s_present_shared) ID3D11Texture2D_Release(s_present_shared);
    s_present_mutex = NULL;
    s_present_rtv = NULL;
    s_present_shared = NULL;
    s_present_handle = NULL;
    s_present_w = s_present_h = 0;
}

static int create_shared_present(uint32_t width, uint32_t height, SharedPresent *present)
{
    D3D11_TEXTURE2D_DESC td;
    IDXGIResource *resource = NULL;
    HRESULT hr;

    memset(present, 0, sizeof *present);
    memset(&td, 0, sizeof td);
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    hr = scale_test_failure("shared-texture") ? E_OUTOFMEMORY :
         ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &present->texture);
    if (SUCCEEDED(hr))
        hr = scale_test_failure("shared-rtv") ? E_OUTOFMEMORY :
             ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)present->texture,
                                                 NULL, &present->rtv);
    if (SUCCEEDED(hr))
        hr = ID3D11Texture2D_QueryInterface(present->texture, &IID_IDXGIResource,
                                            (void **)&resource);
    if (SUCCEEDED(hr))
        hr = scale_test_failure("shared-handle") ? E_OUTOFMEMORY :
             IDXGIResource_GetSharedHandle(resource, &present->handle);
    if (SUCCEEDED(hr))
        hr = scale_test_failure("shared-mutex") ? E_OUTOFMEMORY :
             ID3D11Texture2D_QueryInterface(present->texture, &IID_IDXGIKeyedMutex,
                                            (void **)&present->mutex);
    if (resource)
        IDXGIResource_Release(resource);
    if (FAILED(hr) || !present->handle) {
        gpu_d3d11_check_device(s_dev, hr, "creating shared presentation texture");
        fprintf(stderr, "[GPU-D3D11] shared presentation texture failed 0x%08lX;"
                        " retaining prior shared resources and CPU presentation\n",
                (unsigned long)hr);
        release_shared_candidate(present);
        return 0;
    }
    present->width = width;
    present->height = height;
    return 1;
}

static void install_shared_present(SharedPresent *present)
{
    release_shared_present();
    s_present_shared = present->texture;
    s_present_rtv = present->rtv;
    s_present_mutex = present->mutex;
    s_present_handle = present->handle;
    s_present_w = present->width;
    s_present_h = present->height;
    memset(present, 0, sizeof *present);
}

static void shared_mutex_error(HRESULT hr, const char *operation)
{
    gpu_d3d11_check_device(s_dev, hr, operation);
    fprintf(stderr, "[GPU-D3D11] %s failed 0x%08lX; shared-frame ownership"
                    " cannot be recovered, stopping\n", operation, (unsigned long)hr);
    fflush(stderr);
    abort();
}

static int reclaim_unsubmitted_present(void)
{
    HRESULT hr;
    if (!s_present_unsubmitted)
        return 1;
    hr = IDXGIKeyedMutex_AcquireSync(s_present_mutex, 1, 0);
    gpu_d3d11_check_device(s_dev, hr, "reclaiming shared frame");
    if (hr == WAIT_TIMEOUT) {
        return 0;
    }
    if (hr != S_OK)
        shared_mutex_error(hr, "reclaiming shared frame");
    hr = IDXGIKeyedMutex_ReleaseSync(s_present_mutex, 0);
    gpu_d3d11_check_device(s_dev, hr, "releasing reclaimed shared frame");
    if (hr != S_OK)
        shared_mutex_error(hr, "releasing reclaimed shared frame");
    s_present_unsubmitted = 0;
    return 1;
}

static int prepare_shared_present(uint32_t width, uint32_t height,
                                  SharedPresent *candidate, int *old_locked)
{
    HRESULT hr;
    memset(candidate, 0, sizeof *candidate);
    *old_locked = 0;
    if (s_present_failed)
        return 0;
    if (s_present_shared && s_present_w == width && s_present_h == height)
        return 1;
    if (!reclaim_unsubmitted_present())
        return 0;
    if (s_present_shared) {
        hr = IDXGIKeyedMutex_AcquireSync(s_present_mutex, 0, 0);
        gpu_d3d11_check_device(s_dev, hr, "resizing shared frame");
        if (hr == WAIT_TIMEOUT) {
            s_st.present_busy++;
            fprintf(stderr, "[GPU-D3D11] shared frame resize deferred: prior frame"
                            " is pending or busy\n");
            return 0;
        }
        if (hr != S_OK)
            shared_mutex_error(hr, "shared frame resize acquire");
        *old_locked = 1;
    }
    return create_shared_present(width, height, candidate);
}

static int release_shared_resize_lock(int *old_locked)
{
    HRESULT hr;
    if (!*old_locked)
        return 1;
    *old_locked = 0;
    hr = IDXGIKeyedMutex_ReleaseSync(s_present_mutex, 0);
    gpu_d3d11_check_device(s_dev, hr, "releasing shared frame resize lock");
    if (hr == S_OK)
        return 1;
    shared_mutex_error(hr, "shared frame resize release");
    return 0;
}

static void publish_shared_present(Surf *sf)
{
    uint32_t width = sf->pw, height = sf->ph;
    SharedPresent candidate;
    int old_locked = 0;
    HRESULT hr;

    s_shared_ok = 0;
    if (!s_direct_present || s_present_failed)
        return;
    if (!reclaim_unsubmitted_present())
        return;
    if (sf->bpp != 4 || sf->pitch != sf->w * 4)
        return;
    if (gpu_scale_requested()) {
        if (gpu_scale_native() && !gpu_scale_fixed_output(NULL, NULL))
            gpu_scale_output_size(&width, &height);
        else
            gpu_present_output_size(&width, &height);
    }
    if (!gpu_present_shared_ready(width, height))
        return;
    if (!prepare_shared_present(width, height, &candidate, &old_locked)) {
        release_shared_resize_lock(&old_locked);
        return;
    }
    if (!release_shared_resize_lock(&old_locked)) {
        release_shared_candidate(&candidate);
        return;
    }
    if (candidate.texture)
        install_shared_present(&candidate);
    hr = IDXGIKeyedMutex_AcquireSync(s_present_mutex, 0, 0);
    if (scale_test_failure("shared-producer-acquire"))
        hr = E_FAIL;
    if (scale_test_failure("shared-producer-loss"))
        hr = DXGI_ERROR_DEVICE_REMOVED;
    gpu_d3d11_check_device(s_dev, hr, "acquiring producer shared frame");
    if (hr == WAIT_TIMEOUT) {
        s_st.present_busy++;
        return;
    }
    if (hr != S_OK)
        shared_mutex_error(hr, "producer shared frame acquire");
    surf_resolve_raw(sf);
    if (sf->pw == width && sf->ph == height) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_present_shared,
                                         (ID3D11Resource *)sf->tex);
    } else {
        enum GpuScaleBlitMode mode =
            sf->pw > width || sf->ph > height ? GPU_SCALE_BLIT_BOX
                                              : GPU_SCALE_BLIT_CATMULL_ROM;
        if (!d3d_scale_blit(sf->srv, sf->pw, sf->ph, s_present_rtv, width, height, mode)) {
            fprintf(stderr, "[GPU-D3D11] final scaled presentation resolve failed;"
                            " retaining CPU presentation\n");
            hr = IDXGIKeyedMutex_ReleaseSync(s_present_mutex, 0);
            gpu_d3d11_check_device(s_dev, hr, "releasing failed shared resolve");
            if (hr != S_OK)
                shared_mutex_error(hr, "failed shared resolve release");
            s_present_failed = 1;
            return;
        }
    }
    if (halo2_fxaa_enabled())
        halo2_fxaa_apply(s_dev, s_ctx, s_present_shared, s_present_rtv, width, height);
    ID3D11DeviceContext_Flush(s_ctx);
#ifdef RECOMP_DEBUG_TOOLS
    if (halo2_screenshot_enabled())
        halo2_screenshot_try_copy(s_dev, s_ctx, s_present_shared, width, height,
                                  DXGI_FORMAT_B8G8R8A8_UNORM);
#endif
    hr = IDXGIKeyedMutex_ReleaseSync(s_present_mutex, 1);
    if (scale_test_failure("shared-producer-release"))
        hr = E_FAIL;
    gpu_d3d11_check_device(s_dev, hr, "releasing producer shared frame");
    if (hr != S_OK)
        shared_mutex_error(hr, "producer shared frame release");
    if (gpu_present_submit_shared(s_present_handle, width, height,
                                  xbox_FramebufferWindowNextFrameId())) {
        s_st.present_shared++;
        s_shared_ok = 1;
    }
    else
        s_present_unsubmitted = 1;
}

typedef struct {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    uint32_t width, height, bpp;
} ReadbackResolve;

static void release_readback_candidate(ReadbackResolve *resolve)
{
    if (resolve->rtv) ID3D11RenderTargetView_Release(resolve->rtv);
    if (resolve->texture) ID3D11Texture2D_Release(resolve->texture);
    memset(resolve, 0, sizeof *resolve);
}

static int create_readback_resolve(const Surf *sf, ReadbackResolve *resolve)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    memset(resolve, 0, sizeof *resolve);
    memset(&td, 0, sizeof td);
    td.Width = sf->w;
    td.Height = sf->h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = surf_format(sf->bpp);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = scale_test_failure("readback-texture") ? E_OUTOFMEMORY :
         ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &resolve->texture);
    if (SUCCEEDED(hr))
        hr = scale_test_failure("readback-rtv") ? E_OUTOFMEMORY :
             ID3D11Device_CreateRenderTargetView(
                 s_dev, (ID3D11Resource *)resolve->texture, NULL, &resolve->rtv);
    if (FAILED(hr)) {
        gpu_d3d11_check_device(s_dev, hr, "creating scaled readback resolve");
        fprintf(stderr, "[GPU-SCALE] readback resolve creation failed 0x%08lX;"
                        " retaining prior readback resources\n", (unsigned long)hr);
        release_readback_candidate(resolve);
        return 0;
    }
    resolve->width = sf->w;
    resolve->height = sf->h;
    resolve->bpp = sf->bpp;
    return 1;
}

static void install_readback_resolve(ReadbackResolve *resolve)
{
    if (s_readback_resolve_rtv) ID3D11RenderTargetView_Release(s_readback_resolve_rtv);
    if (s_readback_resolve) ID3D11Texture2D_Release(s_readback_resolve);
    s_readback_resolve = resolve->texture;
    s_readback_resolve_rtv = resolve->rtv;
    s_readback_resolve_w = resolve->width;
    s_readback_resolve_h = resolve->height;
    s_readback_resolve_bpp = resolve->bpp;
    memset(resolve, 0, sizeof *resolve);
}

static int create_staging_texture(const Surf *sf, ID3D11Texture2D **texture)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    *texture = NULL;
    memset(&td, 0, sizeof td);
    td.Width = sf->w; td.Height = sf->h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = surf_format(sf->bpp);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = scale_test_failure("staging-texture") ? E_OUTOFMEMORY :
         ID3D11Device_CreateTexture2D(s_dev, &td, NULL, texture);
    if (FAILED(hr)) {
        gpu_d3d11_check_device(s_dev, hr, "creating readback texture");
        fprintf(stderr, "[GPU-D3D11] readback texture creation failed 0x%08lX;"
                        " retaining prior readback resources\n", (unsigned long)hr);
        return 0;
    }
    return 1;
}

static void install_staging_texture(ID3D11Texture2D *texture, const Surf *sf)
{
    if (s_staging) ID3D11Texture2D_Release(s_staging);
    s_staging = texture;
    s_staging_w = sf->w;
    s_staging_h = sf->h;
    s_staging_bpp = sf->bpp;
}

static ID3D11Texture2D *readback_source(Surf *sf)
{
    surf_resolve(sf);
    if (sf->pw == sf->w && sf->ph == sf->h)
        return sf->tex;
    if (!s_readback_resolve || s_readback_resolve_w != sf->w ||
        s_readback_resolve_h != sf->h || s_readback_resolve_bpp != sf->bpp) {
        ReadbackResolve candidate;
        if (!create_readback_resolve(sf, &candidate))
            return NULL;
        install_readback_resolve(&candidate);
    }
    if (!d3d_scale_blit(sf->srv, sf->pw, sf->ph, s_readback_resolve_rtv,
                        sf->w, sf->h,
                        sf->pw > sf->w || sf->ph > sf->h ? GPU_SCALE_BLIT_BOX
                                                         : GPU_SCALE_BLIT_BILINEAR)) {
        fprintf(stderr, "[GPU-SCALE] guest-sized readback resolve failed\n");
        return NULL;
    }
    return s_readback_resolve;
}

/* ---- debug surface probe (perf12) ----------------------------------------
 * RECOMP_DEBUG_PROBE=va[,va...] (hex) and RECOMP_DEBUG_PROBE_TRIGGER=<file>:
 * when the trigger file appears it is deleted and, for the next two frames,
 * each listed colour surface is copied out before any draw that follows a
 * write to it, and each listed address that is sampled as a texture without
 * a surface behind it is decoded once from guest memory. Raw BGRA8 (or
 * B5G6R5 for 16-bit surfaces) goes to RECOMP_DEBUG_PROBE_DIR as
 * probe_<gen>_<seq>_<S|T>_<va>_<w>x<h>_<bpp>.raw. Off unless set. */
static uint32_t s_probe_va[8];
static uint64_t s_probe_ws[8];
static int s_probe_tex_done[8];
static int s_probe_n = -1;
static unsigned s_probe_count;
static uint32_t s_probe_polled;

static void probe_init(void)
{
    const char *e = getenv("RECOMP_DEBUG_PROBE");
    s_probe_n = 0;
    while (e && *e && s_probe_n < 8) {
        char *end;
        unsigned long v = strtoul(e, &end, 16);
        if (end == e) break;
        s_probe_va[s_probe_n++] = (uint32_t)v;
        e = *end == ',' ? end + 1 : end;
    }
}

static void probe_write(const char *kind, uint32_t va, uint32_t seq, uint32_t w,
                        uint32_t h, uint32_t bpp, const void *data, size_t pitch)
{
    const char *dir = getenv("RECOMP_DEBUG_PROBE_DIR");
    char path[512];
    FILE *f;
    uint32_t y;
    snprintf(path, sizeof path, "%s\\probe_%u_%05u_%s_%08X_%ux%u_%u.raw",
             dir && *dir ? dir : ".", s_gen, seq, kind, va, w, h, bpp);
    f = fopen(path, "wb");
    if (!f) return;
    for (y = 0; y < h; ++y)
        fwrite((const uint8_t *)data + (size_t)y * pitch, 1, (size_t)w * bpp, f);
    fclose(f);
    s_probe_count++;
}

static void probe_dump_surf(Surf *sf, uint32_t seq)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *st = NULL, *src;
    src = readback_source(sf);
    if (!src) return;
    memset(&td, 0, sizeof td);
    td.Width = sf->w;
    td.Height = sf->h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = surf_format(sf->bpp);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        probe_write("S", sf->va, seq, sf->w, sf->h, sf->bpp == 2 ? 2 : 4, m.pData,
                    m.RowPitch);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

static void probe_check(uint32_t seq, const Nv2aXBatch *xb)
{
    const char *trig;
    int i, st;
    if (s_probe_n < 0) probe_init();
    if (!s_probe_n) return;
    trig = getenv("RECOMP_DEBUG_PROBE_TRIGGER");
    if (!s_probe_armed && s_probe_polled != s_gen && trig && *trig &&
        GetFileAttributesA(trig) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(trig);
        s_probe_armed = s_gen + 1;
        s_probe_count = 0;
        memset(s_probe_ws, 0, sizeof s_probe_ws);
        memset(s_probe_tex_done, 0, sizeof s_probe_tex_done);
        fprintf(stderr, "[PROBE] armed for frames %u-%u\n", s_probe_armed, s_probe_armed + 1);
    }
    s_probe_polled = s_gen;            /* the trigger file is polled once a frame */
    {   /* RECOMP_DEBUG_PROBE_ON_TEX=va or WxH: arm once, the first time a draw samples
         * that address (or a texture of that size); the texture joins the probe list. */
        static int on_tex_done;
        const char *ot = getenv("RECOMP_DEBUG_PROBE_ON_TEX");
        if (!s_probe_armed && !on_tex_done && ot && *ot && xb) {
            unsigned ww = 0, wh = 0;
            uint32_t want = 0;
            if (sscanf(ot, "%ux%u", &ww, &wh) != 2) { ww = wh = 0; want = (uint32_t)strtoul(ot, NULL, 16); }
            for (st = 0; st < 4; ++st)
                if (xb->tex_valid[st] && (ww ? xb->tex[st].width == ww && xb->tex[st].height == wh
                                             : xb->tex[st].offset == want)) {
                    want = xb->tex[st].offset;
                    if (s_probe_n < 8) s_probe_va[s_probe_n++] = want;
                    on_tex_done = 1;
                    s_probe_armed = s_gen + 1;
                    s_probe_count = 0;
                    memset(s_probe_ws, 0, sizeof s_probe_ws);
                    memset(s_probe_tex_done, 0, sizeof s_probe_tex_done);
                    {   /* also take a DEL capture of the same frames */
                        const char *sst = getenv("RECOMP_DEBUG_SCREENSHOT_TRIGGER");
                        FILE *tf = sst && *sst ? fopen(sst, "w") : NULL;
                        if (tf) fclose(tf);
                    }
                    fprintf(stderr, "[PROBE] armed by texture %08X for frames %u-%u\n",
                            want, s_probe_armed, s_probe_armed + 1);
                    break;
                }
        }
    }
    if (!s_probe_armed) return;
    if (s_gen > s_probe_armed + 1) {
        fprintf(stderr, "[PROBE] done: %u dumps\n", s_probe_count);
        if (s_zeq_draws)
            fprintf(stderr, "[ZEQ] frame %u: %u equal-depth draws, %u with near misses;"
                            " samples eq %llu, nearer %llu, farther-or-hidden %llu\n",
                    s_zeq_gen, s_zeq_draws, s_zeq_bad, (unsigned long long)s_zeq_tot[0],
                    (unsigned long long)s_zeq_tot[1], (unsigned long long)s_zeq_tot[2]);
        s_zeq_draws = s_zeq_bad = 0;
        memset(s_zeq_tot, 0, sizeof s_zeq_tot);
        s_probe_armed = 0;
        return;
    }
    if (xb && xb->state && s_gen >= s_probe_armed) {   /* log SET_CLIP_MIN/MAX changes */
        static float lmin = -1.0f, lmax = -1.0f;
        static uint32_t lzc = ~0u, lrt = ~0u;
        uint32_t zc = nv2a_backend_reg(0x1D78);   /* SET_ZMIN_MAX_CONTROL */
        uint32_t rt = s_cur ? s_cur->va : 0;
        if (xb->state->depth_min != lmin || xb->state->depth_max != lmax || zc != lzc || rt != lrt) {
            lmin = xb->state->depth_min; lmax = xb->state->depth_max; lzc = zc; lrt = rt;
            fprintf(stderr, "[PROBE] frame %u draw %u rt %08X: clip min %.1f max %.1f (zmax %.0f) zminmax_control %08X control0 %08X surface_format %08X\n",
                    s_gen, seq, rt, lmin, lmax, zeta_max(), zc, nv2a_backend_reg(0x0290),
                    nv2a_backend_reg(0x0208));
        }
    }
    {   /* RECOMP_DEBUG_PROBE_MAX: dump cap per window (default 240) */
        static unsigned probe_max;
        if (!probe_max) {
            const char *pm = getenv("RECOMP_DEBUG_PROBE_MAX");
            probe_max = pm && atoi(pm) > 0 ? (unsigned)atoi(pm) : 240u;
        }
        if (s_gen < s_probe_armed || s_probe_count >= probe_max) return;
    }
    if (xb && getenv("RECOMP_DEBUG_PROBE_ALLTEX")) {
        /* every distinct non-surface texture sampled in the window, once */
        static uint32_t seen[48];
        static unsigned nseen, seen_armed;
        if (seen_armed != s_probe_armed) { seen_armed = s_probe_armed; nseen = 0; }
        for (st = 0; st < 4; ++st) {
            const Nv2aTexture *t = &xb->tex[st];
            unsigned k;
            uint32_t *px;
            if (!xb->tex_valid[st] || nseen >= 48 || find_surf(t->offset) ||
                !t->width || !t->height || t->width * t->height > 2048u * 2048u)
                continue;
            for (k = 0; k < nseen && seen[k] != t->offset; ++k) ;
            if (k < nseen) continue;
            seen[nseen++] = t->offset;
            px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
            if (px && nv2a_backend_decode_texture(t, px)) {
                char kind[16];
                snprintf(kind, sizeof kind, "T%02X", t->color);
                probe_write(kind, t->offset, seq, t->width, t->height, 4, px,
                            (size_t)t->width * 4);
            }
            free(px);
        }
    }
    if (xb && xb->state && getenv("RECOMP_DEBUG_PROBE_STATE")) {
        /* one line per draw in the probe window: what this draw (seq) is */
        const Nv2aRenderState *r = xb->state;
        char tx[4][48];
        for (st = 0; st < 4; ++st)
            if (xb->tex_valid[st])
                snprintf(tx[st], sizeof tx[st], "%08X:%02X:%ux%u:a%X", xb->tex[st].offset,
                         xb->tex[st].color, xb->tex[st].width, xb->tex[st].height,
                         nv2a_backend_reg(0x1B04 + 0x40u * (unsigned)st) & 0xFFFu);
            else
                snprintf(tx[st], sizeof tx[st], "-");
        fprintf(stderr, "[PROBESTATE] frame %u draw %u rt %08X blend %u %X/%X/%X depth %u %X w%u"
                        " prog %08X final %08X %08X t0 %s t1 %s t2 %s t3 %s\n",
                s_gen, seq, s_cur ? s_cur->va : 0, r->blend_enable, r->blend_src, r->blend_dst,
                r->blend_eq, r->depth_test_enable, r->depth_func, r->depth_write,
                nv2a_backend_reg(0x1E70), nv2a_backend_reg(0x0288), nv2a_backend_reg(0x028C),
                tx[0], tx[1], tx[2], tx[3]);
    }
    for (i = 0; i < s_probe_n; ++i) {
        Surf *sf = find_surf(s_probe_va[i]);
        if (sf && sf->wserial != s_probe_ws[i]) {
            s_probe_ws[i] = sf->wserial;
            probe_dump_surf(sf, seq);
        }
        if (!sf && xb && !s_probe_tex_done[i]) {
            for (st = 0; st < 4; ++st) {
                const Nv2aTexture *t = &xb->tex[st];
                uint32_t *px;
                if (!xb->tex_valid[st] || t->offset != s_probe_va[i] ||
                    !t->width || !t->height || t->width * t->height > 4096u * 4096u)
                    continue;
                px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
                if (px && nv2a_backend_decode_texture(t, px)) {
                    char kind[16];
                    snprintf(kind, sizeof kind, "T%02X", t->color);
                    probe_write(kind, t->offset, seq, t->width, t->height, 4, px,
                                (size_t)t->width * 4);
                }
                free(px);
                s_probe_tex_done[i] = 1;
                break;
            }
        }
    }
}
static int apply_native_output(uint32_t width, uint32_t height, Surf *presented,
                               int require_shared)
{
    GpuScaleUpdate update;
    Surf colors[MAX_SURF];
    Depth depths[MAX_SURF];
    int replace_color[MAX_SURF] = {0}, replace_depth[MAX_SURF] = {0};
    SharedPresent shared = {0};
    ReadbackResolve resolve = {0};
    ID3D11Texture2D *staging = NULL;
    Surf *readback = presented;
    int old_locked = 0, counting, recreated_depths = 0, i;

    if (!gpu_scale_prepare_output(width, height, &update))
        return 0;
    if (!update.changed)
        return 1;
    if (!s_dev) {
        fprintf(stderr, "[GPU-SCALE] output change requires an initialized D3D11 device;"
                        " retaining prior configuration\n");
        return 0;
    }
    memset(colors, 0, sizeof colors);
    memset(depths, 0, sizeof depths);
    counting = query_suspend_clear();
    if (require_shared &&
        !prepare_shared_present(width, height, &shared, &old_locked))
        goto failed;
    if (update.scale_changed) {
        for (i = 0; i < s_nsurf; ++i) {
            Surf *old = &s_surf[i];
            uint32_t pw, ph;
            if (!gpu_scale_surface_size_for(update.scale, old->w, old->h,
                                            old->aa_sx, old->aa_sy, &pw, &ph))
                goto failed;
            if (old->pw == pw && old->ph == ph)
                continue;
            colors[i] = *old;
            colors[i].pw = pw;
            colors[i].ph = ph;
            colors[i].scale_epoch = update.epoch;
            colors[i].tex = NULL;
            colors[i].rtv = NULL;
            colors[i].srv = NULL;
            if (!create_surf(&colors[i], old->srv, old->pw, old->ph))
                goto failed;
            replace_color[i] = 1;
            if (presented == old)
                readback = &colors[i];
        }
        for (i = 0; i < s_ndepth; ++i) {
            Depth *old = &s_depth[i], *next = &depths[i];
            uint32_t w, h;
            if (!depth_size_for(old, update.scale, &w, &h))
                goto failed;
            if (old->w == w && old->h == h)
                continue;
            next->va = old->va;
            next->w = w;
            next->h = h;
            next->pitch = old->pitch;
            next->format = old->format;
            next->surface_type = old->surface_type;
            next->recreated = 1;
            next->scale_epoch = update.epoch;
            memcpy(next->logical_w, old->logical_w, sizeof next->logical_w);
            memcpy(next->logical_h, old->logical_h, sizeof next->logical_h);
            if (!create_depth_resource(next, 1))
                goto failed;
            replace_depth[i] = 1;
        }
    }
    if (readback) {
        if ((readback->pw != readback->w || readback->ph != readback->h) &&
            (!s_readback_resolve || s_readback_resolve_w != readback->w ||
             s_readback_resolve_h != readback->h || s_readback_resolve_bpp != readback->bpp) &&
            !create_readback_resolve(readback, &resolve))
            goto failed;
        if ((!s_staging || s_staging_w != readback->w || s_staging_h != readback->h ||
             s_staging_bpp != readback->bpp) && !create_staging_texture(readback, &staging))
            goto failed;
    }
    if (!release_shared_resize_lock(&old_locked) || !gpu_scale_commit_output(&update))
        goto failed;
    for (i = 0; i < s_nsurf; ++i) {
        if (replace_color[i]) {
            release_surf(&s_surf[i]);
            s_surf[i] = colors[i];
        }
        s_surf[i].scale_epoch = update.epoch;
    }
    for (i = 0; i < s_ndepth; ++i) {
        if (replace_depth[i]) {
            release_depth(&s_depth[i]);
            s_depth[i] = depths[i];
            ++recreated_depths;
        }
        s_depth[i].scale_epoch = update.epoch;
    }
    if (shared.texture)
        install_shared_present(&shared);
    if (resolve.texture)
        install_readback_resolve(&resolve);
    if (staging)
        install_staging_texture(staging, presented);
    if (recreated_depths)
        fprintf(stderr, "[GPU-SCALE] %d resized depth/stencil surface(s) require guest"
                        " reinitialization; depth/stencil migration is not implemented\n",
                recreated_depths);
    if (s_cur)
        bind_targets(s_cur, s_cur_depth);
    else
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    query_resume_clear(counting);
    return 1;

failed:
    for (i = 0; i < s_nsurf; ++i)
        if (replace_color[i])
            release_surf(&colors[i]);
    for (i = 0; i < s_ndepth; ++i)
        if (replace_depth[i])
            release_depth(&depths[i]);
    if (staging) ID3D11Texture2D_Release(staging);
    release_readback_candidate(&resolve);
    release_shared_candidate(&shared);
    release_shared_resize_lock(&old_locked);
    if (s_cur)
        bind_targets(s_cur, s_cur_depth);
    else
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    query_resume_clear(counting);
    fprintf(stderr, "[GPU-SCALE] output picture %ux%u not committed;"
                    " retaining scale %.3f, epoch %u and prior resources\n",
            width, height, gpu_scale_value(), gpu_scale_epoch());
    return 0;
}

static void update_native_output(Surf *presented)
{
    uint32_t width, height;
    if (gpu_scale_native() && !gpu_scale_fixed_output(NULL, NULL) &&
        gpu_present_output_size(&width, &height))
        apply_native_output(width, height, presented,
                            s_direct_present && gpu_present_shared_ready(width, height));
}

#ifdef BRUTEFORCE_D3D11_TESTING
int bruteforce_d3d11_test_native_output(uint32_t width, uint32_t height,
                                        uint32_t color_va, int require_shared)
{
    Surf *presented = color_va ? find_surf(color_va) : NULL;
    if (color_va && !presented)
        return 0;
    return apply_native_output(width, height, presented, require_shared);
}
#endif

static struct {
    ID3D11Texture2D *tex[2];
    uint32_t w, h, bpp, va, pitch;
    int cur, slot, pending;
    GuestSpanIdentity id;
    uint64_t wserial;
} s_arb;
static unsigned s_arb_async, s_arb_done, s_arb_dropped;
unsigned arb_stat(int i) { return i == 0 ? s_arb_async : i == 1 ? s_arb_done : s_arb_dropped; }

#ifdef RECOMP_DEBUG_TOOLS
/* Per-category tallies for one frame of the draw list. */
typedef struct { char key[64]; unsigned n; } DlTally;
static void dl_tally(DlTally *t, unsigned cap, unsigned *nt, const char *key)
{
    unsigned i;
    for (i = 0; i < *nt; i++)
        if (strcmp(t[i].key, key) == 0) { t[i].n++; return; }
    if (*nt < cap) {
        dl_copy(t[*nt].key, sizeof t[*nt].key, key, NULL);
        t[*nt].n = 1;
        (*nt)++;
    }
}

static void dl_tally_print(FILE *f, const char *title, DlTally *t, unsigned nt)
{
    unsigned i, j;
    for (i = 0; i < nt; i++)               /* small n: selection sort by count */
        for (j = i + 1; j < nt; j++)
            if (t[j].n > t[i].n) { DlTally x = t[i]; t[i] = t[j]; t[j] = x; }
    fprintf(f, "  %s:", title);
    for (i = 0; i < nt && i < 24; i++)
        fprintf(f, " %s=%u", t[i].key, t[i].n);
    if (nt > 24)
        fprintf(f, " (+%u more)", nt - 24);
    fprintf(f, "\n");
}

static const char *const s_texmode_name[19] = {
    "none", "2D", "3D", "cube", "pass", "clip", "bumpenv", "bumpenv_lum", "brdf",
    "dot_st", "dot_zw", "dot_rflct_diff", "dot_rflct_spec", "dot_str_3d",
    "dot_str_cube", "dep_ar", "dep_gb", "dot_product", "dot_rflct_spec_const"
};

static void dl_summary(FILE *f, int which)
{
    static DlTally outc[64], reason[128], soft[128], modes[64], blend[64], fog[32], rt[64];
    unsigned no = 0, nr = 0, ns = 0, nm = 0, nb = 0, nf = 0, nrt = 0, i;
    char key[96];
    const DlRec *r = s_dl[which];
    fprintf(f, "frame_draws gen %u: %u recorded, %u over the %u cap\n",
            s_dl_gen[which], s_dl_n[which], s_dl_drop[which], DL_CAP);
    for (i = 0; i < s_dl_n[which]; i++, r++) {
        int st;
        snprintf(key, sizeof key, "%c:%s", r->path, r->outcome[0] ? r->outcome : "unknown");
        dl_tally(outc, 64, &no, key);
        if (r->reason[0])
            dl_tally(reason, 128, &nr, r->reason);
        for (st = 0; st < 4; st++) {
            uint32_t m = (r->stage_program >> (st * 5)) & 0x1F;
            if (strncmp(r->tex_res[st], "soft:", 5) == 0 || strncmp(r->tex_res[st], "stale:", 6) == 0)
                dl_tally(soft, 128, &ns, r->tex_res[st]);
            if (m) {
                snprintf(key, sizeof key, "s%d:%s", st, m < 19 ? s_texmode_name[m] : "?");
                dl_tally(modes, 64, &nm, key);
            }
        }
        if (r->blend_en)
            snprintf(key, sizeof key, "%X/%X/%X", r->blend_src, r->blend_dst, r->blend_eq);
        else
            snprintf(key, sizeof key, "off");
        dl_tally(blend, 64, &nb, key);
        if (r->fog_en)
            snprintf(key, sizeof key, "mode%X_gen%X", r->fog_mode, r->fog_gen);
        else
            snprintf(key, sizeof key, "off");
        dl_tally(fog, 32, &nf, key);
        snprintf(key, sizeof key, "%08X_%ux%u", r->surf_va, r->surf_w, r->surf_h);
        dl_tally(rt, 64, &nrt, key);
    }
    dl_tally_print(f, "outcome", outc, no);
    dl_tally_print(f, "reasons", reason, nr);
    dl_tally_print(f, "texture_stage_fallbacks", soft, ns);
    dl_tally_print(f, "texture_modes", modes, nm);
    dl_tally_print(f, "blend(src/dst/eq)", blend, nb);
    dl_tally_print(f, "fog", fog, nf);
    dl_tally_print(f, "render_targets", rt, nrt);
}

static void dl_write_rows(FILE *f, int which)
{
    unsigned i;
    const DlRec *r = s_dl[which];
    for (i = 0; i < s_dl_n[which]; i++, r++) {
        int st;
        fprintf(f, "%u,%u,%c,%016llX,%u,%s,%s,%08X,%ux%u,%u,%08X,%08X,%08X,%08X,%08X,%03X,%08X,",
                r->gen, r->seq, r->path, (unsigned long long)r->vsh, r->count,
                r->outcome[0] ? r->outcome : "unknown", r->reason,
                r->surf_va, r->surf_w, r->surf_h, r->surf_bpp, r->zeta_va,
                r->stage_program, r->rc_control, r->final0, r->final1,
                r->dot_mapping & 0xFFF, r->other_input);
        for (st = 0; st < 4; st++) {
            uint32_t m = (r->stage_program >> (st * 5)) & 0x1F;
            if (r->tex_valid[st])
                fprintf(f, "%s,%08X,%ux%u,%02X,%u,%u,%s,", m < 19 ? s_texmode_name[m] : "?",
                        r->tex_va[st], r->tex_w[st], r->tex_h[st], r->tex_fmt[st],
                        r->tex_pitch[st], r->tex_cube[st], r->tex_res[st]);
            else
                fprintf(f, "%s,,,,,,%s,", m < 19 ? s_texmode_name[m] : "?", r->tex_res[st]);
        }
        fprintf(f, "%u,%X,%X,%X,%08X,%u,%X,%u,%u,%X,%u,%u,%X,%u,%X,%u,%u,%X,%X,%06X,%g,%g",
                r->blend_en, r->blend_src, r->blend_dst, r->blend_eq, r->color_mask,
                r->alpha_en, r->alpha_func, r->alpha_ref,
                r->depth_en, r->depth_func, r->depth_write, r->cull_en, r->cull_face,
                r->stencil_en, r->stencil_func, r->stencil_ref,
                r->fog_en, r->fog_mode, r->fog_gen, r->fog_color & 0xFFFFFF,
                (double)r->fog_p0, (double)r->fog_p1);
        /* perf12 extra columns */
        for (st = 0; st < 4; st++)
            fprintf(f, ",%02X,%08X,%u,%08X", r->tex_addr[st], r->tex_filter[st],
                    r->tex_border_src[st], r->tex_border[st]);
        fprintf(f, ",");
        if (r->has_rc) {
            unsigned n = r->rc.control & 0xF, i;
            if (n > 8) n = 8;
            for (i = 0; i < n; i++)
                fprintf(f, "%s%08X/%08X/%08X/%08X/%08X/%08X", i ? "|" : "",
                        r->rc.color_icw[i], r->rc.alpha_icw[i], r->rc.color_ocw[i],
                        r->rc.alpha_ocw[i], r->rc.factor0[i], r->rc.factor1[i]);
            fprintf(f, ",%08X,%08X", r->rc.final_c0, r->rc.final_c1);
        } else {
            fprintf(f, ",,");
        }
        fprintf(f, ",%X,%g,%g", r->poff_en, (double)r->poff_scale, (double)r->poff_bias);
        if (r->has_zeq)
            fprintf(f, ",%llu,%llu,%llu", (unsigned long long)r->zeq_eq,
                    (unsigned long long)r->zeq_le, (unsigned long long)r->zeq_ge);
        else
            fprintf(f, ",,,");
        fprintf(f, "\n");
    }
}

/* DEL: settings, per-category counters for the last two frames into the
 * sidecar, and the full draw list to <png>.draws.csv. */
void halo2_debug_drawlist_write(const char *png_path, FILE *sidecar)
{
    char path[1024];
    size_t len;
    const char *dot;
    FILE *f;
    const Halo2Settings *hs = halo2_settings_get();
    int prev = s_dl_cur ^ 1;

    if (sidecar) {
        if (hs)
            fprintf(sidecar, "settings: scale=%dx fullscreen=%d vsync=%d aniso=%d fxaa=%d msaa=%d"
                             " (live: scale=%.2fx msaa=%u aniso=%d mips=%d)\n",
                    hs->resolution_scale, hs->fullscreen, hs->vsync, hs->anisotropy, hs->fxaa,
                    hs->msaa, gpu_scale_value(), (unsigned)s_msaa_want, s_tex_aniso, s_tex_mips);
        fprintf(sidecar, "last_apply: %s\n", halo2_settings_last_apply()
                ? halo2_settings_last_apply() : "(none)");
        if (s_dl[0]) {
            fprintf(sidecar, "frame_draw_summary (previous frame, then the frame being built):\n");
            dl_summary(sidecar, prev);
            dl_summary(sidecar, s_dl_cur);
        } else {
            fprintf(sidecar, "frame_draw_summary: (no draws recorded yet)\n");
        }
    }
    if (!s_dl[0] || !png_path)
        return;
    len = strlen(png_path);
    if (len + 16 >= sizeof path)
        return;
    memcpy(path, png_path, len + 1);
    dot = strrchr(path, '.');
    if (!dot)
        return;
    memcpy((char *)dot, ".draws.csv", 11);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[SCREENSHOT] could not write draw list %s\n", path);
        return;
    }
    fprintf(f, "# Halo 2 debug draw list (RECOMP_DEBUG_TOOLS): previous frame gen %u (%u draws,"
               " %u over cap) then current frame gen %u (%u draws, %u over cap)\n",
            s_dl_gen[prev], s_dl_n[prev], s_dl_drop[prev],
            s_dl_gen[s_dl_cur], s_dl_n[s_dl_cur], s_dl_drop[s_dl_cur]);
    fprintf(f, "# path V=vertex program on GPU, X=CPU-transformed batch, F=vertex program fell back"
               " to CPU. tex result: ok, stale:<why> (last-good view), soft:<why> (stage reads 0)."
               " NV2A enums: blend GL factors, compare 0x200+func, fog mode 0x2601 linear"
               " 0x800 exp 0x801 exp2.\n");
    fprintf(f, "gen,seq,path,vsh,count,outcome,reason,rt_va,rt_size,rt_bpp,zeta_va,stage_program,"
               "rc_control,final0,final1,dot_mapping,other_input");
    {
        int st;
        for (st = 0; st < 4; st++)
            fprintf(f, ",t%d_mode,t%d_va,t%d_size,t%d_fmt,t%d_pitch,t%d_cube,t%d_result",
                    st, st, st, st, st, st, st);
    }
    fprintf(f, ",blend_en,blend_src,blend_dst,blend_eq,color_mask,alpha_en,alpha_func,alpha_ref,"
               "depth_en,depth_func,depth_write,cull_en,cull_face,stencil_en,stencil_func,"
               "stencil_ref,fog_en,fog_mode,fog_gen,fog_color,fog_p0,fog_p1");
    {
        int st;
        for (st = 0; st < 4; st++)
            fprintf(f, ",t%d_addr,t%d_filter,t%d_border_src,t%d_border", st, st, st, st);
    }
    fprintf(f, ",rc_stages(cicw/aicw/cocw/aocw/f0/f1),final_c0,final_c1,poff_en(fill|line<<1|point<<2),poff_scale,poff_bias,zeq_eq,zeq_nearer,zeq_farther\n");
    dl_write_rows(f, prev);
    dl_write_rows(f, s_dl_cur);
    fclose(f);
    fprintf(stderr, "[SCREENSHOT] draw list %s\n", path);
}
#endif /* RECOMP_DEBUG_TOOLS */

#ifdef RECOMP_DEBUG_TOOLS
void halo2_debug_sidecar_gpu(FILE *f)

{
    extern unsigned s_relayout_rb_n(void), s_relayout_rb_fail_n(void);
    extern unsigned arb_stat(int);
    unsigned i, start, count;
    const Surf *presented = s_last_presented ? s_last_presented : s_cur;

    fprintf(f, "frame_gen: %u\n", s_gen);
    fprintf(f, "flips_lifetime: %u\n", s_flip_life);
    fprintf(f, "flips_in_stats_window: %u\n", s_st.flips);
    fprintf(f, "present: %ux%u (shared handle %s)\n",
            s_present_w, s_present_h, s_present_shared ? "yes" : "no");
    if (presented)
        fprintf(f, "guest_surface: va=0x%08X %ux%u physical %ux%u bpp=%u pitch=%u dirty=%d\n",
                presented->va, presented->w, presented->h, presented->pw, presented->ph,
                presented->bpp, presented->pitch, presented->gpu_dirty);
    else
        fprintf(f, "guest_surface: (none)\n");
    {
        IDXGIDevice *dxgi_device = NULL;
        IDXGIAdapter *adapter = NULL;
        DXGI_ADAPTER_DESC desc;
        LARGE_INTEGER driver_version;
        char adapter_name[512];
        char driver[64];
        HRESULT hr;
        unsigned feature = 0;
        adapter_name[0] = 0;
        driver[0] = 0;
        if (s_dev && SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dxgi_device))
            && dxgi_device) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter)) && adapter) {
                if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
                    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                             adapter_name, sizeof adapter_name, NULL, NULL))
                        snprintf(adapter_name, sizeof adapter_name, "(unreadable)");
                    /* Same query report_device_identity uses; may return
                     * DXGI_ERROR_UNSUPPORTED (0x887A0004) on some drivers. */
                    hr = IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_ID3D11Device, &driver_version);
                    if (SUCCEEDED(hr))
                        snprintf(driver, sizeof driver, "%u.%u.%u.%u",
                                 HIWORD(driver_version.HighPart), LOWORD(driver_version.HighPart),
                                 HIWORD(driver_version.LowPart), LOWORD(driver_version.LowPart));
                    else
                        snprintf(driver, sizeof driver, "unavailable:0x%08lX", (unsigned long)hr);
                    fprintf(f, "adapter: %s\n", adapter_name);
                    fprintf(f, "adapter_id: vendor=0x%04X device=0x%04X\n",
                            desc.VendorId, desc.DeviceId);
                    fprintf(f, "adapter_driver: %s\n", driver);
                    fprintf(f, "adapter_vram_bytes: %llu\n",
                            (unsigned long long)desc.DedicatedVideoMemory);
                }
                IDXGIAdapter_Release(adapter);
            } else {
                fprintf(f, "adapter: (GetAdapter failed)\n");
            }
            IDXGIDevice_Release(dxgi_device);
        } else {
            fprintf(f, "adapter: (no device)\n");
        }
        feature = (unsigned)ID3D11Device_GetFeatureLevel(s_dev);
        fprintf(f, "feature_level: 0x%04X\n", feature);
    }

    fprintf(f, "stats_window:\n");
    fprintf(f, "  draws: %u (extended %u, vsh %u raw %u cpu_fallback %u) triangles %u\n",
            s_st.draws, s_st.xdraws, s_st.vdraws, s_st.vraw, s_st.vfall, s_st.tris);
    fprintf(f, "  clears %u uploads %u rtt %u overlap_rtt %u ps_compiles %u\n",
            s_st.clears, s_st.uploads, s_st.rtt, s_st.overlap_rtt, s_st.ps_compiles);
    fprintf(f, "  declined_batches: no_color_surface=%u begin=%u guest_sync_clip=%u"
              " texture=%u (cpu_skipped=%u) combiner_shader=%u\n",
            s_xrej[0], s_xrej[1], s_xrej[2], s_xrej[3], s_texrej_skips, s_xrej[4]);
    fprintf(f, "  alias_rejects: allocation_gone=%u alias_uncertain=%u sync_failed=%u"
              " dirty_non_exact=%u dirty_beside_exact=%u strict_bounds=%u no_view=%u\n",
            s_arej[0], s_arej[1], s_arej[2], s_arej[3], s_arej[4], s_arej[5], s_arej[6]);
    fprintf(f, "  tex_unsupported: %u (cached hits %u) stale_fallback=%u soft=%u stale_surface=%u\n",
            s_st.tex_unsupported, s_st.tex_unsupported_hits,
            s_st.tex_stale_fallback, s_st.tex_soft, s_st.tex_stale_surface);
    fprintf(f, "  textures: gpu=%u cpu=%u bc=%u direct=%u stale_pitch=%u\n",
            s_st.tex_gpu, s_st.tex_cpu, s_st.tex_bc, s_st.tex_direct, s_st.tex_pitch);
    fprintf(f, "  depth: seeds=%u samples=%u snapshots=%u rejected=%u"
              " stencil_aliases=%u conversions=%u\n",
            s_st.depth_seeds, s_st.depth_samples, s_st.depth_snapshots, s_st.depth_rejected,
            s_st.stencil_samples, s_st.stencil_conversions);
    fprintf(f, "  guest_uploads=%u guest_conflicts=%u shared_presents=%u present_busy=%u\n",
            s_st.guest_uploads, s_st.guest_conflicts, s_st.present_shared, s_st.present_busy);
    fprintf(f, "  readbacks=%u coherent=%u work_batches=%u waits=%u relayout_rb=%u fail=%u"
              " write_order_wins=%u delayed_rb=%u done=%u dropped=%u msaa_resolve=%u\n",
            s_st.readbacks, s_st.readback_coherent, s_st.work_batches, s_st.work_waits,
            s_relayout_rb_n(), s_relayout_rb_fail_n(), s_order_wins,
            arb_stat(0), arb_stat(1), arb_stat(2), s_st.msaa_resolve_samples);
    fprintf(f, "recent_events (alias/unsupported/rejects, newest last, cap %u):\n", DEBUG_NOTE_MAX);
    count = s_dbg_note_n;
    start = count ? (s_dbg_note_i - count) % DEBUG_NOTE_MAX : 0;
    if (!count)
        fprintf(f, "  (none recorded)\n");
    for (i = 0; i < count; i++) {
        const DebugNote *n = &s_dbg_notes[(start + i) % DEBUG_NOTE_MAX];
        {
            const char *cls = n->soft ? "soft" : "hard";
            int st;
            if (n->kind[0] == 's' && n->kind[1] == 't' && n->kind[2] == 'a')
                cls = "stale";
            fprintf(f, "  frame %u %s %s va=0x%08X %ux%u fmt=0x%02X extra=%u %s"
                       " vsh=0x%016llX idx=%u stages=",
                    n->frame, cls, n->kind, n->va, n->w, n->h, n->fmt, n->extra, n->detail,
                    (unsigned long long)n->vsh, n->indices);
            for (st = 0; st < 4; st++)
                fprintf(f, " [%d:va=0x%08X %ux%u fmt=0x%02X mode=%u]",
                        st, n->stage_va[st], n->stage_w[st], n->stage_h[st],
                        n->stage_fmt[st], n->stage_mode[st]);
            fprintf(f, " block=va=0x%08X %ux%u pitch=%u bpp=%u wserial=%llu age=%u",
                    n->block_va, n->block_w, n->block_h, n->block_pitch, n->block_bpp,
                    (unsigned long long)n->block_wserial, n->block_age);
            fprintf(f, "\n");
        }
    }
}
#endif


static int async_rb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_ASYNC_READBACK");
        on = e && e[0] == '1';   /* opt-in: guest memory lags the presented frame by one flip */
        if (on && s_direct_present)
            fprintf(stderr, "[GPU-D3D11] delayed flip readback enabled (shared-present flips;"
                            " guest framebuffer lags one flip)\n");
    }
    return on && !rpz_ab_off(256);
}

static int arb_ensure(const Surf *sf)
{
    int i;
    if (s_arb.tex[0] && s_arb.w == sf->w && s_arb.h == sf->h && s_arb.bpp == sf->bpp)
        return 1;
    if (s_arb.pending)
        return 0;
    for (i = 0; i < 2; ++i) {
        if (s_arb.tex[i]) ID3D11Texture2D_Release(s_arb.tex[i]);
        s_arb.tex[i] = NULL;
    }
    for (i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = sf->w;
        td.Height = sf->h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = surf_format(sf->bpp);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_arb.tex[i]))) {
            s_arb.tex[i] = NULL;
            return 0;
        }
    }
    s_arb.w = sf->w;
    s_arb.h = sf->h;
    s_arb.bpp = sf->bpp;
    return 1;
}

/* Write the pending frame into guest memory (it finished on the GPU a frame ago). */
static void arb_complete(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    Surf *sf;
    HRESULT hr;
    size_t row;
    uint32_t y;

    if (!s_arb.pending)
        return;
    s_arb.pending = 0;
    sf = find_surf(s_arb.va);
    if (!sf || sf->w != s_arb.w || sf->h != s_arb.h || sf->bpp != s_arb.bpp ||
        sf->pitch != s_arb.pitch || !same_guest_identity(&sf->identity, &s_arb.id) ||
        !ensure_guest_shadow(sf)) {
        s_arb_dropped++;
        return;
    }
    hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_arb.tex[s_arb.slot], 0,
                                D3D11_MAP_READ, 0, &m);
    gpu_d3d11_check_device(s_dev, hr, "mapping delayed readback");
    if (FAILED(hr)) {
        s_arb_dropped++;
        return;
    }
    row = surf_guest_row(sf);
    if (checked_write_guest_surface(sf, m.pData, m.RowPitch)) {
        for (y = 0; y < sf->h; ++y)
            memcpy(sf->guest_shadow + (size_t)y * row,
                   (const uint8_t *)m.pData + (size_t)y * m.RowPitch, row);
        sf->guest_check_gen = s_gen;
        sf->guest_exact = 0;
        if (sf->wserial == s_arb.wserial) {   /* not drawn since the copy */
            sf->gpu_dirty = 0;
            sf->alias_uncertain = 0;
        }
        s_arb_done++;
        if (arbdbg())
            fprintf(stderr, "[ARBDBG]   wrote %08X guest now %08X\n", sf->va, arbdbg_sum(sf->va, sf->pitch, sf->h));
    } else {
        sf->guest_check_gen = 0;
        s_arb_dropped++;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_arb.tex[s_arb.slot], 0);
}

static unsigned s_arbdbg_n;
static int arbdbg(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_ARB_DEBUG"); on = e && e[0] == '1'; }
    return on && (s_arbdbg_n % 1500) < 10;
}
static uint32_t arbdbg_sum(uint32_t va, uint32_t pitch, uint32_t h)
{
    const uint32_t *p = (const uint32_t *)GUEST(va + pitch * (h / 2));
    uint32_t i, x = 0;
    for (i = 0; i < 64; ++i) x = x * 31 + p[i * 2];
    return x;
}
static int d3d_readback(uint32_t color_va)
{
    Surf *sf = find_surf(color_va);
    ID3D11Texture2D *source;
    D3D11_MAPPED_SUBRESOURCE m;
    GuestSpanIdentity identity;
    uint32_t y, row;
    double t0 = qpc_ms();
    HRESULT hr;
    int ok = 0, counting, exact, matches = 1, fast = 0;

    if (s_dev)
        gpu_d3d11_check_device(s_dev, S_OK, "starting surface readback");
    if (!sf || !s_dev)
        return 0;
    gpu_work_begin();
    s_last_presented = sf;
    update_native_output(sf);
    counting = query_suspend_clear();
    if (!sync_guest_surface(sf, 1))
        goto done;
    publish_shared_present(sf);
#ifdef RECOMP_DEBUG_TOOLS
    if (halo2_screenshot_enabled()) {
        if (!s_shared_ok)
            halo2_screenshot_try_copy(s_dev, s_ctx, sf->tex, sf->pw, sf->ph, surf_format(sf->bpp));
        halo2_screenshot_finalize(s_ctx);
    }
#endif
    s_arbdbg_n++;
    if (arbdbg())
        fprintf(stderr, "[ARBDBG] #%u gen %u rb va %08X %ux%u p%u bpp%u pw%u ph%u shared %d pend %d pva %08X dirty %d guest %08X\n",
                s_arbdbg_n, (unsigned)s_gen, color_va, sf->w, sf->h, sf->pitch, sf->bpp, sf->pw, sf->ph,
                s_shared_ok, s_arb.pending, s_arb.va, sf->gpu_dirty, arbdbg_sum(sf->va, sf->pitch, sf->h));
    if (s_shared_ok && async_rb_enabled() && (sf->bpp == 4 || sf->bpp == 2)) {
        ID3D11Texture2D *src;
        arb_complete();
        src = readback_source(sf);
        if (src && arb_ensure(sf)) {
            int slot = s_arb.cur;
            s_arb.cur ^= 1;
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_arb.tex[slot],
                                             (ID3D11Resource *)src);
            s_arb.slot = slot;
            s_arb.pending = 1;
            s_arb.va = sf->va;
            s_arb.pitch = sf->pitch;
            s_arb.id = sf->identity;
            s_arb.wserial = sf->wserial;
            sf->guest_exact = 0;
            s_arb_async++;
            s_st.readbacks++;
            s_st.readback_ms += qpc_ms() - t0;
            ok = 1;
            goto done;
        }
    }
    arb_complete();
    if (s_skip_clean_readback && !gpu_scale_requested() && sf->guest_exact && !sf->gpu_dirty &&
        !getenv("RECOMP_FRAME_STATS") && !getenv("RECOMP_FB_DUMP") &&
        !getenv("RECOMP_FB_DUMP_FLIPS") && !getenv("RECOMP_PRESENTER_CAPTURE")) {
        gpu_work_finish();
#ifdef BRUTEFORCE_D3D11_TESTING
        if (scale_test_failure("guest-change-wait"))
            *(uint32_t *)GUEST(sf->va) ^= 1u;
#endif
        if (!guest_surface_changed(sf)) {
            s_st.readback_coherent++;
            s_st.readback_ms += qpc_ms() - t0;
            ok = 1;
            goto done;
        }
        sf->guest_exact = 0;
    }
    source = readback_source(sf);
    if (!source)
        goto done;
    if (!s_staging || s_staging_w != sf->w || s_staging_h != sf->h || s_staging_bpp != sf->bpp) {
        ID3D11Texture2D *candidate;
        if (!create_staging_texture(sf, &candidate))
            goto done;
        install_staging_texture(candidate, sf);
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_staging,
                                     (ID3D11Resource *)source);
    {
        RPZ_BEGIN(rpz_map);
        hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                    D3D11_MAP_READ, 0, &m);
        RPZ_END(RPZ_GPU_WAIT, rpz_map);
    }
    if (scale_test_failure("readback-loss"))
        hr = DXGI_ERROR_DEVICE_REMOVED;
    gpu_d3d11_check_device(s_dev, hr, "mapping surface readback");
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] readback map failed 0x%08lX\n", (unsigned long)hr);
        goto done;
    }
    row = sf->w * sf->bpp;
    if (row > sf->pitch)
        row = sf->pitch;
    if (!checked_write_guest_surface(sf, m.pData, m.RowPitch)) {
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
        sf->guest_check_gen = 0;
        sf->gpu_dirty = 1;
        sf->guest_exact = 0;
        sf->alias_uncertain = 1;
        goto done;
    }
#ifdef BRUTEFORCE_D3D11_TESTING
    if (scale_test_failure("guest-change-readback"))
        *(uint32_t *)GUEST(sf->va) ^= 1u;
    if (scale_test_failure("guest-guard-readback")) {
        DWORD old_protection;
        VirtualProtect(GUEST(sf->va), 1, PAGE_READWRITE | PAGE_GUARD, &old_protection);
    }
#endif
    fast = surf_fast_enabled();
    if (fast ? !surf_identity_current(sf)
             : (!copy_guest_surface(sf, &identity) || !same_guest_identity(&sf->identity, &identity))) {
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
        sf->guest_check_gen = 0;
        sf->gpu_dirty = 1;
        sf->guest_exact = 0;
        sf->alias_uncertain = 1;
        goto done;
    }
    for (y = 0; y < sf->h; y++) {
        const uint8_t *gpu_row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        if (!fast && memcmp(s_surface_probe + (size_t)y * sf->pitch, gpu_row, row))
            matches = 0;
        memcpy(sf->guest_shadow + (size_t)y * row, gpu_row, row);
    }
    exact = matches && s_skip_clean_readback && sf->bpp == 4 && sf->pitch >= sf->w * 4 &&
            sf->pw == sf->w && sf->ph == sf->h;
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
    sf->guest_check_gen = matches ? s_gen : 0;
    sf->gpu_dirty = !matches;
    sf->guest_exact = exact;
    sf->alias_uncertain = !matches;
    s_st.readbacks++;
    s_st.readback_ms += qpc_ms() - t0;
    gpu_d3d11_check_device(s_dev, S_OK, "completing surface readback");
    if (!matches) {
        static unsigned rejected;
        if (rejected < 16) {
            ++rejected;
            fprintf(stderr, "[GPU-D3D11] guest color surface %08X changed during readback%s\n",
                    sf->va, rejected == 16 ? " (further reports suppressed)" : "");
        }
        goto done;
    }
    ok = 1;
done:
    if (s_cur)
        bind_targets(s_cur, s_cur_depth);
    query_resume_clear(counting);
    return ok;
}

static int d3d_flip_readback(uint32_t color_va)
{
    if (rtlog_on())
        fprintf(stderr, "[RTLOG] g%u flip readback %08X\n", s_gen, color_va);
    int tracked = find_surf(color_va) != NULL;
    int ok = d3d_readback(color_va);
    if (tracked && !ok) {
        fprintf(stderr, "[GPU-D3D11] cannot make tracked color surface %08X guest-visible;"
                        " stopping before presentation, dumps or frame statistics\n", color_va);
        fflush(stderr);
        abort();
    }
    return ok;
}

#ifdef BRUTEFORCE_D3D11_TESTING
int bruteforce_d3d11_test_snapshot(uint32_t color_va, uint32_t depth_va,
                                   BruteforceD3D11TestSnapshot *snapshot)
{
    Surf *color = find_surf(color_va);
    Depth *depth = NULL;
    int i;
    if (!snapshot || !color)
        return 0;
    for (i = 0; i < s_ndepth; ++i)
        if (s_depth[i].va == depth_va) { depth = &s_depth[i]; break; }
    if (depth_va && !depth)
        return 0;
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->color = (uintptr_t)color->tex;
    snapshot->color_rtv = (uintptr_t)color->rtv;
    snapshot->color_srv = (uintptr_t)color->srv;
    snapshot->color_width = color->pw;
    snapshot->color_height = color->ph;
    if (depth) {
        snapshot->depth = (uintptr_t)depth->tex;
        snapshot->depth_dsv = (uintptr_t)depth->dsv;
        snapshot->depth_width = depth->w;
        snapshot->depth_height = depth->h;
    }
    snapshot->shared = (uintptr_t)s_present_shared;
    snapshot->shared_rtv = (uintptr_t)s_present_rtv;
    snapshot->shared_mutex = (uintptr_t)s_present_mutex;
    snapshot->shared_width = s_present_w;
    snapshot->shared_height = s_present_h;
    snapshot->resolve = (uintptr_t)s_readback_resolve;
    snapshot->resolve_rtv = (uintptr_t)s_readback_resolve_rtv;
    snapshot->resolve_width = s_readback_resolve_w;
    snapshot->resolve_height = s_readback_resolve_h;
    snapshot->staging = (uintptr_t)s_staging;
    snapshot->bound_color = (uintptr_t)(s_cur ? s_cur->tex : NULL);
    snapshot->bound_depth = (uintptr_t)(s_cur_depth ? s_cur_depth->tex : NULL);
    snapshot->scale = gpu_scale_value();
    snapshot->epoch = gpu_scale_epoch();
    return 1;
}

int bruteforce_d3d11_test_shared_busy(int busy)
{
    HRESULT hr;
    if (!s_present_mutex)
        return 0;
    hr = IDXGIKeyedMutex_AcquireSync(s_present_mutex, busy ? 0 : 1, 0);
    if (hr == S_OK)
        hr = IDXGIKeyedMutex_ReleaseSync(s_present_mutex, busy ? 1 : 0);
    return hr == S_OK;
}

int bruteforce_d3d11_test_readback(uint32_t color_va)
{
    return d3d_readback(color_va);
}

int bruteforce_d3d11_test_pipeline(uint32_t color_va, uint32_t depth_va,
                                    BruteforceD3D11PipelineStats *stats)
{
    Surf *color = color_va ? find_surf(color_va) : NULL;
    Depth *depth = NULL;
    int i;
    if (!stats || (color_va && !color))
        return 0;
    for (i = 0; i < s_ndepth; ++i)
        if (s_depth[i].va == depth_va) { depth = &s_depth[i]; break; }
    if (depth_va && !depth)
        return 0;
    memset(stats, 0, sizeof *stats);
    stats->readbacks = s_st.readbacks;
    stats->coherent = s_st.readback_coherent;
    stats->work_submitted = s_work_submitted;
    stats->work_waits = s_work_wait_count;
    stats->work_queued = s_work_count;
    stats->work_peak = s_work_peak;
    stats->work_operations = s_work_operations;
    stats->injected_pending = s_work_test_pending_seen;
    if (color) {
        stats->gpu_dirty = color->gpu_dirty;
        stats->guest_exact = color->guest_exact;
    }
    if (depth) {
        stats->depth_revision = depth->revision;
        stats->packed_revision = depth->packed_revision;
        stats->packed_valid = depth->packed_valid;
        stats->depth_gpu_dirty = depth->gpu_dirty;
        stats->depth_conflict = depth->guest_conflict;
        stats->depth_initialized = depth->initialized;
        stats->stencil_conversions = s_st.stencil_conversions;
    }
    return 1;
}

void bruteforce_d3d11_test_pending_work(unsigned polls)
{
    s_work_test_pending = polls;
}

void bruteforce_d3d11_test_finish_work(void)
{
    gpu_work_finish();
}
#endif

static void d3d_flip(void)
{
    pump_live_settings();
    if (s_dev) {
        gpu_d3d11_check_device(s_dev, S_OK, "completing flip");
        gpu_work_begin();
    }
    update_native_output(s_last_presented ? s_last_presented : s_cur);
    if (s_dev)
        gpu_work_checkpoint();
#ifdef RECOMP_DEBUG_TOOLS
    dl_flip();
#endif
    s_gen++;
    stats_flip();
}

#ifdef BRUTEFORCE_D3D11_TESTING
void bruteforce_d3d11_test_flip(void) { d3d_flip(); }
ID3D11Device *bruteforce_d3d11_test_device(void) { return s_dev; }
ID3D11DeviceContext *bruteforce_d3d11_test_context(void) { return s_ctx; }
int bruteforce_d3d11_test_texture_index(void)
{
    Nv2aTexture first = {0}, second, candidate, cube, replacement;
    uint32_t *seen = NULL;
    uint32_t first_offset = 0, second_offset = 0;
    unsigned bucket, i, probes;
    int saved_mips = s_tex_mips;
    int saved_index_enabled = s_tex_index_enabled;
    int ok = 0;
    Tex *entry;

#define INDEX_CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); goto done; } \
} while (0)

    INDEX_CHECK(s_tex && s_ntex == 0, "texture index starts empty");
    s_tex_index_enabled = 1;
    seen = (uint32_t *)malloc(sizeof *seen * TEX_INDEX_BUCKETS);
    INDEX_CHECK(seen != NULL, "allocate texture index collision table");
    for (i = 0; i < TEX_INDEX_BUCKETS; ++i)
        seen[i] = UINT32_MAX;

    first.width = first.height = 4;
    first.pitch = 16;
    first.color = 0x12;
    first.levels = 1;
    for (i = 0; i <= TEX_INDEX_BUCKETS; ++i) {
        candidate = first;
        candidate.offset = 0x01000000u + i * 0x80u;
        bucket = tex_key_bucket(&candidate, 0, 0, 1);
        if (seen[bucket] != UINT32_MAX) {
            first_offset = seen[bucket];
            second_offset = candidate.offset;
            break;
        }
        seen[bucket] = candidate.offset;
    }
    INDEX_CHECK(first_offset && second_offset, "find texture descriptor collision");
    first.offset = first_offset;
    second = first;
    second.offset = second_offset;

    entry = tex_slot(NULL);
    memset(entry, 0, sizeof *entry);
    set_tex_key(entry, &first, 0, 0, 1);
    entry->last_use = 1;
    entry = tex_slot(NULL);
    memset(entry, 0, sizeof *entry);
    set_tex_key(entry, &second, 0, 0, 1);
    entry->last_use = 2;

    probes = 0;
    INDEX_CHECK(tex_index_find(&first, 0, 0, 1, &probes) == &s_tex[0] && probes == 2,
                "collision chain finds first descriptor");
    probes = 0;
    INDEX_CHECK(tex_index_find(&second, 0, 0, 1, &probes) == &s_tex[1] && probes == 1,
                "collision chain finds second descriptor");
    candidate = first; candidate.width++;
    INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 1, NULL), "width is indexed");
    candidate = first; candidate.height++;
    INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 1, NULL), "height is indexed");
    candidate = first; candidate.pitch += 4;
    INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 1, NULL), "pitch is indexed");
    candidate = first; candidate.color++;
    INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 1, NULL), "format is indexed");
    candidate = first; candidate.palette = 0x02000000u;
    INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 1, NULL), "palette address is indexed");
    INDEX_CHECK(!tex_index_find(&first, 0, 32, 1, NULL), "palette length is indexed");
    INDEX_CHECK(!tex_index_find(&first, 1, 0, 1, NULL), "cube state is indexed");
    candidate = first; candidate.levels = 2;
    INDEX_CHECK(tex_index_find(&candidate, 0, 0, 2, NULL) == &s_tex[0],
                "level fields remain ignored for level-zero 2D mode");

    entry = tex_index_find(&first, 0, 0, 1, NULL);
    entry = tex_slot(entry);
    memset(entry, 0, sizeof *entry);
    set_tex_key(entry, &first, 0, 0, 1);
    entry->last_use = 3;
    probes = 0;
    INDEX_CHECK(tex_index_find(&first, 0, 0, 1, &probes) == entry && probes == 1,
                "rewritten descriptor is reindexed");
    probes = 0;
    INDEX_CHECK(tex_index_find(&second, 0, 0, 1, &probes) == &s_tex[1] && probes == 2,
                "collision chain survives rewrite");

    cube = first;
    cube.offset = 0x03000000u;
    cube.levels = 3;
    entry = tex_slot(NULL);
    memset(entry, 0, sizeof *entry);
    set_tex_key(entry, &cube, 1, 0, 1);
    entry->last_use = 4;
    candidate = cube; candidate.levels = 2;
    INDEX_CHECK(!tex_index_find(&candidate, 1, 0, 1, NULL),
                "cube level count is indexed");
    discard_tex(tex_index_find(&second, 0, 0, 1, NULL));
    INDEX_CHECK(!tex_index_find(&second, 0, 0, 1, NULL),
                "discard removes descriptor");
    INDEX_CHECK(tex_index_find(&cube, 1, 0, 1, NULL) == &s_tex[1],
                "discard reindexes relocated entry");

    s_tex[0].last_use = 0;
    s_tex[1].last_use = 1;
    while (s_ntex < MAX_TEX) {
        candidate = first;
        candidate.offset = 0x10000000u + (uint32_t)s_ntex * 0x100u;
        INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 1, NULL),
                    "full-cache fixture keys are unique");
        entry = tex_slot(NULL);
        memset(entry, 0, sizeof *entry);
        set_tex_key(entry, &candidate, 0, 0, 1);
        entry->last_use = (uint32_t)s_ntex + 1;
    }
    replacement = first;
    replacement.offset = 0x70000000u;
    entry = tex_slot(NULL);
    memset(entry, 0, sizeof *entry);
    set_tex_key(entry, &replacement, 0, 0, 1);
    INDEX_CHECK(!tex_index_find(&first, 0, 0, 1, NULL),
                "LRU eviction removes old descriptor");
    INDEX_CHECK(tex_index_find(&replacement, 0, 0, 1, NULL) == entry,
                "LRU replacement is indexed");
    INDEX_CHECK(tex_index_find(&cube, 1, 0, 1, NULL) != NULL,
                "LRU replacement preserves other descriptors");

    memset(s_tex_index, 0, sizeof s_tex_index);
    memset(s_tex, 0, MAX_TEX * sizeof *s_tex);
    s_ntex = 0;
    s_tex_mips = 1;
    candidate = first;
    candidate.offset = 0x71000000u;
    candidate.levels = 3;
    entry = tex_slot(NULL);
    memset(entry, 0, sizeof *entry);
    set_tex_key(entry, &candidate, 0, 0, 3);
    INDEX_CHECK(tex_index_find(&candidate, 0, 0, 3, NULL) == entry,
                "mip descriptor is indexed");
    candidate.levels = 2;
    INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 3, NULL),
                "mip declared level count is indexed");
    candidate.levels = 3;
    INDEX_CHECK(!tex_index_find(&candidate, 0, 0, 2, NULL),
                "mip uploaded level count is indexed");

    puts("texture descriptor index passed");
    ok = 1;
done:
    free(seen);
    if (s_tex) {
        for (i = 0; i < (unsigned)s_ntex; ++i)
            release_tex(&s_tex[i]);
        memset(s_tex, 0, MAX_TEX * sizeof *s_tex);
    }
    memset(s_tex_index, 0, sizeof s_tex_index);
    s_ntex = 0;
    s_tex_snapshot_bytes = 0;
    s_tex_mips = saved_mips;
    s_tex_index_enabled = saved_index_enabled;
#undef INDEX_CHECK
    return ok;
}
ID3D11ShaderResourceView *bruteforce_d3d11_test_texture(const Nv2aTexture *texture,
                                                         int cube, unsigned stage,
                                                         unsigned *levels)
{
    float width, height;
    return get_texture(texture, cube, stage, &width, &height, levels);
}
ID3D11SamplerState *bruteforce_d3d11_test_sampler(uint32_t addr_u, uint32_t addr_v,
                                                   uint32_t filter, uint32_t control0,
                                                   unsigned levels)
{
    return get_sampler(addr_u, addr_v, filter, control0, levels, 0, 0);
}
#endif

static const Nv2aBackend s_backend = { d3d_clear, d3d_draw, d3d_flip };
static const Nv2aBackendExt s_backend_ext = { d3d_xdraw, d3d_xdraw_vsh, d3d_vsh_readback,
                                              d3d_vsh_raw_ok, d3d_flip_readback, d3d_zpass,
                                              d3d_zpass_count, d3d_zpass_report, d3d_poll,
                                              NV2A_BACKEND_CAP_WINDOW_CLIP };

int bruteforce_d3d11_install(void)
{
    static int installed;
    const char *warp = getenv("RECOMP_D3D11_WARP");
    const char *mulzero = getenv("RECOMP_GPU_VSH_MULZERO");
    const char *tex_gpu = getenv("RECOMP_GPU_TEX_DECODE");
    const char *tex_log = getenv("RECOMP_GPU_TEXLOG");
    const char *tex_approx = getenv("RECOMP_GPU_TEX_CACHE_APPROX");
    const char *tex_index = getenv("RECOMP_GPU_TEX_INDEX");
    const char *tex_bounds = getenv("RECOMP_GPU_TEXTURE_BOUNDS");
    const char *tex_mips = getenv("RECOMP_GPU_MIPS");
    const char *tex_aniso = getenv("RECOMP_GPU_ANISO");
    const char *tex_fxaa = getenv("RECOMP_GPU_FXAA");
    const char *tex_msaa = getenv("RECOMP_GPU_MSAA");
    const char *shader_diag = getenv("RECOMP_GPU_SHADER_DIAG");
    const char *depth_texture = getenv("RECOMP_GPU_DEPTH_TEXTURE");
    const char *stencil_alias = getenv("RECOMP_GPU_STENCIL_ALIAS");
    const char *direct_present = getenv("RECOMP_GPU_DIRECT_PRESENT");
    const char *skip_clean_readback = getenv("RECOMP_GPU_SKIP_CLEAN_READBACK");
    GpuScaleConfig scale;
    Tex *tex;

    if (installed)
        return 1;
#ifdef RECOMP_DEBUG_TOOLS
    halo2_screenshot_runtime_init();
#endif

    if (depth_texture && *depth_texture && strcmp(depth_texture, "0") != 0 &&
        strcmp(depth_texture, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_DEPTH_TEXTURE must be 0 or 1\n");
        return 0;
    }
    if (stencil_alias && *stencil_alias && strcmp(stencil_alias, "0") != 0 &&
        strcmp(stencil_alias, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_STENCIL_ALIAS must be 0 or 1\n");
        return 0;
    }
    if (direct_present && *direct_present && strcmp(direct_present, "0") != 0 &&
        strcmp(direct_present, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_DIRECT_PRESENT must be 0 or 1\n");
        return 0;
    }
    if (skip_clean_readback && *skip_clean_readback &&
        strcmp(skip_clean_readback, "0") != 0 && strcmp(skip_clean_readback, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_SKIP_CLEAN_READBACK must be 0 or 1\n");
        return 0;
    }
    if (tex_approx && *tex_approx && strcmp(tex_approx, "0") != 0 &&
        strcmp(tex_approx, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_TEX_CACHE_APPROX must be 0 or 1\n");
        return 0;
    }
    if (tex_index && *tex_index && strcmp(tex_index, "0") != 0 &&
        strcmp(tex_index, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_TEX_INDEX must be 0 or 1\n");
        return 0;
    }
    if (tex_bounds && *tex_bounds && strcmp(tex_bounds, "0") != 0 &&
        strcmp(tex_bounds, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_TEXTURE_BOUNDS must be 0 or 1\n");
        return 0;
    }
    if (tex_mips && *tex_mips && strcmp(tex_mips, "0") != 0 &&
        strcmp(tex_mips, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_MIPS must be 0 or 1\n");
        return 0;
    }
    if (tex_aniso && *tex_aniso &&
        strcmp(tex_aniso, "0") != 0 && strcmp(tex_aniso, "1") != 0 &&
        strcmp(tex_aniso, "2") != 0 && strcmp(tex_aniso, "4") != 0 &&
        strcmp(tex_aniso, "8") != 0 && strcmp(tex_aniso, "16") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_ANISO must be 0, 1 (legacy 8x), 2, 4, 8 or 16\n");
        return 0;
    }
    if (tex_fxaa && *tex_fxaa && strcmp(tex_fxaa, "0") != 0 && strcmp(tex_fxaa, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_FXAA must be 0 or 1\n");
        return 0;
    }
    if (tex_msaa && *tex_msaa &&
        strcmp(tex_msaa, "0") != 0 && strcmp(tex_msaa, "1") != 0 &&
        strcmp(tex_msaa, "2") != 0 && strcmp(tex_msaa, "4") != 0 &&
        strcmp(tex_msaa, "8") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_MSAA must be 0, 1 (off), 2, 4 or 8\n");
        return 0;
    }
    if (shader_diag && *shader_diag && strcmp(shader_diag, "0") != 0 &&
        strcmp(shader_diag, "1") != 0) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_GPU_SHADER_DIAG must be 0 or 1\n");
        return 0;
    }
    if (!gpu_scale_prepare_config(&scale))
        return 0;
    if (scale.requested && direct_present && strcmp(direct_present, "0") == 0) {
        fprintf(stderr, "[GPU-SCALE] scaled rendering requires shared GPU presentation;"
                        " RECOMP_GPU_DIRECT_PRESENT cannot be 0\n");
        return 0;
    }
    if (scale.requested &&
        ((depth_texture && strcmp(depth_texture, "1") == 0) ||
         (stencil_alias && strcmp(stencil_alias, "1") == 0))) {
        fprintf(stderr, "[GPU-SCALE] scaled depth/stencil texture aliases are not supported;"
                        " disable RECOMP_GPU_DEPTH_TEXTURE and RECOMP_GPU_STENCIL_ALIAS\n");
        return 0;
    }
    if (!gpu_pscache_init())
        return 0;
    tex = (Tex *)calloc(MAX_TEX, sizeof *tex);
    if (!tex) {
        fprintf(stderr, "[GPU-D3D11] cannot allocate the D3D11 texture cache;"
                        " configuration not committed\n");
        return 0;
    }
    if (!scale.requested && !gpu_scale_commit_config(&scale)) {
        fprintf(stderr, "[GPU-SCALE] cannot commit D3D11 configuration\n");
        free(tex);
        return 0;
    }
    if (scale.requested) {
        s_pending_scale = scale;
        s_scale_pending = 1;
    }
    s_warp = warp && strcmp(warp, "1") == 0;
    s_tex = tex;
    s_ntex = 0;
    s_tex_snapshot_bytes = 0;
    memset(s_tex_index, 0, sizeof s_tex_index);
    s_depth_texture = depth_texture && strcmp(depth_texture, "1") == 0;
    s_stencil_alias = stencil_alias && strcmp(stencil_alias, "1") == 0;
    s_skip_clean_readback = skip_clean_readback && strcmp(skip_clean_readback, "1") == 0;
    s_direct_present = (direct_present && strcmp(direct_present, "1") == 0) ||
                       (scale.requested && (!direct_present || !*direct_present));
    s_tex_gpu = !tex_gpu || strcmp(tex_gpu, "0") != 0;
    s_tex_log = tex_log && strcmp(tex_log, "1") == 0;
    s_tex_approx = tex_approx && strcmp(tex_approx, "1") == 0;
    s_tex_index_enabled = !tex_index || strcmp(tex_index, "0") != 0;
    s_tex_strict_bounds = tex_bounds && strcmp(tex_bounds, "1") == 0;
    const char *physical_reads = getenv("RECOMP_GPU_PHYSICAL_TEXTURE_READS");
    s_tex_physical_read = physical_reads && strcmp(physical_reads, "1") == 0;
    s_tex_mips = tex_mips && strcmp(tex_mips, "1") == 0;
    if (!tex_aniso || !*tex_aniso || strcmp(tex_aniso, "0") == 0)
        s_tex_aniso = 0;
    else if (strcmp(tex_aniso, "1") == 0)
        s_tex_aniso = 8; /* legacy RECOMP_GPU_ANISO=1 */
    else
        s_tex_aniso = atoi(tex_aniso);
    if (s_tex_aniso >= 2 && !s_tex_mips)
        s_tex_mips = 1;
    halo2_fxaa_set_enabled(tex_fxaa && strcmp(tex_fxaa, "1") == 0);
    if (halo2_fxaa_enabled() && !(direct_present && strcmp(direct_present, "0") == 0))
        s_direct_present = 1;
    if (!tex_msaa || !*tex_msaa || strcmp(tex_msaa, "0") == 0 || strcmp(tex_msaa, "1") == 0)
        s_msaa_want = 1;
    else
        s_msaa_want = (UINT)atoi(tex_msaa);
    s_shader_diag = shader_diag && strcmp(shader_diag, "1") == 0;
    if (s_tex_approx)
        fprintf(stderr, "[GPU-D3D11] experimental approximate texture cache enabled;"
                        " texture rewrites can be missed\n");
    if (!s_tex_index_enabled)
        fprintf(stderr, "[GPU-D3D11] exact texture descriptor index disabled;"
                        " using diagnostic linear reference lookup\n");
    if (s_tex_strict_bounds)
        fprintf(stderr, "[GPU-D3D11] texture spans require registered logical bounds\n");
    if (s_tex_mips)
        fprintf(stderr, "[GPU-MIPS] experimental guest mip chains enabled;"
                        " CPU reference remains level 0\n");
    if (s_tex_aniso)
        fprintf(stderr, "[GPU-MIPS] anisotropic filtering %dx %s\n",
                s_tex_aniso,
                s_tex_mips ? "enabled for eligible mipped samplers"
                           : "inactive without mip chains");
    if (s_msaa_want > 1)
        fprintf(stderr, "[GPU-D3D11] requested MSAA %ux (clamped to device support at create)\n",
                (unsigned)s_msaa_want);
    if (halo2_fxaa_enabled())
        fprintf(stderr, "[FXAA] enabled (applied at present)\n");
    s_vsh_flags = mulzero && strcmp(mulzero, "1") == 0 ? GPU_VSH_MULZERO : 0;
    s_vsh_sync = getenv("RECOMP_GPU_VSH_SYNC") && strcmp(getenv("RECOMP_GPU_VSH_SYNC"), "1") == 0;
    s_zpass_async = getenv("RECOMP_GPU_ZPASS") && strcmp(getenv("RECOMP_GPU_ZPASS"), "async") == 0;
    nv2a_backend_ext_register(&s_backend_ext);
    nv2a_backend_register(&s_backend);
    installed = 1;
    fprintf(stderr, "[GPU] Direct3D 11 back end registered (RECOMP_GPU=d3d11%s)\n",
            s_warp ? ", WARP" : "");
    if (s_depth_texture)
        fprintf(stderr, "[GPU-D3D11] experimental D24 depth texture aliases enabled\n");
    if (s_stencil_alias)
        fprintf(stderr, "[GPU-D3D11] experimental packed D24S8 texture aliases enabled\n");
    if (s_direct_present)
        fprintf(stderr, "[GPU-D3D11] shared GPU presentation enabled;"
                        " guest visibility retained\n");
    if (s_skip_clean_readback)
        fprintf(stderr, "[GPU-D3D11] exact guest-coherent 32-bit readback elision enabled;"
                        " dirty, scaled and diagnostic frames retain readback\n");
    return 1;
}
