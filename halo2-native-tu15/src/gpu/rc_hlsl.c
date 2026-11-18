/*
 * HLSL pixel shaders from NV2A register-combiner state.
 *
 * Adapted from Blood Wake's bw_rc_hlsl.c (bloodwake-recomp at
 * e631edddfe56d22344e9f696923aeb6d78a9e7ba, MIT licence; see
 * THIRD_PARTY_NOTICES.md). Generic code only.
 *
 * Generates, for one combiner configuration, the shader that does on the GPU
 * what the framework's CPU path does per pixel: the texture stages' fetch by
 * SHADER_STAGE_PROGRAM mode (rc_stage_fetch in nv2a_pb_exec.c), up to eight
 * general combiner stages over the 16-register file, and the final combiner
 * (nv2a_combiner.c, itself after xemu's psh.c). Rule for rule the same, so
 * the GPU back end draws what the CPU rasteriser draws.
 *
 * Texture-shader modes 6-18 (bump environment, dot product, dot-reflect
 * cube maps, dependent reads) follow xemu's psh.c: the dot mapping comes
 * from SET_DOT_RGBMAPPING and the input stage from SET_SHADER_OTHER_STAGE_
 * INPUT. Halo 2 uses DOT_PRODUCT + DOT_RFLCT_DIFF + DOT_RFLCT_SPEC for its
 * bump-mapped cube-map reflections (visors, weapons, metal); those read
 * black before. Still unsupported: BRDF, DOT_STR_3D (no 3D textures here)
 * and DOT_ZW's depth replacement. Only texture level 0 is sampled. Colours and fog are
 * interpolated affinely in screen space and fog is clamped per vertex, as
 * the CPU rasteriser does; texture coordinates are perspective-correct.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_hlsl.h"

typedef struct {
    char *p;
    size_t n, cap;
    int overflow;
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int w;
    if (b->overflow)
        return;
    va_start(ap, fmt);
    w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->cap - b->n) {
        b->overflow = 1;
        return;
    }
    b->n += (size_t)w;
}

/* read_rgb / read_alpha of nv2a_combiner.c, for one input byte. */
static void put_rgb(Buf *b, const char *var, uint32_t byte)
{
    put(b, "  float3 %s = rd3(R[%u], %d, %u);\n", var, byte & 0xF,
        (byte & 0x10) ? 1 : 0, (byte >> 5) & 7);
}

static void put_alpha(Buf *b, const char *var, uint32_t byte)
{
    put(b, "  float %s = rda(R[%u], %d, %u);\n", var, byte & 0xF,
        (byte & 0x10) ? 1 : 0, (byte >> 5) & 7);
}

/* The input stage of a dependent mode (xemu psh.c input_tex) and the dot
 * mapping of stage st (1..3). */
static int tex_input(const GpuRcKey *k, int st)
{
    int in = st <= 1 ? 0 : st == 2 ? (int)((k->other_input >> 16) & 0xF)
                                   : (int)((k->other_input >> 20) & 0xF);
    return in < st ? in : st - 1;
}

static int tex_dotmap(const GpuRcKey *k, int st)
{
    return st < 1 ? 0 : (int)((k->dot_mapping >> (4 * (st - 1))) & 7);
}

/* D[st] = dot(texcoord st, mapped colour of the input stage). */
static void put_dot(Buf *b, const GpuRcKey *k, int st)
{
    put(b, "  D[%d] = dot(tc[%d].xyz, dm(T[%d], %d));\n", st, st, tex_input(k, st),
        tex_dotmap(k, st));
}

