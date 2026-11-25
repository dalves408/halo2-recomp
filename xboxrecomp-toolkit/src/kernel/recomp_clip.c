/* Test clips: RECOMP_CLIP=1 keeps the last seconds of presented frames (480x360)
 * with one line of data per frame. Pressing Delete writes the 5 seconds before
 * the press and 1 second after it to data\clips\clip-<date>-<time>.avi (plain
 * uncompressed video, plays anywhere) and a .txt next to it with the per-frame
 * lines. Meant for a tester to mark a problem as it happens; nothing runs
 * unless RECOMP_CLIP=1.
 *
 * The per-frame line comes from the title (g_rclip_note, e.g. what the
 * smoother blended that frame) and from here (flip number, time, the title's
 * update counter, the vblank lock). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#endif
#include "recomp_clip.h"
#include "../input/keyboard.h"
#include "platform/host_time.h"   /* xbox_HostNowNs */

#define CLIP_W        480
#define CLIP_H        360
#define CLIP_BEFORE   300         /* frames kept before the press (5 s at 60) */
#define CLIP_AFTER    60          /* frames after it (1 s) */
#define CLIP_RING     (CLIP_BEFORE + CLIP_AFTER)
#define NOTE_LEN      512

char g_rclip_note[512];
volatile int g_rclip_vblank_locked;

typedef struct {
    uint32_t flip, tick;
    uint64_t ns;
    int locked;
    char note[NOTE_LEN];
} ClipMeta;

static int s_on = -1;
static uint8_t *s_pix;            /* CLIP_RING frames, bottom-up BGR rows, CLIP_W * 3 bytes each */
static ClipMeta *s_meta;
static uint32_t s_count;          /* frames captured so far */
static int s_after = -1;          /* frames still to take after a press; -1: none pending */
static uint32_t s_press_index;    /* s_count at the press */
static int s_key_was;

static int clip_enabled(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_CLIP");
        s_on = e && e[0] == '1';
        if (s_on) {
            s_pix = (uint8_t *)malloc((size_t)CLIP_RING * CLIP_W * CLIP_H * 3);
            s_meta = (ClipMeta *)calloc(CLIP_RING, sizeof(ClipMeta));
            if (!s_pix || !s_meta) {
                free(s_pix); free(s_meta); s_pix = NULL; s_meta = NULL;
                s_on = 0;
                fprintf(stderr, "[CLIP] not enough memory for clips; off\n");
            } else
                fprintf(stderr, "[CLIP] on: Delete saves the 5 s before it and 1 s after to data\\clips\n");
        }
    }
    return s_on;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void fourcc(FILE *f, const char *c) { fwrite(c, 1, 4, f); }

/* An uncompressed 24-bit AVI of n ring frames starting at ring index first. */
static int write_avi(const char *path, uint32_t first, uint32_t n)
{
    const uint32_t frame_bytes = CLIP_W * CLIP_H * 3;
    const uint32_t movi_size = 4 + n * (8 + frame_bytes);
    const uint32_t hdrl_size = 4 + (8 + 56) + (8 + 4 + (8 + 56) + (8 + 40));
    const uint32_t idx_size = n * 16;
    const uint32_t riff_size = 4 + (8 + hdrl_size) + (8 + movi_size) + (8 + idx_size);
    uint32_t i, off;
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fourcc(f, "RIFF"); put32(f, riff_size); fourcc(f, "AVI ");
    fourcc(f, "LIST"); put32(f, hdrl_size); fourcc(f, "hdrl");
    fourcc(f, "avih"); put32(f, 56);
    put32(f, 16667); put32(f, frame_bytes * 60); put32(f, 0); put32(f, 0x10);  /* us/frame, max bytes/s, pad, AVIF_HASINDEX */
    put32(f, n); put32(f, 0); put32(f, 1); put32(f, frame_bytes);
    put32(f, CLIP_W); put32(f, CLIP_H); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 0);
    fourcc(f, "LIST"); put32(f, 4 + (8 + 56) + (8 + 40)); fourcc(f, "strl");
    fourcc(f, "strh"); put32(f, 56);
    fourcc(f, "vids"); fourcc(f, "DIB "); put32(f, 0); put16(f, 0); put16(f, 0); put32(f, 0);
    put32(f, 1); put32(f, 60); put32(f, 0); put32(f, n); put32(f, frame_bytes); put32(f, 0xFFFFFFFFu); put32(f, 0);
    put16(f, 0); put16(f, 0); put16(f, CLIP_W); put16(f, CLIP_H);
    fourcc(f, "strf"); put32(f, 40);
    put32(f, 40); put32(f, CLIP_W); put32(f, CLIP_H); put16(f, 1); put16(f, 24);
    put32(f, 0); put32(f, frame_bytes); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 0);
    fourcc(f, "LIST"); put32(f, movi_size); fourcc(f, "movi");
    for (i = 0; i < n; i++) {
        fourcc(f, "00db"); put32(f, frame_bytes);
        fwrite(s_pix + (size_t)((first + i) % CLIP_RING) * frame_bytes, 1, frame_bytes, f);
    }
    fourcc(f, "idx1"); put32(f, idx_size);
    for (i = 0, off = 4; i < n; i++, off += 8 + frame_bytes) {
        fourcc(f, "00db"); put32(f, 0x10); put32(f, off); put32(f, frame_bytes);
    }
    fclose(f);
    return 1;
}

