#define COBJMACROS
#include "screenshot.h"

#ifdef RECOMP_DEBUG_TOOLS

#include <windows.h>
#include <wincodec.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input/keyboard.h"
#include "kernel.h"

#define SHOT_PATH_MAX 1024

static ID3D11Texture2D *s_staging;
static uint32_t s_staging_w, s_staging_h;
static DXGI_FORMAT s_staging_fmt;
static int s_pending;
static int s_del_was_down;
static int s_com_ready;

static char s_last_map[512];
static char s_last_guest_path[512];
static void (*s_prev_path_hook)(const char *);
static char s_exe_sha256[65];
static char s_exe_path[MAX_PATH];
static int s_exe_hashed;
static uint64_t s_exe_size;

int halo2_screenshot_enabled(void)
{
    static int on = -1;
    const char *e;

    if (on >= 0)
        return on;
    e = getenv("RECOMP_DEBUG_SCREENSHOT");
    on = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
    if (on) {
        fprintf(stderr, "[SCREENSHOT] debug capture enabled (RECOMP_DEBUG_SCREENSHOT=1);"
                        " press DEL in the game window\n");
        fflush(stderr);
    }
    return on;
}

static void path_hook(const char *xbox_path)
{
    size_t n, i;
    int is_map = 0;

    if (s_prev_path_hook)
        s_prev_path_hook(xbox_path);
    if (!xbox_path || !*xbox_path)
        return;
    n = strlen(xbox_path);
    if (n >= sizeof s_last_guest_path)
        n = sizeof s_last_guest_path - 1;
    memcpy(s_last_guest_path, xbox_path, n);
    s_last_guest_path[n] = 0;
    if (n >= 4) {
        char ext[5];
        memcpy(ext, xbox_path + n - 4, 4);
        ext[4] = 0;
        for (i = 0; i < 4; i++)
            if (ext[i] >= 'A' && ext[i] <= 'Z')
                ext[i] = (char)(ext[i] - 'A' + 'a');
        is_map = (strcmp(ext, ".map") == 0);
    }
    if (is_map) {
        const char *base = xbox_path;
        const char *ch;
        int cache = 0;
        for (ch = xbox_path; *ch; ch++)
            if (*ch == '\\' || *ch == '/')
                base = ch + 1;
        if ((base[0] == 'c' || base[0] == 'C') &&
            (base[1] == 'a' || base[1] == 'A') &&
            (base[2] == 'c' || base[2] == 'C') &&
            (base[3] == 'h' || base[3] == 'H') &&
            (base[4] == 'e' || base[4] == 'E'))
            cache = 1;
        if (!cache)
            memcpy(s_last_map, s_last_guest_path, n + 1);
    }
}

void halo2_screenshot_runtime_init(void)
{
    static int once;
    if (once)
        return;
    once = 1;
    /* Path hook is only for the sidecar map name; skip it when the
     * hotkey is off so a debug-tools build matches release until DEL
     * capture is explicitly enabled. */
    if (!halo2_screenshot_enabled())
        return;
    s_prev_path_hook = g_xbox_path_hook;
    g_xbox_path_hook = path_hook;
}

static int screenshot_requested(void)
{
    /* Keys are recorded by the framebuffer window (WM_KEYDOWN), so a press
     * only appears here when that window received it (i.e. it had focus). */
    int down, pressed;

    if (!halo2_screenshot_enabled())
        return 0;
    down = xbox_FramebufferKeyDown(XBOX_VK_DELETE);
    pressed = down && !s_del_was_down;
    s_del_was_down = down;
    {
        /* Scripted capture for unattended test runs: when the file named by
         * RECOMP_DEBUG_SCREENSHOT_TRIGGER appears, take one shot (same PNG,
         * sidecar and draw list as DEL) and delete the file. */
        static const char *trigger = (const char *)-1;
        if (trigger == (const char *)-1)
            trigger = getenv("RECOMP_DEBUG_SCREENSHOT_TRIGGER");
        if (!pressed && trigger && trigger[0] &&
            GetFileAttributesA(trigger) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(trigger);
            pressed = 1;
        }
    }
    return pressed;
}

static int screenshot_exe_dir(char *out, size_t cap)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;

    if (!n || n >= MAX_PATH)
        return 0;
    slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    *slash = 0;
    if (strlen(path) >= cap)
        return 0;
    memcpy(out, path, strlen(path) + 1);
    return 1;
}

