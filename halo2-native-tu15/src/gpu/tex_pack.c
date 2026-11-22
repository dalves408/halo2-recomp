/* tex_pack.c -- hi-res texture pack loader (see tex_pack.h).
 *
 * Pack layout (analysis/textures/README.md has the naming scheme):
 *   <pack>\**\<hash16>_<W>x<H>_<anything>.dds
 *       hash16 is hash_bytes() over the face-0 level-0 guest bytes of the
 *       original texture and WxH its original size, both copied from the
 *       extracted PNG's name. The DDS may be any power-of-two multiple of
 *       WxH (same aspect) and should carry a full mip chain. Cube maps are
 *       one DDS cubemap (six faces) named after face 0.
 *   <pack>\mip_aliases.txt (optional)
 *       "<alias16> <w> <h> <base16> <W> <H>" per line: the game also uploads
 *       textures starting at their second or third mip (distant objects,
 *       streaming), which hash differently; these lines send those uploads
 *       to the same DDS.
 *
 * Formats: BC1-BC5, BC7 (DX10 header or the legacy DXT/ATI FourCCs) and
 * 32-bit RGBA/BGRA. sRGB variants load as their UNORM twins: the game's
 * shaders expect the original (non-linearised) values.
 *
 * Every DDS loads once, on the first texture that asks for it, and stays on
 * the GPU; texture entries hold their own references. The pack can be
 * switched off and on while the game runs (texpack_set_active). */
#include "tex_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct {
    uint64_t hash;
    uint32_t w, h;
    int file;               /* index into s_files */
} PackKey;

typedef struct {
    char *path;
    int state;              /* 0 not loaded, 1 loaded, -1 unusable */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    unsigned levels, w, h, cube;
} PackFile;

static int s_init;
static int s_enabled;
static volatile int s_active = 1;   /* the hi-res textures setting */
static PackKey *s_keys;
static int s_nkeys, s_capkeys;
static PackFile *s_files;
static int s_nfiles, s_capfiles;
static int s_log;
static unsigned s_hits, s_loaded;

static int key_cmp(const void *a, const void *b)
{
    const PackKey *x = (const PackKey *)a, *y = (const PackKey *)b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    if (x->w != y->w) return x->w < y->w ? -1 : 1;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    return 0;
}

static void add_key(uint64_t hash, uint32_t w, uint32_t h, int file)
{
    if (s_nkeys == s_capkeys) {
        int cap = s_capkeys ? s_capkeys * 2 : 1024;
        PackKey *k = (PackKey *)realloc(s_keys, (size_t)cap * sizeof *k);
        if (!k) return;
        s_keys = k; s_capkeys = cap;
    }
    s_keys[s_nkeys].hash = hash;
    s_keys[s_nkeys].w = w;
    s_keys[s_nkeys].h = h;
    s_keys[s_nkeys].file = file;
    s_nkeys++;
}

static const PackKey *find_key(uint64_t hash, uint32_t w, uint32_t h)
{
    PackKey k;
    k.hash = hash; k.w = w; k.h = h; k.file = 0;
    return s_nkeys ? (const PackKey *)bsearch(&k, s_keys, (size_t)s_nkeys, sizeof k, key_cmp)
                   : NULL;
}

/* "<16 hex>_<W>x<H>_..." -> hash, W, H. */
static int parse_name(const char *name, uint64_t *hash, uint32_t *w, uint32_t *h)
{
    uint64_t v = 0;
    char *end;
    unsigned long a, b;
    int i;
    for (i = 0; i < 16; i++) {
        char c = name[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint64_t)d;
    }
    if (name[16] != '_') return 0;
    a = strtoul(name + 17, &end, 10);
    if (*end != 'x' && *end != 'X') return 0;
    b = strtoul(end + 1, &end, 10);
    if (*end != '_' && *end != '.') return 0;
    if (!a || !b || a > 4096 || b > 4096) return 0;
    *hash = v; *w = (uint32_t)a; *h = (uint32_t)b;
    return 1;
}

/* Textures the game keeps even when the pack has them: the zoom overlays
 * (ui\hud\bitmaps\new_hud\scope_masks -- battle rifle, sniper, beam rifle,
 * carbine, binoculars). They are thin line art drawn at their own size;
 * upscaled, the lines go thick and the dotted rim is lost. Matched on the
 * tag name the pack's file names end with. */