static void write_clip(void)
{
    uint32_t n = s_count < CLIP_RING ? s_count : CLIP_RING;
    uint32_t first = s_count - n, i;
    char base[256], path[300];
    time_t now = time(NULL);
    struct tm lt;
    FILE *f;
#if defined(_WIN32)
    localtime_s(&lt, &now);
    _mkdir("data");
    _mkdir("data\\clips");
    snprintf(base, sizeof base, "data\\clips\\clip-%04d%02d%02d-%02d%02d%02d",
#else
    localtime_r(&now, &lt);
    snprintf(base, sizeof base, "data/clips/clip-%04d%02d%02d-%02d%02d%02d",
#endif
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    snprintf(path, sizeof path, "%s.avi", base);
    if (!write_avi(path, first, n)) {
        fprintf(stderr, "[CLIP] could not write %s\n", path);
        return;
    }
    snprintf(path, sizeof path, "%s.txt", base);
    f = fopen(path, "w");
    if (f) {
        const ClipMeta *m0 = &s_meta[first % CLIP_RING];
        fprintf(f, "Halo 2 test clip: %u frames, Delete pressed at frame %u (the 5 s before it and 1 s after).\n"
                   "frame  ms    flip    update  gap_ms  lock  | title's note\n", n, s_press_index - first);
        for (i = 0; i < n; i++) {
            const ClipMeta *m = &s_meta[(first + i) % CLIP_RING];
            const ClipMeta *p = i ? &s_meta[(first + i - 1) % CLIP_RING] : m;
            fprintf(f, "%5u %6.1f %7u %7u %6.1f   %s  | %s%s\n", i, (m->ns - m0->ns) / 1e6, m->flip, m->tick,
                    (m->ns - p->ns) / 1e6, m->locked ? "y" : "n",
                    m->note, first + i == s_press_index ? "   <-- Delete" : "");
        }
        fclose(f);
    }
    fprintf(stderr, "[CLIP] saved %s.avi and .txt (%u frames, Delete at frame %u)\n", base, n, s_press_index - first);
#if defined(_WIN32)
    MessageBeep(MB_OK);
#endif
}

void rclip_on_flip(const uint8_t *mem, uint32_t base, uint32_t pitch, uint32_t cx, uint32_t cy,
                   uint32_t w, uint32_t h, uint32_t bpp, uint32_t flip, uint32_t tick)
{
    uint32_t slot, y, x;
    uint8_t *dst;
    ClipMeta *m;
    int key;
    if (!clip_enabled() || !mem || !w || !h || (bpp != 4 && bpp != 2))
        return;
    slot = s_count % CLIP_RING;
    dst = s_pix + (size_t)slot * CLIP_W * CLIP_H * 3;
    for (y = 0; y < CLIP_H; y++) {               /* nearest sample, bottom-up rows for the AVI */
        const uint8_t *row = mem + base + (size_t)(cy + (CLIP_H - 1 - y) * h / CLIP_H) * pitch;
        uint8_t *o = dst + (size_t)y * CLIP_W * 3;
        for (x = 0; x < CLIP_W; x++) {
            uint32_t sx = cx + x * w / CLIP_W;
            if (bpp == 4) {
                const uint8_t *px = row + (size_t)sx * 4;
                o[0] = px[0]; o[1] = px[1]; o[2] = px[2];
            } else {                             /* R5G6B5 */
                uint16_t v = (uint16_t)(row[sx * 2] | row[sx * 2 + 1] << 8);
                o[0] = (uint8_t)((v & 31) << 3); o[1] = (uint8_t)(((v >> 5) & 63) << 2); o[2] = (uint8_t)((v >> 11) << 3);
            }
            o += 3;
        }
    }
    m = &s_meta[slot];
    m->flip = flip;
    m->tick = tick;
    m->ns = xbox_HostNowNs();
    m->locked = g_rclip_vblank_locked;
    snprintf(m->note, NOTE_LEN, "%s", g_rclip_note);
    s_count++;

    key = xbox_FramebufferKeyDown(XBOX_VK_DELETE);
    {   /* RECOMP_CLIP_TRIGGER=<file>: the file appearing counts as a Delete press (for scripted tests) */
        static const char *trig = (const char *)-1;
        if (trig == (const char *)-1)
            trig = getenv("RECOMP_CLIP_TRIGGER");
#if defined(_WIN32)
        if (trig && (s_count & 15) == 0 && GetFileAttributesA(trig) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(trig);
            key = 1;
            s_key_was = 0;
        }
#endif
    }
    if (key && !s_key_was && s_after < 0) {
        s_after = CLIP_AFTER;
        s_press_index = s_count - 1;
        fprintf(stderr, "[CLIP] Delete at flip %u: saving after 1 more second\n", flip);
    }
    s_key_was = key;
    if (s_after >= 0 && s_after-- == 0) {
        write_clip();
        s_after = -1;
    }
}