static void put_sample2d(Buf *b, const GpuRcKey *k, int st, const char *uv)
{
    if (!k->tex_valid[st] || k->tex_cube[st]) {
        put(b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
        return;
    }
    put(b, "  { float2 uv = %s;\n", uv);
    if (!k->tex_normalized[st])
        put(b, "    uv *= texinfo[%d].xy;\n", st);
    put(b, "    T[%d] = t%d.Sample(s%d, uv); }\n", st, st, st);
}

static void put_samplecube(Buf *b, const GpuRcKey *k, int st, const char *dir)
{
    if (!k->tex_valid[st] || !k->tex_cube[st]) {
        put(b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
        return;
    }
    put(b, "  T[%d] = t%d.Sample(s%d, %s);\n", st, st, st, dir);
}

/* Texture-shader modes 6-18, after xemu's psh.c. */
static void put_texmode(Buf *b, const GpuRcKey *k, int st, uint32_t mode)
{
    char expr[160];
    int in = tex_input(k, st);
    if (st == 0) {                  /* every dependent mode needs an earlier stage */
        put(b, "  T[0] = float4(0, 0, 0, 0);\n");
        return;
    }
    switch (mode) {
    case 6: case 7:                 /* BUMPENVMAP, BUMPENVMAP_LUM */
        put(b, "  { float2 ds = float2(sg3(T[%d].b), sg3(T[%d].g));\n", in, in);
        put(b, "    ds = float2(bumpmat[%d].x * ds.x + bumpmat[%d].z * ds.y,"
               " bumpmat[%d].y * ds.x + bumpmat[%d].w * ds.y);\n", st, st, st, st);
        put(b, "    float2 bev = tc[%d].xy + ds;\n", st);
        put_sample2d(b, k, st, "bev");
        if (mode == 7)
            put(b, "    T[%d] *= T[%d].r * bumplum[%d].x + bumplum[%d].y;\n", st, in, st, st);
        put(b, "  }\n");
        break;
    case 9:                         /* DOT_ST */
        put_dot(b, k, st);
        snprintf(expr, sizeof expr, "float2(D[%d], D[%d])", st - 1, st);
        put_sample2d(b, k, st, expr);
        break;
    case 11:                        /* DOT_RFLCT_DIFF: normal from stages 1-3 */
        put_dot(b, k, st);
        if (st == 2) {
            put(b, "  float dn2 = dot(tc[3].xyz, dm(T[%d], %d));\n", tex_input(k, 3),
                tex_dotmap(k, 3));
            put_samplecube(b, k, st, "float3(D[1], D[2], dn2)");
        } else {
            put(b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
        }
        break;
    case 12: case 18:               /* DOT_RFLCT_SPEC(_CONST): reflect the eye */
        put_dot(b, k, st);
        if (st == 3) {
            put(b, "  { float3 n = float3(D[1], D[2], D[3]);\n");
            if (mode == 12)
                put(b, "    float3 e = float3(tc[1].w, tc[2].w, tc[3].w);\n");
            else
                put(b, "    float3 e = eyevec.xyz;\n");
            put(b, "    float nn = dot(n, n);\n"
                   "    float3 rv = nn != 0.0 ? 2.0 * n * dot(n, e) / nn - e : -e;\n");
            put_samplecube(b, k, st, "rv");
            put(b, "  }\n");
        } else {
            put(b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
        }
        break;
    case 14:                        /* DOT_STR_CUBE */
        put_dot(b, k, st);
        if (st == 3)
            put_samplecube(b, k, st, "float3(D[1], D[2], D[3])");
        else
            put(b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
        break;
    case 15:                        /* DEPENDENT_AR */
        snprintf(expr, sizeof expr, "float2(T[%d].a, T[%d].r)", in, in);
        put_sample2d(b, k, st, expr);
        break;
    case 16:                        /* DEPENDENT_GB */
        snprintf(expr, sizeof expr, "float2(T[%d].g, T[%d].b)", in, in);
        put_sample2d(b, k, st, expr);
        break;
    case 10: case 13: case 17:      /* DOT_ZW, DOT_STR_3D, DOT_PRODUCT */
        put_dot(b, k, st);
        put(b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
        break;
    default:                        /* BRDF and reserved modes: zero */
        put(b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
        break;
    }
}

size_t gpu_rc_hlsl(const GpuRcKey *k, char *out, size_t cap)
{
    Buf b = { out, 0, cap, 0 };
    uint32_t n = k->control & 0xFF, flags = k->control >> 8;
    int st;
    uint32_t s;

    if (n > 8)
        n = 8;
    put(&b, GPU_RC_CBUFFER GPU_WINDOW_CLIP_HLSL GPU_SHADOW_HLSL);
    /* A stage bound to a six-face cube map is a TextureCube. */
    for (st = 0; st < 4; st++)
        put(&b, "%s t%d : register(t%d);\n", k->tex_cube[st] ? "TextureCube" : "Texture2D", st, st);
    put(&b,
        "SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
        "SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n");
    {   /* Colours and fog interpolate perspective-correctly, as on the NV2A and in
         * xemu (smooth vtxD0/vtxD1/vtxFog). Screen-linear interpolation bent
         * vertex-colour light falloff across large floor triangles into hard
         * creases that moved with the camera (Halo 2 fire/lamp light passes).
         * RECOMP_GPU_AFFINE_COLOR=1 restores the old screen-linear rule. */
        static int affine = -1;
        const char *q;
        if (affine < 0) {
            const char *e = getenv("RECOMP_GPU_AFFINE_COLOR");
            affine = e && e[0] == '1';
        }
        q = affine ? "noperspective " : "";
        put(&b, "struct PSIn { float4 p : SV_Position; %sfloat4 d0 : COLOR0;\n"
                "  %sfloat4 d1 : COLOR1; %sfloat fog : FOG;\n"
                "  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
                "  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; float fogp : FOG1; };\n",
            q, q, q);
    }
    put(&b,
        /* map_in */
        "float mi(float x, int m) { float p = max(x, 0.0);\n"
        "  if (m == 0) return p; if (m == 1) return 1.0 - saturate(x);\n"
        "  if (m == 2) return 2.0 * p - 1.0; if (m == 3) return -2.0 * p + 1.0;\n"
        "  if (m == 4) return p - 0.5; if (m == 5) return -p + 0.5;\n"
        "  if (m == 6) return x; return -x; }\n"
        "float3 rd3(float4 r, int arep, int m) { float3 v = arep ? r.aaa : r.rgb;\n"
        "  return float3(mi(v.x, m), mi(v.y, m), mi(v.z, m)); }\n"
        "float rda(float4 r, int arep, int m) { return mi(arep ? r.a : r.b, m); }\n"
        /* map_out */
        "float3 mo(float3 x, int m) {\n"
        "  if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0;\n"
        "  if (m == 0x18) return (x - 0.5) * 2.0; if (m == 0x20) return x * 4.0;\n"
        "  if (m == 0x30) return x * 0.5; return x; }\n"
        /* xemu psh.c sign1/sign2/sign3 and the dot mappings */
        "float sg1(float x) { x *= 255.0; return (x - 128.0) / 127.0; }\n"
        "float sg2(float x) { x *= 255.0; return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }\n"
        "float sg3(float x) { x = round(x * 255.0); return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }\n"
        "float3 dm(float4 c, int m) {\n"
        "  if (m == 1) return float3(sg1(c.r), sg1(c.g), sg1(c.b));\n"
        "  if (m == 2) return float3(sg2(c.r), sg2(c.g), sg2(c.b));\n"
        "  if (m == 3) return float3(sg3(c.r), sg3(c.g), sg3(c.b));\n"
        "  if (m >= 4) { float hi = (round(c.a * 255.0) * 256.0 + round(c.r * 255.0));\n"
        "    float lo = (round(c.g * 255.0) * 256.0 + round(c.b * 255.0));\n"
        "    if (m == 4) return float3(hi / 65535.0, lo / 65535.0, 1.0);\n"
        "    hi = hi >= 32768.0 ? (hi - 65536.0) / 32767.0 : hi / 32767.0;\n"
        "    lo = lo >= 32768.0 ? (lo - 65536.0) / 32767.0 : lo / 32767.0;\n"
        "    return float3(hi, lo, sqrt(max(0.0, 1.0 - hi * hi - lo * lo))); }\n"
        "  return c.rgb; }\n");
    if (k->wbuffer)
        /* SV_Position.w is the clip w, interpolated perspective-correctly as
         * the NV2A w-buffer does; depth.w is the decal bias (depth units). */
        put(&b,
            "float4 ps_main(PSIn i, out float od : SV_Depth) : SV_Target {\n"
            "  od = (i.p.w - depth.x) * depth.y;\n"
            "  if (depth.w > 0.0) od -= depth.w + max(abs(ddx(od)), abs(ddy(od)));\n"
            "  od = saturate(od);\n"
            "  if (!window_allows(i.p.xy)) discard;\n");
    else
        put(&b,
            "float4 ps_main(PSIn i) : SV_Target {\n"
            "  if (!window_allows(i.p.xy)) discard;\n");
    put(&b,
        "  float4 R[16];\n"
        "  float4 tc[4] = { i.tc0, i.tc1, i.tc2, i.tc3 };\n"
        "  float4 T[4] = { float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0) };\n"
        "  float D[4] = { 0.0, 0.0, 0.0, 0.0 };\n");

    /* rc_stage_fetch, per stage. */
    for (st = 0; st < 4; st++) {
        uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
        switch (mode) {
        case 0:
            put(&b, "  T[%d] = float4(0, 0, 0, 1);\n", st);
            break;
        case 1: case 2: case 3:
            if (!k->tex_valid[st]) {
                /* Invalid stage: transparent black. White (1,1,1,1) showed up as
                 * a huge circle on additive glow / flare meshes when a stage
                 * failed softly. Black is also safe for opaque sampling. */
                put(&b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
                break;
            }
            if (mode == 3 && k->tex_cube[st]) {
                /* CUBEMAP: the coordinate is a direction; D3D picks the face
                 * the same way the CPU path's rc_stage_fetch does. */
                put(&b, "  T[%d] = t%d.Sample(s%d, tc[%d].xyz);\n", st, st, st, st);
                break;
            }
            if (mode == 3)          /* CUBEMAP on a 2D texture: face 0, as the CPU path */
                put(&b, "  { float2 uv = tc[%d].xy;\n", st);
            else
                put(&b, "  { float q = tc[%d].w != 0.0 ? tc[%d].w : 1.0;"
                        " float2 uv = tc[%d].xy / q;\n", st, st, st);
            if (!k->tex_normalized[st])
                put(&b, "    uv *= texinfo[%d].xy;\n", st);
            if (k->tex_depth[st]) {
                put(&b, "    float d = t%d.Sample(s%d, uv).r * 16777215.0;\n", st, st);
                if (mode == 2)
                    put(&b, "    float z = clamp(tc[%d].z / q, 0.0, 16777215.0);\n", st);
                else
                    put(&b, "    float z = 0.0;\n");
                put(&b, "    T[%d] = shadow_compare(d, z, %uu).xxxx; }\n",
                    st, k->shadow_func);
            } else {
                put(&b, "    T[%d] = t%d.Sample(s%d, uv); }\n", st, st, st);
            }
            break;
        case 4:
            put(&b, "  T[%d] = saturate(tc[%d]);\n", st, st);
            break;
        case 5: {
            int j;
            put(&b, "  T[%d] = float4(0, 0, 0, 0);\n", st);
            for (j = 0; j < 4; j++) {
                int ge = (k->clip_plane_mode >> (st * 4 + j)) & 1;
                put(&b, "  if (tc[%d][%d] %s 0.0) discard;\n", st, j, ge ? ">=" : "<");
            }
            break;
        }
        default:
            put_texmode(&b, k, st, mode);
            break;
        }
    }

    /* The register file (nv2a_rc_eval's start). */
    put(&b,
        "  R[0] = 0; R[1] = 0; R[2] = 0; R[6] = 0; R[7] = 0;\n"
        "  R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 0;\n"
        "  R[3] = float4(fogc.rgb, i.fog); R[4] = i.d0; R[5] = i.d1;\n"
        "  R[8] = T[0]; R[9] = T[1]; R[10] = T[2]; R[11] = T[3];\n"
        "  R[12].a = %s;\n", (k->stage_program & 0x1F) ? "T[0].a" : "1.0");

    for (s = 0; s < n; s++) {
        uint32_t icw = k->color_icw[s], ocw = k->color_ocw[s];
        uint32_t aicw = k->alpha_icw[s], aocw = k->alpha_ocw[s];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        put(&b, "  { /* stage %u */\n", s);
        put(&b, "  R[1] = C0[%u]; R[2] = C1[%u];\n",
            (flags & 0x010) ? s : 0, (flags & 0x100) ? s : 0);
        if (flags & 1)
            put(&b, "  bool mux = R[12].a >= 0.5;\n");
        else
            put(&b, "  bool mux = (((int)(R[12].a * 255.0)) & 1) != 0;\n");
        put_rgb(&b, "A", icw >> 24); put_rgb(&b, "B", icw >> 16);
        put_rgb(&b, "C", icw >> 8);  put_rgb(&b, "D", icw);
        put_alpha(&b, "aA", aicw >> 24); put_alpha(&b, "aB", aicw >> 16);
        put_alpha(&b, "aC", aicw >> 8);  put_alpha(&b, "aD", aicw);
        put(&b, "  float3 pab = %s; float3 pcd = %s;\n",
            (fl & 2) ? "dot(A, B).xxx" : "A * B", (fl & 1) ? "dot(C, D).xxx" : "C * D");
        put(&b, "  float3 sum = %s;\n", (fl & 4) ? "mux ? pcd : pab" : "pab + pcd");
        put(&b, "  float3 ab = clamp(mo(pab, %u), -1.0, 1.0);"
                " float3 cd = clamp(mo(pcd, %u), -1.0, 1.0);"
                " float3 ms = clamp(mo(sum, %u), -1.0, 1.0);\n",
            fl & 0x38, fl & 0x38, fl & 0x38);
        put(&b, "  float apab = aA * aB, apcd = aC * aD;\n");
        put(&b, "  float asum = %s;\n", (afl & 4) ? "mux ? apcd : apab" : "apab + apcd");
        put(&b, "  float aab = clamp(mo(apab.xxx, %u).x, -1.0, 1.0);"
                " float acd = clamp(mo(apcd.xxx, %u).x, -1.0, 1.0);"
                " float ams = clamp(mo(asum.xxx, %u).x, -1.0, 1.0);\n",
            afl & 0x38, afl & 0x38, afl & 0x38);
        if ((ocw >> 4) & 0xF) {
            put(&b, "  R[%u].rgb = ab;\n", (ocw >> 4) & 0xF);
            if (fl & 0x80)
                put(&b, "  R[%u].a = ab.b;\n", (ocw >> 4) & 0xF);
        }
        if (ocw & 0xF) {
            put(&b, "  R[%u].rgb = cd;\n", ocw & 0xF);
            if (fl & 0x40)
                put(&b, "  R[%u].a = cd.b;\n", ocw & 0xF);
        }
        if ((ocw >> 8) & 0xF)
            put(&b, "  R[%u].rgb = ms;\n", (ocw >> 8) & 0xF);
        if ((aocw >> 4) & 0xF)
            put(&b, "  R[%u].a = aab;\n", (aocw >> 4) & 0xF);
        if (aocw & 0xF)
            put(&b, "  R[%u].a = acd;\n", aocw & 0xF);
        if ((aocw >> 8) & 0xF)
            put(&b, "  R[%u].a = ams;\n", (aocw >> 8) & 0xF);
        put(&b, "  R[0] = 0; }\n");
    }

    if (k->final0 || k->final1) {
        uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
        put(&b, "  { /* final combiner */\n  R[1] = FC0; R[2] = FC1;\n");
        put(&b, "  float3 va = %s; float3 vb = %s;\n",
            (ff & 0x40) ? "1.0 - R[5].rgb" : "R[5].rgb",
            (ff & 0x20) ? "1.0 - R[12].rgb" : "R[12].rgb");
        put(&b, "  R[14] = float4(%s, 0.0);\n", (ff & 0x80) ? "saturate(va + vb)" : "va + vb");
        put_rgb(&b, "E", f1 >> 24);
        put_rgb(&b, "F", f1 >> 16);
        put(&b, "  R[15] = float4(E * F, 0.0);\n");
        put_rgb(&b, "A", f0 >> 24); put_rgb(&b, "B", f0 >> 16);
        put_rgb(&b, "C", f0 >> 8);  put_rgb(&b, "D", f0);
        put_alpha(&b, "G", f1 >> 8);
        put(&b, "  float4 o = float4(D + C * (1.0 - A) + B * A, G); o = saturate(o);\n");
    } else {
        put(&b, "  { float4 o = saturate(R[12]);\n");
    }
    /* alpha_test_pass, on the final alpha as 0..255 */
    put(&b,
        "  if (alpha.z > 0.5) { int v = (int)(o.a * 255.0 + 0.5), r = (int)alpha.y; int f = (int)alpha.x;\n"
        "    bool ok = (f == 7) || (f == 1 && v < r) || (f == 2 && v == r) || (f == 3 && v <= r)\n"
        "      || (f == 4 && v > r) || (f == 5 && v != r) || (f == 6 && v >= r);\n"
        "    if (!ok) discard; }\n"
        "  return o; }\n"
        "}\n");
    if (b.overflow)
        return 0;
    return b.n;
}