static int pack_skip(const char *name)
{
    static const char *const skip[] = { "_scope_mask_", "_binoculars_elite", "_binoculars_human" };
    char low[MAX_PATH];
    size_t i, n = strlen(name);
    if (n >= sizeof low)
        return 0;
    for (i = 0; i <= n; i++)
        low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    for (i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (strstr(low, skip[i]))
            return 1;
    return 0;
}

static void scan_dir(const char *dir, int depth)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hf;
    if (depth > 8 || snprintf(pattern, sizeof pattern, "%s\\*", dir) >= (int)sizeof pattern)
        return;
    hf = FindFirstFileA(pattern, &fd);
    if (hf == INVALID_HANDLE_VALUE)
        return;
    do {
        char full[MAX_PATH];
        size_t n;
        uint64_t hash;
        uint32_t w, h;
        if (fd.cFileName[0] == '.')
            continue;
        if (snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName) >= (int)sizeof full)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(full, depth + 1);
            continue;
        }
        n = strlen(fd.cFileName);
        if (n < 5 || _stricmp(fd.cFileName + n - 4, ".dds") != 0)
            continue;
        if (!parse_name(fd.cFileName, &hash, &w, &h))
            continue;
        if (pack_skip(fd.cFileName))
            continue;
        if (s_nfiles == s_capfiles) {
            int cap = s_capfiles ? s_capfiles * 2 : 1024;
            PackFile *f = (PackFile *)realloc(s_files, (size_t)cap * sizeof *f);
            if (!f) break;
            s_files = f; s_capfiles = cap;
        }
        memset(&s_files[s_nfiles], 0, sizeof s_files[s_nfiles]);
        s_files[s_nfiles].path = _strdup(full);
        if (!s_files[s_nfiles].path)
            continue;
        add_key(hash, w, h, s_nfiles);
        s_nfiles++;
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

static void dedupe_keys(void)
{
    int i, o = 0;
    qsort(s_keys, (size_t)s_nkeys, sizeof *s_keys, key_cmp);
    for (i = 0; i < s_nkeys; i++)
        if (!o || key_cmp(&s_keys[o - 1], &s_keys[i]) != 0)
            s_keys[o++] = s_keys[i];
    s_nkeys = o;
}

static int load_aliases(const char *dir)
{
    char path[MAX_PATH], line[256];
    FILE *f;
    int added = 0, base_n = s_nkeys;
    if (snprintf(path, sizeof path, "%s\\mip_aliases.txt", dir) >= (int)sizeof path)
        return 0;
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        unsigned aw, ah, bw, bh;
        const PackKey *base;
        if (sscanf(line, "%llx %u %u %llx %u %u", &a, &aw, &ah, &b, &bw, &bh) != 6)
            continue;
        /* Only the base keys are sorted so far, and aliases are appended
         * after them: search the base range alone. */
        {
            PackKey k;
            k.hash = b; k.w = bw; k.h = bh; k.file = 0;
            base = (const PackKey *)bsearch(&k, s_keys, (size_t)base_n, sizeof k, key_cmp);
        }
        if (!base)
            continue;
        add_key(a, aw, ah, base->file);
        added++;
    }
    fclose(f);
    return added;
}

