/*
 * D3D11 presentation for the framebuffer window (fb_present.c).
 *
 * GDI shows each frame with StretchDIBits on a 16 ms timer, scaled on the
 * CPU. This puts a D3D11 device and a swap chain on the same window and
 * presents the flipped frame through it, vsync-paced. The frame (the guest's
 * A8R8G8B8, i.e. B8G8R8A8 in memory order) is uploaded into its own texture
 * and drawn into a back buffer the size of the window's client area,
 * letterboxed to the frame's aspect ratio on black -- the window can be any
 * size (or borderless fullscreen) without stretching the picture.
 *
 * Opt-in with RECOMP_D3D_PRESENT=1; anything that fails falls back to GDI.
 * Linear filtering, RECOMP_PRESENT_FILTER=point for sharp pixels;
 * RECOMP_PRESENT_VSYNC=0 presents without waiting for the display, as the
 * SDL window does.
 */
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_present.h"

static ID3D11Device             *s_dev;
static ID3D11DeviceContext      *s_ctx;
static IDXGISwapChain           *s_chain;
/* Frames presented: a title's freeze watch reads it (Halo 2's h2_diag.c). */
volatile LONG g_gpu_present_count;
static ID3D11RenderTargetView   *s_rtv;
static ID3D11Texture2D          *s_frame;      /* the guest frame, w x h */
static ID3D11ShaderResourceView *s_srv;
static ID3D11SamplerState       *s_samp;
static ID3D11VertexShader       *s_vs;
static ID3D11PixelShader        *s_ps;
static HWND                      s_hwnd;
static uint32_t                  s_w, s_h;      /* frame size */
static uint32_t                  s_bw, s_bh;    /* back buffer (client) size */
static int                       s_vsync = 1;
static volatile LONG             s_shared_ready;
static SRWLOCK                   s_shared_lock = SRWLOCK_INIT;
static HANDLE                    s_pending_shared, s_shared_handle;
static uint64_t                  s_pending_frame_id;
static uint32_t                  s_pending_width, s_pending_height;
static ID3D11Texture2D          *s_shared_texture;
static ID3D11ShaderResourceView *s_shared_srv;
static IDXGIKeyedMutex          *s_shared_mutex;
static uint32_t                  s_shared_width, s_shared_height;
/* The last GPU frame, copied out of the shared texture. Shown again when the
 * window loop repeats a frame (the title below 60 fps, the loop at 60 Hz):
 * with no new shared frame the repeat went out as the 640x480 CPU copy, and
 * those blurry repeats between sharp frames were the menu flicker at 4x-5x
 * (a test laptop, 2026-11-25). */
static ID3D11Texture2D          *s_keep_tex;
static ID3D11ShaderResourceView *s_keep_srv;
static uint32_t                  s_keep_w, s_keep_h;
static uint64_t                  s_keep_id;

static void release_keep(void)
{
    if (s_keep_srv) ID3D11ShaderResourceView_Release(s_keep_srv);
    if (s_keep_tex) ID3D11Texture2D_Release(s_keep_tex);
    s_keep_srv = NULL;
    s_keep_tex = NULL;
    s_keep_w = s_keep_h = 0;
    s_keep_id = 0;
}

/* Copies the held shared frame into s_keep_tex (made to match). 0 if it cannot. */
static int keep_shared_copy(ID3D11Texture2D *src, uint64_t frame_id)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(src, &td);
    if (!s_keep_tex || s_keep_w != td.Width || s_keep_h != td.Height) {
        HRESULT hr;
        release_keep();
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_keep_tex);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_keep_tex,
                                                       NULL, &s_keep_srv);
        if (FAILED(hr)) {
            release_keep();
            return 0;
        }
        s_keep_w = td.Width;
        s_keep_h = td.Height;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_keep_tex, (ID3D11Resource *)src);
    s_keep_id = frame_id;
    return 1;
}

#ifdef RECOMP_PRESENT_TESTING
static const char *s_present_test_stage;
static HRESULT s_present_test_result, s_present_test_reason;
static HRESULT present_test_result(const char *stage, HRESULT result)
{
    if (s_present_test_stage && strcmp(s_present_test_stage, stage) == 0) {
        s_present_test_stage = NULL;
        return s_present_test_result;
    }
    return result;
}
#else
static HRESULT present_test_result(const char *stage, HRESULT result)
{
    (void)stage;
    return result;
}
#endif

