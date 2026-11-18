#ifndef XBOX_PRESENT_CAPTURE_H
#define XBOX_PRESENT_CAPTURE_H

/* Windows-only, presenter-thread diagnostic. No capture is evidence of
 * physical scanout or window visibility. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "present_provenance.h"

#define RECOMP_CAPTURE_PATH 2048

enum RecompCaptureOrigin {
    RECOMP_CAPTURE_MISSING,
    RECOMP_CAPTURE_FALLBACK,
    RECOMP_CAPTURE_PINNED,
    RECOMP_CAPTURE_FLIP
};
enum RecompCaptureRoute {
    RECOMP_CAPTURE_NO_ROUTE,
    RECOMP_CAPTURE_GDI,
    RECOMP_CAPTURE_D3D_CPU,
    RECOMP_CAPTURE_D3D_SHARED_REJECTED
};
enum RecompCaptureResult {
    RECOMP_CAPTURE_NO_INPUT,
    RECOMP_CAPTURE_GDI_NO_CLIENT,
    RECOMP_CAPTURE_GDI_OK,
    RECOMP_CAPTURE_GDI_FAILED,
    RECOMP_CAPTURE_D3D_NO_SURFACE,
    RECOMP_CAPTURE_D3D_RESIZE_FAILED,
    RECOMP_CAPTURE_D3D_TARGET_FAILED,
    RECOMP_CAPTURE_D3D_OK,
    RECOMP_CAPTURE_D3D_OCCLUDED,
    RECOMP_CAPTURE_D3D_FAILED,
    RECOMP_CAPTURE_D3D_OTHER_STATUS,
    RECOMP_CAPTURE_SHARED_REJECTED
};

typedef struct RecompCaptureRecord {
    uint64_t start_ns, end_ns;
    enum RecompCaptureOrigin origin;
    uint64_t publication_id;
    RecompPresentTag tag;
    uint32_t pitch, bpp, aa_sy, overlay, pinned;
    enum RecompCaptureRoute route;
    enum RecompCaptureResult result;
    uint32_t detail;
    uint32_t width, height;
    const uint32_t *image;
} RecompCaptureRecord;

typedef struct RecompPresenterCapture {
    HANDLE ledger;
    char directory[RECOMP_CAPTURE_PATH];
    uint64_t sequence, last_end_ns;
    int active;
} RecompPresenterCapture;

static int recomp_capture_clock(uint64_t *ns)
{
    LARGE_INTEGER ticks, frequency;
    uint64_t sec, rem, freq, whole, fraction;

    if (!ns || !QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&ticks) || frequency.QuadPart <= 0 ||
        ticks.QuadPart < 0)
        return 0;
    freq = (uint64_t)frequency.QuadPart;
    sec = (uint64_t)ticks.QuadPart / freq;
    rem = (uint64_t)ticks.QuadPart % freq;
    if (sec > UINT64_MAX / 1000000000ull ||
        rem > UINT64_MAX / 1000000000ull)
        return 0;
    whole = sec * 1000000000ull;
    fraction = rem * 1000000000ull / freq;
    if (fraction > UINT64_MAX - whole)
        return 0;
    *ns = whole + fraction;
    return 1;
}

static int recomp_capture_write(HANDLE file, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    while (bytes) {
        DWORD written = 0;
        DWORD chunk = bytes > 0x7fffffffu ? 0x7fffffffu : (DWORD)bytes;
        if (!WriteFile(file, p, chunk, &written, NULL) || !written)
            return 0;
        p += written;
        bytes -= written;
    }
    return 1;
}

static int recomp_capture_path(char *out, size_t cap, const char *dir,
                               const char *name)
{
    int n = snprintf(out, cap, "%s\\%s", dir, name);
    return n >= 0 && (size_t)n < cap;
}

static void recomp_capture_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint64_t recomp_capture_hash(const uint8_t *data, size_t bytes)
{
    uint64_t hash = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < bytes; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

/* BI_RGB top-down 32-bit BMP. All four bytes of the actual CPU input are
 * retained; both presenter routes use its RGB and ignore the fourth byte. */
