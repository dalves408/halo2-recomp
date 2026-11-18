#include "fxaa.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

static int s_enabled;
static int s_failed;
static ID3D11Device *s_dev;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11SamplerState *s_samp;
static ID3D11Buffer *s_cb;
static ID3D11Texture2D *s_src;
static ID3D11ShaderResourceView *s_src_srv;
static ID3D11RasterizerState *s_rs;
static ID3D11BlendState *s_bs;
static ID3D11DepthStencilState *s_ds;
static UINT s_w, s_h;

void halo2_fxaa_set_enabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

int halo2_fxaa_enabled(void)
{
    return s_enabled;
}

void halo2_fxaa_shutdown(void)
{
    if (s_src_srv) ID3D11ShaderResourceView_Release(s_src_srv);
    if (s_src) ID3D11Texture2D_Release(s_src);
    if (s_cb) ID3D11Buffer_Release(s_cb);
    if (s_samp) ID3D11SamplerState_Release(s_samp);
    if (s_ps) ID3D11PixelShader_Release(s_ps);
    if (s_vs) ID3D11VertexShader_Release(s_vs);
    if (s_rs) ID3D11RasterizerState_Release(s_rs);
    if (s_bs) ID3D11BlendState_Release(s_bs);
    if (s_ds) ID3D11DepthStencilState_Release(s_ds);
    s_src_srv = NULL;
    s_src = NULL;
    s_cb = NULL;
    s_samp = NULL;
    s_ps = NULL;
    s_vs = NULL;
    s_rs = NULL;
    s_bs = NULL;
    s_ds = NULL;
    s_dev = NULL;
    s_w = s_h = 0;
    s_failed = 0;
}

static const char s_hlsl[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer CB : register(b0) { float2 rcp; float2 pad; };\n"
    "float4 vs_main(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_POSITION {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  uv = p;\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float4 ps_main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "  float3 rgbM = tex.SampleLevel(samp, uv, 0).rgb;\n"
    "  float3 rgbN = tex.SampleLevel(samp, uv + float2(0, -rcp.y), 0).rgb;\n"
    "  float3 rgbS = tex.SampleLevel(samp, uv + float2(0,  rcp.y), 0).rgb;\n"
    "  float3 rgbW = tex.SampleLevel(samp, uv + float2(-rcp.x, 0), 0).rgb;\n"
    "  float3 rgbE = tex.SampleLevel(samp, uv + float2( rcp.x, 0), 0).rgb;\n"
    "  float3 rgbNW = tex.SampleLevel(samp, uv + float2(-rcp.x, -rcp.y), 0).rgb;\n"
    "  float3 rgbNE = tex.SampleLevel(samp, uv + float2( rcp.x, -rcp.y), 0).rgb;\n"
    "  float3 rgbSW = tex.SampleLevel(samp, uv + float2(-rcp.x,  rcp.y), 0).rgb;\n"
    "  float3 rgbSE = tex.SampleLevel(samp, uv + float2( rcp.x,  rcp.y), 0).rgb;\n"
    "  float lM = luma(rgbM), lN = luma(rgbN), lS = luma(rgbS), lW = luma(rgbW), lE = luma(rgbE);\n"
    "  float lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE);\n"
    "  float lMin = min(lM, min(min(lN, lS), min(lW, lE)));\n"
    "  float lMax = max(lM, max(max(lN, lS), max(lW, lE)));\n"
    "  float contrast = lMax - lMin;\n"
    "  if (contrast < max(0.0312, lMax * 0.125)) return float4(rgbM, 1);\n"
    "  float horz = abs(lNW + lNE - 2 * lN) + 2 * abs(lW + lE - 2 * lM) + abs(lSW + lSE - 2 * lS);\n"
    "  float vert = abs(lNW + lSW - 2 * lW) + 2 * abs(lN + lS - 2 * lM) + abs(lNE + lSE - 2 * lE);\n"
    "  int isHorz = horz >= vert;\n"
    "  float2 off = isHorz ? float2(0, rcp.y) : float2(rcp.x, 0);\n"
    "  float lNeg = isHorz ? lN : lW;\n"
    "  float lPos = isHorz ? lS : lE;\n"
    "  float gradNeg = abs(lNeg - lM);\n"
    "  float gradPos = abs(lPos - lM);\n"
    "  float2 dir = gradNeg >= gradPos ? -off : off;\n"
    "  float3 rgb1 = tex.SampleLevel(samp, uv + dir * 0.5, 0).rgb;\n"
    "  float3 rgb2 = tex.SampleLevel(samp, uv + dir * 1.5, 0).rgb;\n"
    "  float3 rgb3 = tex.SampleLevel(samp, uv - dir * 0.5, 0).rgb;\n"
    "  float3 blended = (rgb1 + rgb3) * 0.5;\n"
    "  float3 far = (blended + rgb2) * 0.5;\n"
    "  float lB = luma(blended);\n"
    "  if (lB < lMin || lB > lMax) return float4(blended, 1);\n"
    "  return float4(far, 1);\n"
    "}\n";

static int compile(const char *entry, const char *target, ID3D10Blob **blob)
{
    ID3D10Blob *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "fxaa", NULL, NULL,
                            entry, target, 0, 0, blob, &err);
    if (FAILED(hr)) {
        const char *msg = err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "";
        fprintf(stderr, "[FXAA] compile %s failed 0x%08lX: %s\n", entry, (unsigned long)hr, msg);
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    return 1;
}

static int ensure_init(ID3D11Device *device)
{
    D3D11_SAMPLER_DESC sd;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC blend;
    D3D11_DEPTH_STENCIL_DESC ds;
    ID3D10Blob *vsb = NULL, *psb = NULL;
    HRESULT hr;

    if (s_failed)
        return 0;
    if (s_dev == device && s_vs && s_ps)
        return 1;
    halo2_fxaa_shutdown();
    s_dev = device;
    if (!compile("vs_main", "vs_5_0", &vsb) || !compile("ps_main", "ps_5_0", &psb))
        goto failed;
    hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    if (FAILED(hr))
        goto failed;
    hr = ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb),
                                        ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    if (FAILED(hr))
        goto failed;
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    vsb = psb = NULL;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = 0;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(device, &sd, &s_samp)))
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
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(device, &ds, &s_ds)))
        goto failed;
    fprintf(stderr, "[FXAA] post-process ready\n");
    return 1;