static void present_check_device(HRESULT result, const char *operation)
{
    HRESULT reason = s_dev ? ID3D11Device_GetDeviceRemovedReason(s_dev) : S_OK;
#ifdef RECOMP_PRESENT_TESTING
    if (FAILED(s_present_test_reason))
        reason = s_present_test_reason;
#endif
    if (FAILED(reason) || result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG ||
        result == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        fprintf(stderr, "  [D3D] presenter device lost during %s: result 0x%08lX,"
                        " removed reason 0x%08lX; recovery is not implemented,"
                        " stopping rather than completing a shared frame\n",
                operation, (unsigned long)result, (unsigned long)reason);
        fflush(stderr);
        abort();
    }
}

static void present_fatal(HRESULT result, const char *operation)
{
    present_check_device(result, operation);
    fprintf(stderr, "  [D3D] presenter %s failed 0x%08lX;"
                    " resource recovery is not implemented, stopping\n",
            operation, (unsigned long)result);
    fflush(stderr);
    abort();
}

/* A triangle covering the viewport, from the vertex id; uv 0..1. */
static const char s_hlsl[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
    "  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";

int gpu_present_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_D3D_PRESENT");
        on = e && *e && *e != '0';
    }
    return on;
}

static void client_size(uint32_t *w, uint32_t *h)
{
    RECT r;
    GetClientRect(s_hwnd, &r);
    *w = r.right > r.left ? (uint32_t)(r.right - r.left) : 1;
    *h = r.bottom > r.top ? (uint32_t)(r.bottom - r.top) : 1;
}

static int make_rtv(void)
{
    ID3D11Texture2D *back = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(s_chain, 0, &IID_ID3D11Texture2D, (void **)&back);
    hr = present_test_result("back-buffer", hr);
    present_check_device(hr, "getting back buffer");
    if (FAILED(hr)) {
        if (back) ID3D11Texture2D_Release(back);
        return 0;
    }
    hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back, NULL, &s_rtv);
    hr = present_test_result("render-target", hr);
    present_check_device(hr, "creating back-buffer view");
    ID3D11Texture2D_Release(back);
    return SUCCEEDED(hr);
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "present", NULL, NULL, entry, target,
                            0, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "  [D3D] present shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "failed");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

int gpu_present_init(HWND hwnd, uint32_t width, uint32_t height)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SAMPLER_DESC smp;
    ID3DBlob *vsb, *psb;
    HRESULT hr;
    const char *v = getenv("RECOMP_PRESENT_VSYNC"), *f = getenv("RECOMP_PRESENT_FILTER");

    if (v && v[0] == '0')
        s_vsync = 0;
    s_hwnd = hwnd;
    s_w = width; s_h = height;
    client_size(&s_bw, &s_bh);
    ZeroMemory(&sd, sizeof sd);
    sd.BufferDesc.Width  = s_bw;
    sd.BufferDesc.Height = s_bh;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count  = 1;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount       = 1;
    sd.OutputWindow      = hwnd;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &sd,
                                       &s_chain, &s_dev, NULL, &s_ctx);
    present_check_device(hr, "creating device and swap chain");
    if (FAILED(hr)) {
        fprintf(stderr, "  [D3D] device/swap chain failed (0x%08lX); staying on GDI\n",
                (unsigned long)hr);
        return 0;
    }
    {
        /* Alt+Enter is the window's (borderless fullscreen, fb_present.c),
         * not DXGI's exclusive mode at 640x480. */
        IDXGIFactory *fac = NULL;
        hr = IDXGISwapChain_GetParent(s_chain, &IID_IDXGIFactory, (void **)&fac);
        present_check_device(hr, "querying swap-chain factory");
        if (SUCCEEDED(hr)) {
            hr = IDXGIFactory_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
            present_check_device(hr, "associating presenter window");
            IDXGIFactory_Release(fac);
        }
    }
    ZeroMemory(&td, sizeof td);
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ZeroMemory(&smp, sizeof smp);
    smp.Filter = (f && (f[0] == 'p' || f[0] == 'P')) ? D3D11_FILTER_MIN_MAG_MIP_POINT
                                                     : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    vsb = compile("vs", "vs_4_0");
    psb = compile("ps", "ps_4_0");
    if (!vsb || !psb || !make_rtv())
        goto failed;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_frame);
    hr = present_test_result("init-frame", hr);
    present_check_device(hr, "creating CPU frame texture");
    if (FAILED(hr)) goto failed;
    hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_frame, NULL, &s_srv);
    hr = present_test_result("init-view", hr);
    present_check_device(hr, "creating CPU frame view");
    if (FAILED(hr)) goto failed;
    hr = ID3D11Device_CreateSamplerState(s_dev, &smp, &s_samp);
    hr = present_test_result("init-sampler", hr);
    present_check_device(hr, "creating presenter sampler");
    if (FAILED(hr)) goto failed;
    hr = ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    hr = present_test_result("init-vs", hr);
    present_check_device(hr, "creating presenter vertex shader");
    if (FAILED(hr)) goto failed;
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                        ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    hr = present_test_result("init-ps", hr);
    present_check_device(hr, "creating presenter pixel shader");
    if (FAILED(hr)) goto failed;
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    fprintf(stderr, "  [D3D] presenting through D3D11 (%ux%u frame, vsync %s, %s filter)\n",
            width, height, s_vsync ? "on" : "off",
            smp.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT ? "point" : "linear");
    InterlockedExchange(&s_shared_ready, 1);
    return 1;