static int sha256_file(const char *path, char hex[65], uint64_t *size_out)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    UCHAR digest[32], buf[64 * 1024];
    DWORD obj_len = 0, cb = 0;
    PUCHAR obj = NULL;
    FILE *f;
    size_t n, i;
    int ok = 0;
    uint64_t total = 0;

    hex[0] = 0;
    if (size_out)
        *size_out = 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof obj_len, &cb, 0) < 0)
        goto done;
    obj = (PUCHAR)malloc(obj_len);
    if (!obj)
        goto done;
    if (BCryptCreateHash(alg, &hash, obj, obj_len, NULL, 0, 0) < 0)
        goto done;
    for (;;) {
        n = fread(buf, 1, sizeof buf, f);
        if (!n)
            break;
        total += n;
        if (BCryptHashData(hash, buf, (ULONG)n, 0) < 0)
            goto done;
    }
    if (ferror(f) || BCryptFinishHash(hash, digest, sizeof digest, 0) < 0)
        goto done;
    for (i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02X", digest[i]);
    hex[64] = 0;
    if (size_out)
        *size_out = total;
    ok = 1;
done:
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    free(obj);
    fclose(f);
    return ok;
}

static void ensure_exe_identity(void)
{
    if (s_exe_hashed)
        return;
    s_exe_hashed = 1;
    if (!GetModuleFileNameA(NULL, s_exe_path, MAX_PATH) ||
        !sha256_file(s_exe_path, s_exe_sha256, &s_exe_size)) {
        s_exe_sha256[0] = 0;
    }
}

static int screenshot_ensure_staging(ID3D11Device *dev, uint32_t width, uint32_t height,
                                     DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (s_staging && s_staging_w == width && s_staging_h == height && s_staging_fmt == format)
        return 1;
    if (s_staging) {
        ID3D11Texture2D_Release(s_staging);
        s_staging = NULL;
    }
    memset(&td, 0, sizeof td);
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s_staging);
    if (FAILED(hr) || !s_staging) {
        fprintf(stderr, "[SCREENSHOT] staging texture %ux%u failed 0x%08lX\n",
                width, height, (unsigned long)hr);
        fflush(stderr);
        s_staging = NULL;
        return 0;
    }
    s_staging_w = width;
    s_staging_h = height;
    s_staging_fmt = format;
    return 1;
}

static int screenshot_ensure_com(void)
{
    HRESULT hr;

    if (s_com_ready)
        return 1;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return 0;
    s_com_ready = 1;
    return 1;
}

static void expand_565_to_bgra(const uint8_t *src, UINT src_pitch,
                               uint8_t *dst, UINT dst_pitch,
                               uint32_t width, uint32_t height)
{
    uint32_t y, x;

    for (y = 0; y < height; ++y) {
        const uint16_t *in = (const uint16_t *)(src + (size_t)y * src_pitch);
        uint8_t *out = dst + (size_t)y * dst_pitch;
        for (x = 0; x < width; ++x) {
            uint16_t v = in[x];
            uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
            uint32_t g = (uint32_t)((v >> 5) & 0x3F) * 255u / 63u;
            uint32_t b = (uint32_t)(v & 0x1F) * 255u / 31u;
            out[x * 4 + 0] = (uint8_t)b;
            out[x * 4 + 1] = (uint8_t)g;
            out[x * 4 + 2] = (uint8_t)r;
            out[x * 4 + 3] = 255;
        }
    }
}

static void copy_bgra_opaque(const uint8_t *src, UINT src_pitch,
                             uint8_t *dst, UINT dst_pitch,
                             uint32_t width, uint32_t height)
{
    uint32_t y, x;

    for (y = 0; y < height; ++y) {
        const uint8_t *in = src + (size_t)y * src_pitch;
        uint8_t *out = dst + (size_t)y * dst_pitch;
        memcpy(out, in, (size_t)width * 4);
        for (x = 0; x < width; ++x)
            out[x * 4 + 3] = 255;
    }
}

static int write_bmp_bgra(const char *path, const uint8_t *bgra, uint32_t width, uint32_t height,
                          UINT pitch)
{
    FILE *f;
    uint32_t row = ((width * 3u) + 3u) & ~3u;
    uint32_t img = row * height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    f = fopen(path, "wb");
    if (!f)
        return 0;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &width, 4);
    memcpy(hdr + 22, &height, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f);
        return 0;
    }
    line = (uint8_t *)calloc(1, row);
    if (!line) {
        fclose(f);
        return 0;
    }
    for (y = 0; y < height; ++y) {
        const uint8_t *src = bgra + (size_t)(height - 1 - y) * pitch;
        for (x = 0; x < width; ++x) {
            line[x * 3 + 0] = src[x * 4 + 0];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 2];
        }
        if (fwrite(line, 1, row, f) != row) {
            free(line);
            fclose(f);
            return 0;
        }
    }
    free(line);
    fclose(f);
    return 1;
}