failed:
    if (vsb) ID3D10Blob_Release(vsb);
    if (psb) ID3D10Blob_Release(psb);
    s_failed = 1;
    fprintf(stderr, "[FXAA] initialization failed; FXAA stays off\n");
    return 0;
}

static int ensure_src(ID3D11Device *device, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (s_src && s_w == width && s_h == height)
        return 1;
    if (s_src_srv) { ID3D11ShaderResourceView_Release(s_src_srv); s_src_srv = NULL; }
    if (s_src) { ID3D11Texture2D_Release(s_src); s_src = NULL; }
    memset(&td, 0, sizeof td);
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &s_src);
    if (FAILED(hr))
        return 0;
    hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)s_src, NULL, &s_src_srv);
    if (FAILED(hr))
        return 0;
    s_w = width;
    s_h = height;
    return 1;
}

int halo2_fxaa_apply(ID3D11Device *device, ID3D11DeviceContext *ctx,
                     ID3D11Texture2D *target, ID3D11RenderTargetView *target_rtv,
                     UINT width, UINT height)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp, old_vp;
    UINT nvp = 1, nvp_out = 0;
    ID3D11RenderTargetView *old_rtv = NULL;
    ID3D11DepthStencilView *old_dsv = NULL;
    ID3D11ShaderResourceView *none_srv[1] = { NULL };
    ID3D11RenderTargetView *none_rtv[1] = { NULL };
    float rcp[4];
    HRESULT hr;

    if (!s_enabled || !device || !ctx || !target || !target_rtv || !width || !height)
        return 1;
    if (!ensure_init(device) || !ensure_src(device, width, height))
        return 0;

    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s_src, (ID3D11Resource *)target);

    rcp[0] = 1.0f / (float)width;
    rcp[1] = 1.0f / (float)height;
    rcp[2] = rcp[3] = 0;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
        return 0;
    memcpy(mapped.pData, rcp, sizeof rcp);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);

    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &old_rtv, &old_dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &nvp, NULL);
    nvp_out = nvp = 1;
    ID3D11DeviceContext_RSGetViewports(ctx, &nvp_out, &old_vp);

    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &target_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &s_src_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);
    ID3D11DeviceContext_OMSetBlendState(ctx, s_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, s_ds, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, none_srv);

    ID3D11DeviceContext_OMSetRenderTargets(ctx, old_rtv ? 1 : 0, old_rtv ? &old_rtv : none_rtv, old_dsv);
    if (nvp_out)
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &old_vp);
    if (old_rtv) ID3D11RenderTargetView_Release(old_rtv);
    if (old_dsv) ID3D11DepthStencilView_Release(old_dsv);
    return 1;
}