failed:
    fprintf(stderr, "  [D3D] present resources failed; staying on GDI\n");
    if (vsb) ID3D10Blob_Release(vsb);
    if (psb) ID3D10Blob_Release(psb);
    gpu_present_shutdown();
    return 0;
}

int gpu_present_shared_ready(uint32_t width, uint32_t height)
{
    return InterlockedCompareExchange(&s_shared_ready, 0, 0) &&
           width && height && width <= 16384 && height <= 16384 && !getenv("RECOMP_FB_VA");
}

/* The picture's shape: 4:3 unless a title renders wider (widescreen) and
 * says so with gpu_present_set_picture_aspect. */
static volatile float s_picture_aspect = 4.0f / 3.0f;

static float s_vzoom = 1.0f;

void gpu_present_set_vzoom(float z)
{
    s_vzoom = z >= 1.0f && z <= 2.0f ? z : 1.0f;
}

void gpu_present_set_picture_aspect(float aspect)
{
    if (aspect >= 1.0f && aspect <= 4.0f)
        s_picture_aspect = aspect;
}

float gpu_present_picture_aspect(void) { return s_picture_aspect; }

static int picture_size(uint32_t client_width, uint32_t client_height,
                        uint32_t *width, uint32_t *height)
{
    float aspect = s_picture_aspect;
    if (!client_width || !client_height || !width || !height)
        return 0;
    if ((double)client_width <= (double)client_height * aspect) {
        *width = client_width;
        *height = (uint32_t)((double)client_width / aspect + 0.5);
    } else {
        *height = client_height;
        *width = (uint32_t)((double)client_height * aspect + 0.5);
    }
    return *width && *height;
}

int gpu_present_output_size(uint32_t *width, uint32_t *height)
{
    RECT r;
    uint32_t client_width, client_height;

    if (!width || !height || !InterlockedCompareExchange(&s_shared_ready, 0, 0) ||
        !s_hwnd || !GetClientRect(s_hwnd, &r) || r.right <= r.left || r.bottom <= r.top)
        return 0;
    client_width = (uint32_t)(r.right - r.left);
    client_height = (uint32_t)(r.bottom - r.top);
    return picture_size(client_width, client_height, width, height);
}

int gpu_present_submit_shared(HANDLE handle, uint32_t width, uint32_t height,
                              uint64_t frame_id)
{
    int submitted = 0;
    if (!handle || !frame_id || !gpu_present_shared_ready(width, height))
        return 0;
    AcquireSRWLockExclusive(&s_shared_lock);
    if (InterlockedCompareExchange(&s_shared_ready, 0, 0) && !s_pending_shared) {
        s_pending_shared = handle;
        s_pending_frame_id = frame_id;
        s_pending_width = width;
        s_pending_height = height;
        submitted = 1;
    }
    ReleaseSRWLockExclusive(&s_shared_lock);
    return submitted;
}

