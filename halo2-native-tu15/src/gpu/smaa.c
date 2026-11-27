/* SMAA 1x at present time (smaa.h). Three full-screen passes over the presented picture:
 * luma edge detection into a two-channel edge texture, blending-weight calculation with the
 * precomputed area and search textures, and neighbourhood blending back into the target.
 * The shader and the two lookup textures are the SMAA reference implementation (src/gpu/smaa,
 * MIT, Jimenez et al.), embedded in the exe; nothing is read from disk.
 * RECOMP_GPU_SMAA_PRESET=low|medium|high|ultra picks the quality preset (default high). */
#include "smaa.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"
#include "smaa/smaa_hlsl.h"

static int s_enabled;
static int s_failed;
static ID3D11Device *s_dev;
static ID3D11VertexShader *s_vs[3];
static ID3D11PixelShader *s_ps[3];
static ID3D11SamplerState *s_lin, *s_pt;
static ID3D11Buffer *s_cb;
static ID3D11RasterizerState *s_rs;
static ID3D11BlendState *s_bs;
static ID3D11DepthStencilState *s_ds;
static ID3D11Texture2D *s_area, *s_search;
static ID3D11ShaderResourceView *s_area_srv, *s_search_srv;
/* per size: the source copy, the edge and the weight targets */
static ID3D11Texture2D *s_src, *s_edges, *s_blend;
static ID3D11ShaderResourceView *s_src_srv, *s_edges_srv, *s_blend_srv;
static ID3D11RenderTargetView *s_edges_rtv, *s_blend_rtv;
static UINT s_w, s_h;
static char *s_src_text;             /* the shader source with the preset, built once */
static size_t s_src_len;

#define REL(p, T) do { if (p) { T##_Release(p); (p) = NULL; } } while (0)

void halo2_smaa_set_enabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

int halo2_smaa_enabled(void)
{
    return s_enabled;
}

static void release_sized(void)
{
    REL(s_src_srv, ID3D11ShaderResourceView);
    REL(s_edges_srv, ID3D11ShaderResourceView);
    REL(s_blend_srv, ID3D11ShaderResourceView);
    REL(s_edges_rtv, ID3D11RenderTargetView);
    REL(s_blend_rtv, ID3D11RenderTargetView);
    REL(s_src, ID3D11Texture2D);
    REL(s_edges, ID3D11Texture2D);
    REL(s_blend, ID3D11Texture2D);
    s_w = s_h = 0;
}

void halo2_smaa_shutdown(void)
{
    int i;
    release_sized();
    for (i = 0; i < 3; i++) {
        REL(s_vs[i], ID3D11VertexShader);
        REL(s_ps[i], ID3D11PixelShader);
    }
    REL(s_lin, ID3D11SamplerState);
    REL(s_pt, ID3D11SamplerState);
    REL(s_cb, ID3D11Buffer);
    REL(s_rs, ID3D11RasterizerState);
    REL(s_bs, ID3D11BlendState);
    REL(s_ds, ID3D11DepthStencilState);
    REL(s_area_srv, ID3D11ShaderResourceView);
    REL(s_search_srv, ID3D11ShaderResourceView);
    REL(s_area, ID3D11Texture2D);
    REL(s_search, ID3D11Texture2D);
    s_dev = NULL;
    s_failed = 0;
    free(s_src_text);                   /* the preset may change before the next build */
    s_src_text = NULL;
}

/* Our side of SMAA's "custom shading language" hooks: fixed sampler slots, so binding never
 * depends on which samplers a pass happens to use. */
static const char s_head[] =
    "#define SMAA_CUSTOM_SL 1\n"
    "#define SMAA_RT_METRICS g_rt\n"
    "cbuffer SmaaCB : register(b0) { float4 g_rt; };\n"
    "SamplerState LinearSampler : register(s0);\n"
    "SamplerState PointSampler : register(s1);\n"
    "#define SMAATexture2D(tex) Texture2D tex\n"
    "#define SMAATexturePass2D(tex) tex\n"
    "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
    "#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)\n"
    "#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)\n"
    "#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)\n"
    "#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)\n"
    "#define SMAA_FLATTEN [flatten]\n"
    "#define SMAA_BRANCH [branch]\n";