static int recomp_capture_image(RecompPresenterCapture *capture, uint64_t seq,
                                const RecompCaptureRecord *record,
                                char *name, size_t name_cap, uint64_t *image_hash)
{
    char partial_name[96], partial[RECOMP_CAPTURE_PATH], final[RECOMP_CAPTURE_PATH];
    uint8_t header[54] = {0};
    uint64_t bytes;
    HANDLE file;
    int ok, n;

    if (!record->image || !image_hash || !record->width || !record->height ||
        record->width > INT32_MAX || record->height > INT32_MAX)
        return 0;
    bytes = (uint64_t)record->width * record->height * 4u;
    if (bytes > UINT32_MAX - sizeof header || bytes > SIZE_MAX)
        return 0;
    n = snprintf(name, name_cap, "present-%020llu.bmp", (unsigned long long)seq);
    if (n < 0 || (size_t)n >= name_cap)
        return 0;
    n = snprintf(partial_name, sizeof partial_name, "%s.partial", name);
    if (n < 0 || (size_t)n >= sizeof partial_name ||
        !recomp_capture_path(partial, sizeof partial, capture->directory, partial_name) ||
        !recomp_capture_path(final, sizeof final, capture->directory, name))
        return 0;
    file = CreateFileA(partial, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    header[0] = 'B'; header[1] = 'M';
    recomp_capture_le32(header + 2, (uint32_t)bytes + (uint32_t)sizeof header);
    recomp_capture_le32(header + 10, (uint32_t)sizeof header);
    recomp_capture_le32(header + 14, 40);
    recomp_capture_le32(header + 18, record->width);
    recomp_capture_le32(header + 22, 0u - record->height);
    header[26] = 1;
    header[28] = 32;
    recomp_capture_le32(header + 34, (uint32_t)bytes);
    ok = recomp_capture_write(file, header, sizeof header) &&
         recomp_capture_write(file, record->image, (size_t)bytes) &&
         FlushFileBuffers(file);
    if (!CloseHandle(file))
        ok = 0;
    if (ok)
        ok = MoveFileExA(partial, final, MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileA(partial);
    else
        *image_hash = recomp_capture_hash((const uint8_t *)record->image, (size_t)bytes);
    return ok;
}

static int recomp_capture_open(RecompPresenterCapture *capture, const char *directory)
{
    static const char header[] =
        "sequence,start_ns,end_ns,origin,publication_id,flip_candidate,"
        "stats,stats_volume,stats_file_index,pixels,format,pitch,aa_sy,"
        "overlay,pinned,route,status,detail,width,height,image,image_fnv64\n";
    char path[RECOMP_CAPTURE_PATH];
    DWORD length;

    if (!capture || !directory || !*directory)
        return 0;
    memset(capture, 0, sizeof *capture);
    capture->ledger = INVALID_HANDLE_VALUE;
    length = GetFullPathNameA(directory, sizeof capture->directory,
                              capture->directory, NULL);
    if (!length || length >= sizeof capture->directory)
        return 0;
    while (length > 3 && (capture->directory[length - 1] == '\\' ||
                          capture->directory[length - 1] == '/'))
        capture->directory[--length] = 0;
    if (!CreateDirectoryA(capture->directory, NULL) ||
        !recomp_capture_path(path, sizeof path, capture->directory, "submissions.csv"))
        return 0;
    capture->ledger = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (capture->ledger == INVALID_HANDLE_VALUE)
        return 0;
    if (!recomp_capture_write(capture->ledger, header, sizeof header - 1) ||
        !FlushFileBuffers(capture->ledger)) {
        CloseHandle(capture->ledger);
        capture->ledger = INVALID_HANDLE_VALUE;
        return 0;
    }
    capture->active = 1;
    return 1;
}

static int recomp_capture_record_valid(const RecompCaptureRecord *r)
{
    int has_input;
    if (!r || r->origin < RECOMP_CAPTURE_MISSING || r->origin > RECOMP_CAPTURE_FLIP ||
        r->route < RECOMP_CAPTURE_NO_ROUTE ||
        r->route > RECOMP_CAPTURE_D3D_SHARED_REJECTED ||
        r->result < RECOMP_CAPTURE_NO_INPUT ||
        r->result > RECOMP_CAPTURE_SHARED_REJECTED ||
        r->tag.stats < RECOMP_PRESENT_STATS_UNTRACKED ||
        r->tag.stats > RECOMP_PRESENT_STATS_EXHAUSTED ||
        r->tag.pixels < RECOMP_PRESENT_PIXELS_UNKNOWN ||
        r->tag.pixels > RECOMP_PRESENT_PIXELS_READBACK_UNAVAILABLE ||
        r->end_ns < r->start_ns || r->overlay > 1 || r->pinned > 1)
        return 0;
    if (r->origin == RECOMP_CAPTURE_PINNED && !r->pinned)
        return 0;
    if ((r->origin == RECOMP_CAPTURE_MISSING) != (r->route == RECOMP_CAPTURE_NO_ROUTE) ||
        (r->pinned && r->overlay))
        return 0;
    if ((r->origin == RECOMP_CAPTURE_FLIP) != (r->publication_id != 0))
        return 0;
    if ((r->tag.stats == RECOMP_PRESENT_STATS_READY && !r->tag.stats_file_index) ||
        (r->tag.stats != RECOMP_PRESENT_STATS_READY &&
         (r->tag.stats_volume || r->tag.stats_file_index)))
        return 0;
    if (r->tag.flip_candidate &&
        (r->origin != RECOMP_CAPTURE_FLIP || r->pinned ||
         r->tag.stats != RECOMP_PRESENT_STATS_READY ||
         !r->tag.stats_file_index ||
         (r->tag.pixels != RECOMP_PRESENT_PIXELS_CPU &&
          r->tag.pixels != RECOMP_PRESENT_PIXELS_READBACK)))
        return 0;
    has_input = r->result == RECOMP_CAPTURE_GDI_OK ||
                r->result == RECOMP_CAPTURE_GDI_FAILED ||
                r->result == RECOMP_CAPTURE_D3D_OK ||
                r->result == RECOMP_CAPTURE_D3D_OCCLUDED ||
                r->result == RECOMP_CAPTURE_D3D_FAILED ||
                r->result == RECOMP_CAPTURE_D3D_OTHER_STATUS;
    if (has_input != (r->image != NULL) ||
        (has_input && (!r->width || !r->height)))
        return 0;
    switch (r->route) {
    case RECOMP_CAPTURE_NO_ROUTE:
        return r->result == RECOMP_CAPTURE_NO_INPUT;
    case RECOMP_CAPTURE_GDI:
        return r->result >= RECOMP_CAPTURE_GDI_NO_CLIENT &&
               r->result <= RECOMP_CAPTURE_GDI_FAILED;
    case RECOMP_CAPTURE_D3D_CPU:
        return r->result >= RECOMP_CAPTURE_D3D_NO_SURFACE &&
               r->result <= RECOMP_CAPTURE_D3D_OTHER_STATUS;
    case RECOMP_CAPTURE_D3D_SHARED_REJECTED:
        return r->result == RECOMP_CAPTURE_SHARED_REJECTED;
    default:
        return 0;
    }
}

static int recomp_capture_record(RecompPresenterCapture *capture,
                                 const RecompCaptureRecord *record)
{
    static const char *const origins[] = {"missing", "fallback", "pinned", "flip"};
    static const char *const stats[] = {"untracked", "disabled", "ready", "no_identity", "exhausted"};
    static const char *const pixels[] = {"unknown", "cpu", "readback", "readback_failed",
                                         "readback_unavailable"};
    static const char *const routes[] = {"none", "gdi", "d3d_cpu", "d3d_shared_rejected"};
    static const char *const results[] = {
        "no_input", "gdi_no_client", "gdi_ok", "gdi_failed", "d3d_no_surface",
        "d3d_resize_failed", "d3d_target_failed", "d3d_ok", "d3d_occluded",
        "d3d_failed", "d3d_other_status", "shared_rejected"
    };
    const char *format;
    char image[96] = "", line[1024];
    uint64_t sequence, image_hash = 0;
    int n;

    if (!capture || !capture->active || !recomp_capture_record_valid(record) ||
        record->start_ns < capture->last_end_ns || capture->sequence == UINT64_MAX)
        return 0;
    sequence = capture->sequence + 1;
    if (record->image && !recomp_capture_image(capture, sequence, record, image, sizeof image,
                                                &image_hash))
        return 0;
    format = record->bpp == 2 ? "rgb565" : record->bpp == 4 ? "xrgb8888" :
             record->bpp == 8 ? "aa2x32" : record->bpp ? "unsupported_black" : "unknown";
    n = snprintf(line, sizeof line,
                 "%llu,%llu,%llu,%s,%llu,%u,%s,%u,%llu,%s,%s,%u,%u,%u,%u,%s,%s,"
                 "0x%08lX,%u,%u,%s,%016llx\n",
                 (unsigned long long)sequence,
                 (unsigned long long)record->start_ns,
                 (unsigned long long)record->end_ns,
                 origins[record->origin], (unsigned long long)record->publication_id,
                 record->tag.flip_candidate, stats[record->tag.stats],
                 record->tag.stats_volume, (unsigned long long)record->tag.stats_file_index,
                 pixels[record->tag.pixels], format, record->pitch, record->aa_sy,
                 record->overlay, record->pinned, routes[record->route], results[record->result],
                 (unsigned long)record->detail, record->width, record->height, image,
                 (unsigned long long)image_hash);
    if (n < 0 || (size_t)n >= sizeof line ||
        !recomp_capture_write(capture->ledger, line, (size_t)n) ||
        !FlushFileBuffers(capture->ledger))
        return 0;
    capture->sequence = sequence;
    capture->last_end_ns = record->end_ns;
    return 1;
}

static void recomp_capture_abandon(RecompPresenterCapture *capture)
{
    if (capture && capture->ledger != INVALID_HANDLE_VALUE) {
        CloseHandle(capture->ledger);
        capture->ledger = INVALID_HANDLE_VALUE;
        capture->active = 0;
    }
}

static int recomp_capture_close(RecompPresenterCapture *capture, const char *reason)
{
    char path[RECOMP_CAPTURE_PATH], contents[128];
    HANDLE marker;
    int n, ok;

    if (!capture || !capture->active || !reason ||
        (strcmp(reason, "window_closed") && strcmp(reason, "thread_stopped")))
        return 0;
    ok = FlushFileBuffers(capture->ledger) != 0;
    if (!CloseHandle(capture->ledger))
        ok = 0;
    capture->ledger = INVALID_HANDLE_VALUE;
    capture->active = 0;
    if (!ok || !recomp_capture_path(path, sizeof path, capture->directory, "complete.txt"))
        return 0;
    n = snprintf(contents, sizeof contents, "version=1\nlast_sequence=%llu\nreason=%s\n",
                 (unsigned long long)capture->sequence, reason);
    if (n < 0 || (size_t)n >= sizeof contents)
        return 0;
    marker = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (marker == INVALID_HANDLE_VALUE)
        return 0;
    ok = recomp_capture_write(marker, contents, (size_t)n) && FlushFileBuffers(marker);
    if (!CloseHandle(marker))
        ok = 0;
    if (!ok)
        DeleteFileA(path);
    return ok;
}

#endif