static void release_shared_resources(void)
{
    if (s_shared_srv) ID3D11ShaderResourceView_Release(s_shared_srv);
    if (s_shared_mutex) IDXGIKeyedMutex_Release(s_shared_mutex);
    if (s_shared_texture) ID3D11Texture2D_Release(s_shared_texture);
    s_shared_srv = NULL;
    s_shared_mutex = NULL;
    s_shared_texture = NULL;
    s_shared_handle = NULL;
    s_shared_width = s_shared_height = 0;
}

static ID3D11ShaderResourceView *take_shared(int allow_shared, uint64_t frame_id, int *held,
                                             uint32_t *width, uint32_t *height)
{
    HANDLE handle;
    uint64_t pending_id;
    uint32_t pending_width, pending_height;
    HRESULT hr;
    int dimensions_match = 1;

    *held = 0;
    AcquireSRWLockExclusive(&s_shared_lock);
    handle = s_pending_shared;
    pending_id = s_pending_frame_id;
    pending_width = s_pending_width;
    pending_height = s_pending_height;
    if (handle && allow_shared && frame_id < pending_id) {
        ReleaseSRWLockExclusive(&s_shared_lock);
        return NULL;
    }
    s_pending_shared = NULL;
    s_pending_width = s_pending_height = 0;
    ReleaseSRWLockExclusive(&s_shared_lock);
    if (!handle)
        return NULL;
    if (handle != s_shared_handle) {
        release_shared_resources();
        hr = ID3D11Device_OpenSharedResource(s_dev, handle, &IID_ID3D11Texture2D,
                                            (void **)&s_shared_texture);
        hr = present_test_result("shared-open", hr);
        present_check_device(hr, "opening shared frame");
        if (SUCCEEDED(hr))
            hr = ID3D11Texture2D_QueryInterface(s_shared_texture, &IID_IDXGIKeyedMutex,
                                                (void **)&s_shared_mutex);
        present_check_device(hr, "querying shared-frame mutex");
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_shared_texture,
                                                       NULL, &s_shared_srv);
        present_check_device(hr, "creating shared-frame view");
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc(s_shared_texture, &td);
            if (td.Width != pending_width || td.Height != pending_height)
                hr = E_INVALIDARG;
            else {
                s_shared_width = td.Width;
                s_shared_height = td.Height;
            }
        }
        if (FAILED(hr)) {
            fprintf(stderr, "  [D3D] shared frame open failed 0x%08lX; using CPU frames\n",
                    (unsigned long)hr);
            release_shared_resources();
            InterlockedExchange(&s_shared_ready, 0);
            return NULL;
        }
        s_shared_handle = handle;
    } else if (pending_width != s_shared_width || pending_height != s_shared_height)
        dimensions_match = 0;
    hr = IDXGIKeyedMutex_AcquireSync(s_shared_mutex, 1, 0);
    hr = present_test_result("shared-acquire", hr);
    present_check_device(hr, "acquiring shared frame");
    if (hr == WAIT_TIMEOUT) {
        AcquireSRWLockExclusive(&s_shared_lock);
        if (!s_pending_shared && InterlockedCompareExchange(&s_shared_ready, 0, 0)) {
            s_pending_shared = handle;
            s_pending_frame_id = pending_id;
            s_pending_width = pending_width;
            s_pending_height = pending_height;
        }
        ReleaseSRWLockExclusive(&s_shared_lock);
        return NULL;
    }
    if (hr != S_OK) {
        present_fatal(hr, "shared frame acquire");
    }
    if (!dimensions_match) {
        fprintf(stderr, "  [D3D] shared frame dimensions changed without a new resource;"
                        " using CPU frames\n");
        hr = IDXGIKeyedMutex_ReleaseSync(s_shared_mutex, 0);
        hr = present_test_result("shared-release", hr);
        present_check_device(hr, "releasing mismatched shared frame");
        if (hr != S_OK)
            present_fatal(hr, "shared frame release");
        InterlockedExchange(&s_shared_ready, 0);
        return NULL;
    }
    if (!allow_shared || frame_id != pending_id) {
        hr = IDXGIKeyedMutex_ReleaseSync(s_shared_mutex, 0);
        hr = present_test_result("shared-release", hr);
        present_check_device(hr, "releasing discarded shared frame");
        if (hr != S_OK)
            present_fatal(hr, "shared frame release");
        return NULL;
    }
    *held = 1;
    *width = s_shared_width;
    *height = s_shared_height;
    return s_shared_srv;
}