static const char s_tail[] =
    "\nTexture2D colorTex : register(t0);\n"
    "Texture2D edgesTex : register(t1);\n"
    "Texture2D areaTex : register(t2);\n"
    "Texture2D searchTex : register(t3);\n"
    "Texture2D blendTex : register(t4);\n"
    "float4 fs_pos(uint id, out float2 uv) {\n"
    "  uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "struct VE { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; };\n"
    "VE edge_vs(uint id : SV_VertexID) {\n"
    "  VE o; float4 off[3]; o.pos = fs_pos(id, o.uv);\n"
    "  SMAAEdgeDetectionVS(o.uv, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o;\n"
    "}\n"
    "float2 edge_ps(VE i) : SV_TARGET {\n"
    "  float4 off[3] = { i.o0, i.o1, i.o2 };\n"
    "  return SMAALumaEdgeDetectionPS(i.uv, off, colorTex);\n"
    "}\n"
    "struct VW { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };\n"
    "VW weight_vs(uint id : SV_VertexID) {\n"
    "  VW o; float4 off[3]; o.pos = fs_pos(id, o.uv);\n"
    "  SMAABlendingWeightCalculationVS(o.uv, o.pix, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o;\n"
    "}\n"
    "float4 weight_ps(VW i) : SV_TARGET {\n"
    "  float4 off[3] = { i.o0, i.o1, i.o2 };\n"
    "  return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));\n"
    "}\n"
    "struct VN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };\n"
    "VN blend_vs(uint id : SV_VertexID) {\n"
    "  VN o; o.pos = fs_pos(id, o.uv); SMAANeighborhoodBlendingVS(o.uv, o.o); return o;\n"
    "}\n"
    "float4 blend_ps(VN i) : SV_TARGET {\n"
    "  return float4(SMAANeighborhoodBlendingPS(i.uv, i.o, colorTex, blendTex).rgb, 1);\n"
    "}\n";

static const char *preset_define(void)
{
    const char *p = getenv("RECOMP_GPU_SMAA_PRESET");
    if (p && !_stricmp(p, "low")) return "#define SMAA_PRESET_LOW 1\n";
    if (p && !_stricmp(p, "medium")) return "#define SMAA_PRESET_MEDIUM 1\n";
    if (p && !_stricmp(p, "ultra")) return "#define SMAA_PRESET_ULTRA 1\n";
    return "#define SMAA_PRESET_HIGH 1\n";
}

static int build_source(void)
{
    const char *preset = preset_define();
    size_t a = strlen(preset), b = sizeof s_head - 1, c = sizeof smaa_hlsl_src, d = sizeof s_tail - 1;
    if (s_src_text)
        return 1;
    s_src_text = (char *)malloc(a + b + c + d + 1);
    if (!s_src_text)
        return 0;
    memcpy(s_src_text, preset, a);
    memcpy(s_src_text + a, s_head, b);
    memcpy(s_src_text + a + b, smaa_hlsl_src, c);
    memcpy(s_src_text + a + b + c, s_tail, d);
    s_src_len = a + b + c + d;
    s_src_text[s_src_len] = 0;
    return 1;
}

static int compile(const char *entry, const char *target, ID3D10Blob **blob)
{
    ID3D10Blob *err = NULL;
    HRESULT hr = D3DCompile(s_src_text, s_src_len, "smaa", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
    if (FAILED(hr)) {
        const char *msg = err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "";
        fprintf(stderr, "[SMAA] compile %s failed 0x%08lX: %.600s\n", entry, (unsigned long)hr, msg);
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    return 1;
}

static int make_lookup(ID3D11Device *device, UINT w, UINT h, DXGI_FORMAT fmt, const void *bytes, UINT pitch,
                       ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = bytes;
    sd.SysMemPitch = pitch;
    sd.SysMemSlicePitch = 0;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, &sd, tex)) &&
           SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)*tex, NULL, srv));
}

static int ensure_init(ID3D11Device *device)
{
    static const char *const vs_names[3] = { "edge_vs", "weight_vs", "blend_vs" };
    static const char *const ps_names[3] = { "edge_ps", "weight_ps", "blend_ps" };
    D3D11_SAMPLER_DESC sd;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC blend;
    D3D11_DEPTH_STENCIL_DESC ds;
    int i;

    if (s_failed)
        return 0;
    if (s_dev == device && s_ps[2])
        return 1;
    halo2_smaa_shutdown();
    s_dev = device;
    if (!build_source())
        goto failed;
    for (i = 0; i < 3; i++) {
        ID3D10Blob *vsb = NULL, *psb = NULL;
        int ok = compile(vs_names[i], "vs_5_0", &vsb) && compile(ps_names[i], "ps_5_0", &psb) &&
                 SUCCEEDED(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
                                                           ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs[i])) &&
                 SUCCEEDED(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb),
                                                          ID3D10Blob_GetBufferSize(psb), NULL, &s_ps[i]));
        if (vsb) ID3D10Blob_Release(vsb);
        if (psb) ID3D10Blob_Release(psb);
        if (!ok)
            goto failed;
    }
    if (!make_lookup(device, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, areaTexBytes, AREATEX_PITCH,
                     &s_area, &s_area_srv) ||
        !make_lookup(device, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, searchTexBytes,
                     SEARCHTEX_PITCH, &s_search, &s_search_srv))
        goto failed;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(device, &sd, &s_lin)))
        goto failed;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(ID3D11Device_CreateSamplerState(device, &sd, &s_pt)))
        goto failed;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(device, &bd, NULL, &s_cb)))
        goto failed;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(device, &rd, &s_rs)))
        goto failed;
    memset(&blend, 0, sizeof blend);
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(device, &blend, &s_bs)))
        goto failed;
    memset(&ds, 0, sizeof ds);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(device, &ds, &s_ds)))
        goto failed;
    {
        const char *p = getenv("RECOMP_GPU_SMAA_PRESET");
        fprintf(stderr, "[SMAA] post-process ready (preset %s)\n", p && *p ? p : "high");
    }
    return 1;