static int write_png_bgra(const wchar_t *wpath, const uint8_t *bgra, uint32_t width,
                          uint32_t height, UINT pitch)
{
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr;
    int ok = 0;

    if (!screenshot_ensure_com())
        return 0;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr) || !factory)
        return 0;
    if (FAILED(IWICImagingFactory_CreateStream(factory, &stream)) || !stream)
        goto done;
    if (FAILED(IWICStream_InitializeFromFilename(stream, wpath, GENERIC_WRITE)))
        goto done;
    if (FAILED(IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder)) ||
        !encoder)
        goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)))
        goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &props)) || !frame)
        goto done;
    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, props)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, width, height)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)))
        goto done;
    if (!IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_WritePixels(frame, height, pitch,
                                                 pitch * height, (BYTE *)bgra)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame)))
        goto done;
    if (FAILED(IWICBitmapEncoder_Commit(encoder)))
        goto done;
    ok = 1;
done:
    if (props)
        IPropertyBag2_Release(props);
    if (frame)
        IWICBitmapFrameEncode_Release(frame);
    if (encoder)
        IWICBitmapEncoder_Release(encoder);
    if (stream)
        IWICStream_Release(stream);
    if (factory)
        IWICImagingFactory_Release(factory);
    return ok;
}

static int make_unique_path(char *out, size_t cap, const char *dir, const char *ext,
                            const SYSTEMTIME *st)
{
    int n, seq;

    for (seq = 0; seq < 100; ++seq) {
        if (seq == 0)
            n = snprintf(out, cap, "%s\\halo2_%04u%02u%02u_%02u%02u%02u_%03u.%s",
                         dir, st->wYear, st->wMonth, st->wDay,
                         st->wHour, st->wMinute, st->wSecond, st->wMilliseconds, ext);
        else
            n = snprintf(out, cap, "%s\\halo2_%04u%02u%02u_%02u%02u%02u_%03u_%d.%s",
                         dir, st->wYear, st->wMonth, st->wDay,
                         st->wHour, st->wMinute, st->wSecond, st->wMilliseconds, seq, ext);
        if (n < 0 || (size_t)n >= cap)
            return 0;
        if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    return 0;
}

static int interesting_log_line(const char *line)
{
    static const char *keys[] = {
        "error", "warn", "fail", "alias", "unsupported", "reject",
        "conflict", "crash", "declined", "SCREENSHOT", "GPU-D3D11", "GPU-SCALE",
        "GPU-TEX", NULL
    };
    const char *k;
    char buf[256];
    size_t i, n;

    n = strlen(line);
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    for (i = 0; i < n; i++) {
        char c = line[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[n] = 0;
    for (k = keys[0], i = 0; keys[i]; i++) {
        const char *need = keys[i];
        char fold[32];
        size_t j, m = strlen(need);
        for (j = 0; j < m && j < sizeof fold - 1; j++) {
            char c = need[j];
            fold[j] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        fold[j] = 0;
        if (strstr(buf, fold))
            return 1;
    }
    return 0;
}


static int write_stderr_file_tail(FILE *out, const char *path)
{
    FILE *f;
    long size, start;
    char *buf;
    const char *line, *end;
    int emitted = 0;
    size_t got;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return 0; }
    start = size > 32768L ? size - 32768L : 0;
    if (fseek(f, start, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = (char *)malloc((size_t)(size - start) + 1);
    if (!buf) { fclose(f); return 0; }
    got = fread(buf, 1, (size_t)(size - start), f);
    fclose(f);
    buf[got] = 0;
    line = buf;
    if (start > 0) {
        const char *nl = strchr(line, '\n');
        if (nl)
            line = nl + 1;
    }
    fprintf(out, "stderr_tail (from %s):\n", path);
    for (; *line; line = end) {
        end = strchr(line, '\n');
        if (end)
            end++;
        else
            end = line + strlen(line);
        if (interesting_log_line(line)) {
            fwrite("  ", 1, 2, out);
            fwrite(line, 1, (size_t)(end - line), out);
            if (end > line && end[-1] != '\n')
                fputc('\n', out);
            emitted++;
        }
    }
    if (!emitted)
        fprintf(out, "  (no matching warning/error lines in the last %lu bytes)\n",
                (unsigned long)got);
    free(buf);
    return 1;
}

static void write_stderr_tail(FILE *out)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    LARGE_INTEGER size, pos, start, saved;
    char *buf = NULL;
    DWORD got = 0, chunk;
    const char *line, *end, *nl;
    int count = 0, emitted = 0;

    fflush(stderr);
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK)
        return;
    saved.QuadPart = 0;
    if (!SetFilePointerEx(h, saved, &saved, FILE_CURRENT))
        return;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
        return;
    }
    chunk = size.QuadPart > 32768 ? 32768 : (DWORD)size.QuadPart;
    start.QuadPart = size.QuadPart - chunk;
    buf = (char *)malloc(chunk + 1);
    if (!buf) {
        SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
        return;
    }
    if (!SetFilePointerEx(h, start, NULL, FILE_BEGIN) ||
        !ReadFile(h, buf, chunk, &got, NULL)) {
        free(buf);
        SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
        return;
    }
    SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    buf[got] = 0;
    line = buf;
    /* Skip a possible partial first line unless we started at 0. */
    if (start.QuadPart > 0) {
        nl = strchr(line, '\n');
        if (nl)
            line = nl + 1;
    }
    fprintf(out, "stderr_tail:\n");
    for (; *line; line = end) {
        end = strchr(line, '\n');
        if (end)
            end++;
        else
            end = line + strlen(line);
        if (interesting_log_line(line)) {
            const char *p = end;
            fwrite("  ", 1, 2, out);
            fwrite(line, 1, (size_t)(p - line), out);
            if (p > line && p[-1] != '\n')
                fputc('\n', out);
            emitted++;
        }
        count++;
    }
    if (!emitted)
        fprintf(out, "  (no matching warning/error lines in the last %lu bytes of stderr)\n",
                (unsigned long)got);
    free(buf);
    (void)pos;
}

static void write_recomp_env(FILE *out)
{
    LPWCH env = GetEnvironmentStringsW();
    LPWCH p;
    int n = 0;

    fprintf(out, "recomp_env:\n");
    if (!env) {
        fprintf(out, "  (unavailable)\n");
        return;
    }
    for (p = env; *p; p += wcslen(p) + 1) {
        char line[1024];
        int k;
        if (wcsncmp(p, L"RECOMP_", 7) != 0)
            continue;
        k = WideCharToMultiByte(CP_UTF8, 0, p, -1, line, (int)sizeof line, NULL, NULL);
        if (k <= 0)
            continue;
        fprintf(out, "  %s\n", line);
        n++;
    }
    FreeEnvironmentStringsW(env);
    if (!n)
        fprintf(out, "  (none)\n");
}

static void write_sidecar(const char *png_path, uint32_t width, uint32_t height,
                          const SYSTEMTIME *st)
{
    char side[SHOT_PATH_MAX];
    FILE *f;
    size_t len;
    const char *dot;

    len = strlen(png_path);
    if (len + 1 >= sizeof side)
        return;
    memcpy(side, png_path, len + 1);
    dot = strrchr(side, '.');
    if (!dot)
        return;
    memcpy((char *)dot, ".txt", 5);
    f = fopen(side, "wb");
    if (!f) {
        fprintf(stderr, "[SCREENSHOT] could not write sidecar %s\n", side);
        fflush(stderr);
        return;
    }
    ensure_exe_identity();
    fprintf(f, "Halo 2 debug sidecar (RECOMP_DEBUG_SCREENSHOT)\n");
    fprintf(f, "timestamp: %04u-%02u-%02u %02u:%02u:%02u.%03u (local)\n",
            st->wYear, st->wMonth, st->wDay,
            st->wHour, st->wMinute, st->wSecond, st->wMilliseconds);
    fprintf(f, "screenshot: %s\n", png_path);
    fprintf(f, "image: %ux%u\n", width, height);
    fprintf(f, "exe: %s\n", s_exe_path[0] ? s_exe_path : "(unknown)");
    fprintf(f, "exe_sha256: %s\n", s_exe_sha256[0] ? s_exe_sha256 : "(unavailable)");
    fprintf(f, "exe_size: %llu\n", (unsigned long long)s_exe_size);
    fprintf(f, "map: %s\n", s_last_map[0] ? s_last_map : "(no .map path seen yet)");
    fprintf(f, "last_guest_path: %s\n", s_last_guest_path[0] ? s_last_guest_path : "(none)");
    fprintf(f, "camera: not available (no safe TU 1.5 guest pointer; not probed)\n");
    halo2_debug_sidecar_gpu(f);
    halo2_debug_drawlist_write(png_path, f);
    write_recomp_env(f);
    write_stderr_tail(f);
    {
        char dir[MAX_PATH], alt[MAX_PATH];
        char *slash;
        if (GetModuleFileNameA(NULL, dir, MAX_PATH)) {
            slash = strrchr(dir, '\\');
            if (slash) {
                *slash = 0;
                snprintf(alt, sizeof alt, "%s\\stderr.log", dir);
                if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES)
                    write_stderr_file_tail(f, alt);
                snprintf(alt, sizeof alt, "%s\\..\\stderr.log", dir);
                if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES)
                    write_stderr_file_tail(f, alt);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[SCREENSHOT] sidecar %s\n", side);
    fflush(stderr);
}

int halo2_screenshot_try_copy(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                              ID3D11Texture2D *src, uint32_t width, uint32_t height,
                              DXGI_FORMAT format)
{
    if (s_pending)
        return 0;
    if (!screenshot_requested())
        return 0;
    if (!dev || !ctx || !src || !width || !height) {
        fprintf(stderr, "[SCREENSHOT] DEL pressed but no present surface was ready\n");
        fflush(stderr);
        return 0;
    }
    if (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B5G6R5_UNORM) {
        fprintf(stderr, "[SCREENSHOT] unsupported present format %u\n", (unsigned)format);
        fflush(stderr);
        return 0;
    }
    if (!screenshot_ensure_staging(dev, width, height, format))
        return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s_staging, (ID3D11Resource *)src);
    s_pending = 1;
    return 1;
}

void halo2_screenshot_finalize(ID3D11DeviceContext *ctx)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    char dir[MAX_PATH], shots[MAX_PATH], path[SHOT_PATH_MAX];
    wchar_t wpath[SHOT_PATH_MAX];
    uint8_t *bgra = NULL;
    UINT bgra_pitch;
    SYSTEMTIME st;
    int n, png_ok = 0;

    if (!s_pending)
        return;
    s_pending = 0;
    if (!ctx || !s_staging)
        return;

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        fprintf(stderr, "[SCREENSHOT] map failed 0x%08lX\n", (unsigned long)hr);
        fflush(stderr);
        return;
    }

    bgra_pitch = s_staging_w * 4u;
    bgra = (uint8_t *)malloc((size_t)bgra_pitch * s_staging_h);
    if (bgra) {
        if (s_staging_fmt == DXGI_FORMAT_B5G6R5_UNORM)
            expand_565_to_bgra((const uint8_t *)mapped.pData, mapped.RowPitch,
                               bgra, bgra_pitch, s_staging_w, s_staging_h);
        else
            copy_bgra_opaque((const uint8_t *)mapped.pData, mapped.RowPitch,
                             bgra, bgra_pitch, s_staging_w, s_staging_h);
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_staging, 0);
    if (!bgra) {
        fprintf(stderr, "[SCREENSHOT] out of memory preparing %ux%u image\n",
                s_staging_w, s_staging_h);
        fflush(stderr);
        return;
    }

    if (!screenshot_exe_dir(dir, sizeof dir)) {
        fprintf(stderr, "[SCREENSHOT] could not resolve the exe folder\n");
        fflush(stderr);
        free(bgra);
        return;
    }
    n = snprintf(shots, sizeof shots, "%s\\screenshots", dir);
    if (n < 0 || (size_t)n >= sizeof shots) {
        fprintf(stderr, "[SCREENSHOT] screenshots path too long\n");
        fflush(stderr);
        free(bgra);
        return;
    }
    if (!CreateDirectoryA(shots, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[SCREENSHOT] could not create %s (%lu)\n",
                shots, (unsigned long)GetLastError());
        fflush(stderr);
        free(bgra);
        return;
    }

    GetLocalTime(&st);
    if (!make_unique_path(path, sizeof path, shots, "png", &st)) {
        fprintf(stderr, "[SCREENSHOT] could not build a unique file name\n");
        fflush(stderr);
        free(bgra);
        return;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, SHOT_PATH_MAX) > 0)
        png_ok = write_png_bgra(wpath, bgra, s_staging_w, s_staging_h, bgra_pitch);
    if (!png_ok) {
        DeleteFileA(path);
        if (!make_unique_path(path, sizeof path, shots, "bmp", &st) ||
            !write_bmp_bgra(path, bgra, s_staging_w, s_staging_h, bgra_pitch)) {
            fprintf(stderr, "[SCREENSHOT] failed to write PNG or BMP in %s\n", shots);
            fflush(stderr);
            free(bgra);
            return;
        }
    }
    free(bgra);
    write_sidecar(path, s_staging_w, s_staging_h, &st);
    fprintf(stderr, "[SCREENSHOT] saved %s (%ux%u)\n", path, s_staging_w, s_staging_h);
    fflush(stderr);
}

#endif /* RECOMP_DEBUG_TOOLS */