/* Returns 0 when nothing was shown: the window is occluded or the display
 * is off, and Present does not wait for vsync then -- the caller must pace
 * itself instead of spinning. The pixels are read before this returns. */
static int gpu_present_frame_impl(const uint32_t *pixels, int allow_shared,
                                  uint64_t frame_id, GpuPresentTrace *trace)
{
    static const float black[4] = {0, 0, 0, 1};
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *frame, *none = NULL;
    HRESULT present_hr, hr;
    uint32_t cw, ch, frame_width = s_w, frame_height = s_h;
    float sx, sy;
    int held;

    if (trace) {
        memset(trace, 0, sizeof *trace);
        trace->stage = GPU_PRESENT_NO_SURFACE;
    }
    present_check_device(S_OK, "starting frame");
    if (!s_rtv || !pixels)
        return 0;
    client_size(&cw, &ch);
    if (cw != s_bw || ch != s_bh) {
        /* The window was resized (or went fullscreen): a back buffer its size. */
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(s_rtv);
        s_rtv = NULL;
        hr = IDXGISwapChain_ResizeBuffers(s_chain, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        hr = present_test_result("resize", hr);
        if (FAILED(hr)) {
            if (trace) {
                trace->stage = GPU_PRESENT_RESIZE_FAILED;
                trace->result = hr;
            }
            present_fatal(hr, "swap-chain resize");
        }
        present_check_device(hr, "resizing swap chain");
        if (!make_rtv()) {
            if (trace)
                trace->stage = GPU_PRESENT_TARGET_FAILED;
            present_fatal(E_FAIL, "back-buffer recreation");
        }
        s_bw = cw; s_bh = ch;
    }
    frame = take_shared(allow_shared, frame_id, &held, &frame_width, &frame_height);
    if (frame && trace) {
        trace->route = GPU_PRESENT_SHARED_REJECTED;
        trace->stage = GPU_PRESENT_INCOMPATIBLE;
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        ID3D11DeviceContext_Flush(s_ctx);
        trace->shared_release = IDXGIKeyedMutex_ReleaseSync(s_shared_mutex, 0);
        trace->shared_release = present_test_result("shared-release", trace->shared_release);
        present_check_device(trace->shared_release, "releasing captured shared frame");
        if (trace->shared_release != S_OK)
            present_fatal(trace->shared_release, "shared frame release");
        return 0;
    }
    if (frame && held && keep_shared_copy(s_shared_texture, frame_id)) {
        ID3D11DeviceContext_Flush(s_ctx);
        hr = IDXGIKeyedMutex_ReleaseSync(s_shared_mutex, 0);
        hr = present_test_result("shared-release", hr);
        present_check_device(hr, "releasing copied shared frame");
        if (hr != S_OK)
            present_fatal(hr, "shared frame release");
        held = 0;
        frame = s_keep_srv;
    } else if (!frame && allow_shared && s_keep_srv && frame_id && frame_id == s_keep_id) {
        frame = s_keep_srv;                       /* a repeat: the last GPU frame again */
        frame_width = s_keep_w;
        frame_height = s_keep_h;
    }
    if (!frame) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_frame, 0, NULL,
                                              pixels, s_w * 4, 0);
        frame = s_srv;
        if (trace)
            trace->route = GPU_PRESENT_CPU_UPLOAD;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_rtv, black);
    /* The largest rectangle of the picture's shape that fits, centred: 4:3,
     * or the widescreen shape (gpu_present_set_picture_aspect) whatever the
     * frame's own pixel size (a widescreen frame from the CPU path is still
     * 640x480, squeezed across, as the Xbox sent it to a widescreen TV). */
    {
        uint32_t pw, ph;
        (void)sx; (void)sy;
        if (!picture_size(s_bw, s_bh, &pw, &ph)) { pw = s_bw; ph = s_bh; }
        vp.Width = (float)pw;
        vp.Height = (float)ph * s_vzoom;   /* a cutscene's bars zoomed out of the window */
    }
    vp.TopLeftX = ((float)s_bw - vp.Width) * 0.5f;
    vp.TopLeftY = ((float)s_bh - vp.Height) * 0.5f;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &frame);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    /* Hand the shared frame back as soon as its draw is queued, not after
     * Present: Present waits for the vblank (up to 16 ms), and a frame the
     * title finished in that time found the texture still held and went out
     * as the 640x480 CPU copy instead -- every other frame blurry at 3x and
     * above (about 40 fps on a 60 Hz screen, 2026-11-25). The keyed mutex
     * orders the GPU work, so the draw still reads this frame. */
    if (held) {
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        ID3D11DeviceContext_Flush(s_ctx);
        hr = IDXGIKeyedMutex_ReleaseSync(s_shared_mutex, 0);
        hr = present_test_result("shared-release", hr);
        present_check_device(hr, "releasing presented shared frame");
        if (hr != S_OK)
            present_fatal(hr, "shared frame release");
    }
    /* Paced only when Present waited for the vblank: with RECOMP_PRESENT_VSYNC=0
     * the caller keeps its own 16 ms sleep instead of spinning a core. */
    present_hr = IDXGISwapChain_Present(s_chain, (UINT)s_vsync, 0);
    InterlockedIncrement(&g_gpu_present_count);
    present_hr = present_test_result("present", present_hr);
    present_check_device(present_hr, "presenting frame");
    if (FAILED(present_hr))
        present_fatal(present_hr, "Present");
    if (trace) {
        trace->stage = GPU_PRESENT_CALLED;
        trace->result = present_hr;
    }
    if (held) {
        static int logged;
        if (present_hr == S_OK && !logged++) {
            fprintf(stderr, "  [D3D] shared GPU frame presented\n");
        }
    }
    return present_hr == S_OK && s_vsync;
}