failed:
    halo2_smaa_shutdown();
    s_failed = 1;
    fprintf(stderr, "[SMAA] initialization failed; SMAA stays off\n");
    return 0;
}

static int make_target(ID3D11Device *device, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind,
                       ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv, ID3D11RenderTargetView **rtv)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &td, NULL, tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)*tex, NULL, srv)))
        return 0;
    return !rtv || SUCCEEDED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)*tex, NULL, rtv));
}

static int ensure_sized(ID3D11Device *device, UINT w, UINT h)
{
    if (s_src && s_w == w && s_h == h)
        return 1;
    release_sized();
    if (!make_target(device, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &s_src, &s_src_srv, NULL) ||
        !make_target(device, w, h, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                     &s_edges, &s_edges_srv, &s_edges_rtv) ||
        !make_target(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                     &s_blend, &s_blend_srv, &s_blend_rtv)) {
        release_sized();
        return 0;
    }
    s_w = w;
    s_h = h;
    return 1;
}

static void pass(ID3D11DeviceContext *ctx, int i, ID3D11RenderTargetView *rtv, int clear)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    if (clear)
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, zero);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, s_vs[i], NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, s_ps[i], NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}

int halo2_smaa_apply(ID3D11Device *device, ID3D11DeviceContext *ctx,
                     ID3D11Texture2D *target, ID3D11RenderTargetView *target_rtv,
                     UINT width, UINT height)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp, old_vp;
    UINT nvp = 1;
    ID3D11RenderTargetView *old_rtv = NULL;
    ID3D11DepthStencilView *old_dsv = NULL;
    ID3D11ShaderResourceView *srv[5];
    ID3D11ShaderResourceView *none[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11SamplerState *samp[2];
    float rt[4];

    if (!s_enabled || !device || !ctx || !target || !target_rtv || !width || !height)
        return 1;
    if (!ensure_init(device) || !ensure_sized(device, width, height))
        return 0;

    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s_src, (ID3D11Resource *)target);
    rt[0] = 1.0f / (float)width;
    rt[1] = 1.0f / (float)height;
    rt[2] = (float)width;
    rt[3] = (float)height;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return 0;
    memcpy(mapped.pData, rt, sizeof rt);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);

    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &old_rtv, &old_dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &nvp, &old_vp);
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    samp[0] = s_lin;
    samp[1] = s_pt;
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, samp);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);
    ID3D11DeviceContext_OMSetBlendState(ctx, s_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, s_ds, 0);

    /* 1: edges (the picture) */
    srv[0] = s_src_srv; srv[1] = NULL; srv[2] = NULL; srv[3] = NULL; srv[4] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, none);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, srv);
    pass(ctx, 0, s_edges_rtv, 1);
    /* 2: blending weights (edges, area, search) */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    srv[0] = NULL; srv[1] = s_edges_srv; srv[2] = s_area_srv; srv[3] = s_search_srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, srv);
    pass(ctx, 1, s_blend_rtv, 1);
    /* 3: neighbourhood blending into the target (picture, weights) */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    srv[0] = s_src_srv; srv[1] = NULL; srv[2] = NULL; srv[3] = NULL; srv[4] = s_blend_srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, srv);
    pass(ctx, 2, target_rtv, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, none);

    ID3D11DeviceContext_OMSetRenderTargets(ctx, old_rtv ? 1 : 0, old_rtv ? &old_rtv : NULL, old_dsv);
    if (nvp)
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &old_vp);
    if (old_rtv) ID3D11RenderTargetView_Release(old_rtv);
    if (old_dsv) ID3D11DepthStencilView_Release(old_dsv);
    return 1;
}