static void texpack_init(void)
{
    const char *e = getenv("RECOMP_TEX_PACK");
    char dir[MAX_PATH];
    DWORD attr;
    int aliases;
    s_init = 1;
    if (e && e[0] == '0' && !e[1])
        return;
    if (e && *e)
        snprintf(dir, sizeof dir, "%s", e);
    else
        snprintf(dir, sizeof dir, "textures");
    attr = GetFileAttributesA(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;
    e = getenv("RECOMP_TEX_PACK_LOG");
    s_log = e && e[0] == '1';
    scan_dir(dir, 0);
    dedupe_keys();
    aliases = load_aliases(dir);
    dedupe_keys();
    s_enabled = s_nfiles > 0;
    fprintf(stderr, "[TEXPACK] %d replacement texture(s), %d mip alias(es) from %s\n",
            s_nfiles, aliases, dir);
    fflush(stderr);
}

int texpack_enabled(void)
{
    if (!s_active)
        return 0;
    if (!s_init)
        texpack_init();
    return s_enabled;
}

void texpack_set_active(int on)
{
    s_active = on ? 1 : 0;
}

int texpack_active(void)
{
    return s_active;
}

/* ---- DDS ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | \
                            (uint32_t)(d) << 24)

/* Bytes per 4x4 block (compressed) or per texel; 0 for an unsupported format.
 * *fmt comes back as the UNORM format the texture is created with. */
static unsigned format_info(DXGI_FORMAT *fmt, int *compressed)
{
    *compressed = 1;
    switch (*fmt) {
    case DXGI_FORMAT_BC1_UNORM_SRGB: *fmt = DXGI_FORMAT_BC1_UNORM; /* fall through */
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_TYPELESS:
        *fmt = DXGI_FORMAT_BC1_UNORM; return 8;
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS:
        *fmt = DXGI_FORMAT_BC4_UNORM; return 8;
    case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM: *fmt = DXGI_FORMAT_BC2_UNORM; return 16;
    case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM: *fmt = DXGI_FORMAT_BC3_UNORM; return 16;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM: *fmt = DXGI_FORMAT_BC5_UNORM; return 16;
    case DXGI_FORMAT_BC7_UNORM_SRGB: case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM: *fmt = DXGI_FORMAT_BC7_UNORM; return 16;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        *compressed = 0; *fmt = DXGI_FORMAT_R8G8B8A8_UNORM; return 4;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        *compressed = 0; *fmt = DXGI_FORMAT_B8G8R8A8_UNORM; return 4;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        *compressed = 0; *fmt = DXGI_FORMAT_B8G8R8X8_UNORM; return 4;
    default:
        return 0;
    }
}

static const char *load_dds(ID3D11Device *dev, PackFile *pf)
{
    D3D11_SUBRESOURCE_DATA init[6 * 16];
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint8_t *data = NULL;
    const uint8_t *p, *hdr;
    size_t size, off;
    long fsize;
    uint32_t w, h, mips, pf_flags, fourcc, faces = 1, face, level;
    unsigned unit;
    int compressed;
    FILE *f = fopen(pf->path, "rb");
    HRESULT hr;

    if (!f)
        return "cannot open";
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 128 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return "too short";
    }
    size = (size_t)fsize;
    data = (uint8_t *)malloc(size);
    if (!data || fread(data, 1, size, f) != size) {
        fclose(f);
        free(data);
        return "read failed";
    }
    fclose(f);
#define FAIL(msg) do { free(data); return msg; } while (0)
    if (rd32(data) != FOURCC('D', 'D', 'S', ' ') || rd32(data + 4) != 124)
        FAIL("not a DDS file");
    hdr = data + 4;
    h = rd32(hdr + 8);
    w = rd32(hdr + 12);
    mips = rd32(hdr + 24);
    pf_flags = rd32(hdr + 76);
    fourcc = rd32(hdr + 80);
    if (!mips) mips = 1;
    off = 128;
    if ((pf_flags & 4) && fourcc == FOURCC('D', 'X', '1', '0')) {
        uint32_t dim, misc, array;
        if (size < 148)
            FAIL("truncated DX10 header");
        fmt = (DXGI_FORMAT)rd32(data + 128);
        dim = rd32(data + 132);
        misc = rd32(data + 136);
        array = rd32(data + 140);
        if (dim != 3 /* TEXTURE2D */)
            FAIL("not a 2D texture");
        if (misc & 4) {
            if (array != 1) FAIL("cube arrays are not supported");
            faces = 6;
        } else if (array != 1) {
            FAIL("texture arrays are not supported");
        }
        off = 148;
    } else {
        if (pf_flags & 4) {
            switch (fourcc) {
            case FOURCC('D', 'X', 'T', '1'): fmt = DXGI_FORMAT_BC1_UNORM; break;
            case FOURCC('D', 'X', 'T', '2'):
            case FOURCC('D', 'X', 'T', '3'): fmt = DXGI_FORMAT_BC2_UNORM; break;
            case FOURCC('D', 'X', 'T', '4'):
            case FOURCC('D', 'X', 'T', '5'): fmt = DXGI_FORMAT_BC3_UNORM; break;
            case FOURCC('A', 'T', 'I', '1'):
            case FOURCC('B', 'C', '4', 'U'): fmt = DXGI_FORMAT_BC4_UNORM; break;
            case FOURCC('A', 'T', 'I', '2'):
            case FOURCC('B', 'C', '5', 'U'): fmt = DXGI_FORMAT_BC5_UNORM; break;
            default: FAIL("unsupported FourCC");
            }
        } else if ((pf_flags & 0x40) && rd32(hdr + 84) == 32) {
            uint32_t rmask = rd32(hdr + 88);
            int alpha = (pf_flags & 1) != 0;
            if (rmask == 0x00FF0000u)
                fmt = alpha ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
            else if (rmask == 0x000000FFu)
                fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            else
                FAIL("unsupported 32-bit channel layout");
        } else {
            FAIL("unsupported pixel format");
        }
        if (rd32(hdr + 108) & 0x200) {
            if ((rd32(hdr + 108) & 0xFC00) != 0xFC00)
                FAIL("cube map without all six faces");
            faces = 6;
        }
    }
    unit = format_info(&fmt, &compressed);
    if (!unit)
        FAIL("unsupported DXGI format");
    if (!w || !h || w > 16384 || h > 16384)
        FAIL("bad size");
    if (mips > 16)
        FAIL("too many mip levels");
    {
        uint32_t full = 1, m = w > h ? w : h;
        while (m > 1) { m >>= 1; full++; }
        if (mips > full) mips = full;
    }
    p = data + off;
    for (face = 0; face < faces; face++) {
        uint32_t lw = w, lh = h;
        for (level = 0; level < mips; level++) {
            size_t pitch, rows, bytes;
            if (compressed) {
                pitch = (size_t)((lw + 3) / 4) * unit;
                rows = (lh + 3) / 4;
            } else {
                pitch = (size_t)lw * unit;
                rows = lh;
            }
            bytes = pitch * rows;
            if ((size_t)(p - data) + bytes > size)
                FAIL("file shorter than its mip chain");
            init[face * mips + level].pSysMem = p;
            init[face * mips + level].SysMemPitch = (UINT)pitch;
            init[face * mips + level].SysMemSlicePitch = (UINT)bytes;
            p += bytes;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
    }
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = mips;
    td.ArraySize = faces;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    hr = ID3D11Device_CreateTexture2D(dev, &td, init, &pf->tex);
    if (FAILED(hr))
        FAIL("texture creation failed");
    memset(&sv, 0, sizeof sv);
    sv.Format = fmt;
    if (faces == 6) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = mips;
    } else {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = mips;
    }
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)pf->tex, &sv, &pf->srv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(pf->tex);
        pf->tex = NULL;
        FAIL("view creation failed");
    }