int gpu_present_frame(const uint32_t *pixels, int allow_shared, uint64_t frame_id)
{
    return gpu_present_frame_impl(pixels, allow_shared, frame_id, NULL);
}

int gpu_present_frame_capture(const uint32_t *pixels, int allow_shared,
                              uint64_t frame_id, GpuPresentTrace *trace)
{
    if (!trace)
        return 0;
    return gpu_present_frame_impl(pixels, allow_shared, frame_id, trace);
}

void gpu_present_shutdown(void)
{
    present_check_device(S_OK, "shutting down presenter");
    InterlockedExchange(&s_shared_ready, 0);
    AcquireSRWLockExclusive(&s_shared_lock);
    s_pending_shared = NULL;
    s_pending_width = s_pending_height = 0;
    ReleaseSRWLockExclusive(&s_shared_lock);
    release_shared_resources();
    release_keep();
    if (s_ps)    { ID3D11PixelShader_Release(s_ps);           s_ps = NULL; }
    if (s_vs)    { ID3D11VertexShader_Release(s_vs);          s_vs = NULL; }
    if (s_samp)  { ID3D11SamplerState_Release(s_samp);        s_samp = NULL; }
    if (s_srv)   { ID3D11ShaderResourceView_Release(s_srv);   s_srv = NULL; }
    if (s_frame) { ID3D11Texture2D_Release(s_frame);          s_frame = NULL; }
    if (s_rtv)   { ID3D11RenderTargetView_Release(s_rtv);     s_rtv = NULL; }
    if (s_chain) { IDXGISwapChain_Release(s_chain);           s_chain = NULL; }
    if (s_ctx)   { ID3D11DeviceContext_Release(s_ctx);        s_ctx = NULL; }
    if (s_dev)   { ID3D11Device_Release(s_dev);               s_dev = NULL; }
    s_hwnd = NULL;
}

int gpu_present_active(void)
{
    return s_rtv != NULL;
}
#endif

void gpu_present_set_vsync(int enabled)
{
    s_vsync = enabled ? 1 : 0;
}

int gpu_present_get_vsync(void)
{
    return s_vsync;
}