#undef FAIL
    free(data);
    pf->levels = mips;
    pf->w = w;
    pf->h = h;
    pf->cube = faces == 6;
    return NULL;
}

int texpack_lookup(ID3D11Device *dev, uint64_t hash, unsigned width, unsigned height,
                   int cube, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv,
                   unsigned *levels, unsigned *lod_extra)
{
    const PackKey *k;
    PackFile *pf;
    unsigned scale, extra;

    if (!texpack_enabled() || !dev)
        return 0;
    k = find_key(hash, width, height);
    if (!k)
        return 0;
    pf = &s_files[k->file];
    if (pf->state == 0) {
        const char *err = load_dds(dev, pf);
        pf->state = err ? -1 : 1;
        if (err) {
            fprintf(stderr, "[TEXPACK] %s: %s; the original texture stays\n", pf->path, err);
        } else {
            s_loaded++;
            if (s_log)
                fprintf(stderr, "[TEXPACK] loaded %s (%ux%u, %u levels%s)\n", pf->path,
                        pf->w, pf->h, pf->levels, pf->cube ? ", cube" : "");
        }
        fflush(stderr);
    }
    if (pf->state != 1)
        return 0;
    /* Same aspect, a power-of-two multiple of the size the game asked for. */
    if ((uint64_t)pf->w * height != (uint64_t)pf->h * width || pf->w < width ||
        pf->w % width || (pf->cube != 0) != (cube != 0)) {
        static unsigned warned;
        if (warned++ < 20)
            fprintf(stderr, "[TEXPACK] %s is %ux%u%s; the game wants %ux%u%s, skipped\n",
                    pf->path, pf->w, pf->h, pf->cube ? " cube" : "", width, height,
                    cube ? " cube" : "");
        return 0;
    }
    scale = pf->w / width;
    if (scale & (scale - 1))
        return 0;
    for (extra = 0; (1u << extra) < scale; extra++)
        ;
    ID3D11Texture2D_AddRef(pf->tex);
    ID3D11ShaderResourceView_AddRef(pf->srv);
    *tex = pf->tex;
    *srv = pf->srv;
    *levels = pf->levels;
    *lod_extra = extra;
    s_hits++;
    return 1;
}
