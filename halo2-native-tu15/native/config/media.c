/* media.c: reads the Map Pack disc's bonus videos (see media.h).
 *
 * Sofdec (.sfd): an MPEG-1 system stream. Video (stream 0xE0) is MPEG-1, decoded by pl_mpeg (MIT, pl_mpeg.h).
 * Audio is CRI ADX (stream 0xC0: one ADX stream, stereo) or CRI AIX (0xC1: three stereo ADX streams interleaved
 * in AIXP chunks, together 5.1). ADX is 4-bit ADPCM in 18-byte frames per channel (a 16-bit scale, then 32
 * nibbles) predicted from the two previous samples with coefficients made from the header's high-pass frequency.
 *
 * MPEG-4 (.mp4, from media_encode_mp4): read with Windows' Media Foundation source reader. */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"

#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include "pl_mpeg.h"

/* ---- a byte queue ------------------------------------------------------------------------------------------------ */
typedef struct { uint8_t *b; size_t head, len, cap; } Fifo;

static void fifo_push(Fifo *f, const uint8_t *p, size_t n)
{
    if (f->head && f->head + f->len + n > f->cap) {
        memmove(f->b, f->b + f->head, f->len);
        f->head = 0;
    }
    if (f->len + n > f->cap) {
        size_t c = f->cap ? f->cap : 65536;
        while (c < f->len + n) c *= 2;
        f->b = (uint8_t *)realloc(f->b, c);
        f->cap = c;
    }
    memcpy(f->b + f->head + f->len, p, n);
    f->len += n;
}
static const uint8_t *fifo_peek(const Fifo *f) { return f->b + f->head; }
static void fifo_pop(Fifo *f, size_t n) { f->head += n; f->len -= n; if (!f->len) f->head = 0; }
static void fifo_free(Fifo *f) { free(f->b); memset(f, 0, sizeof *f); }

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned be32(const uint8_t *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

/* ---- CRI ADX ----------------------------------------------------------------------------------------------------- */
typedef struct {
    Fifo in;
    int have_hdr, ended, ch, rate, c1, c2;
    int hist[8][2];
} Adx;

/* Reads the header once it is all there. 1 ready, 0 needs more, -1 not ADX. */
static int adx_header(Adx *a)
{
    const uint8_t *b = fifo_peek(&a->in);
    unsigned off;
    double z, x, y, c;
    if (a->have_hdr) return 1;
    if (a->in.len < 4) return 0;
    if (b[0] != 0x80 || b[1] != 0x00) return -1;
    off = be16(b + 2) + 4;
    if (a->in.len < off || off < 20) return 0;
    if (b[4] != 3 || b[5] != 18 || b[6] != 4 || !b[7] || b[7] > 8) return -1;
    a->ch = b[7];
    a->rate = (int)be32(b + 8);
    x = be16(b + 16);
    y = a->rate;
    z = cos(2.0 * 3.14159265358979323846 * x / y);
    x = 1.41421356237309504880 - z;            /* a */
    y = 1.41421356237309504880 - 1.0;          /* b */
    c = (x - sqrt((x + y) * (x - y))) / y;
    /* rounded, as CRI's decoder and FFmpeg make them (checked: FFmpeg's output exactly) */
    a->c1 = (int)lrint(c * 8192.0);
    a->c2 = (int)lrint(c * c * -4096.0);
    memset(a->hist, 0, sizeof a->hist);
    fifo_pop(&a->in, off);
    a->have_hdr = 1;
    return 1;
}

/* Decodes one frame (32 samples for each channel) into out (interleaved with `stride` channels, starting at channel
 * `first`). 1 done, 0 needs more, -1 the end. */
static int adx_frame(Adx *a, int16_t *out, int stride, int first)
{
    const uint8_t *b;
    int c, i;
    if (a->ended) return -1;
    if (adx_header(a) <= 0) return 0;
    if (a->in.len < (size_t)(18 * a->ch)) return 0;
    b = fifo_peek(&a->in);
    if (be16(b) & 0x8000) { a->ended = 1; return -1; }   /* 0x8001: the end-of-stream frame */
    for (c = 0; c < a->ch; c++) {
        const uint8_t *f = b + 18 * c;
        int scale = (int)be16(f), h1 = a->hist[c][0], h2 = a->hist[c][1];
        for (i = 0; i < 32; i++) {
            int n = f[2 + i / 2], s;
            n = (i & 1) ? (n & 15) : (n >> 4);
            if (n & 8) n -= 16;
            s = (n * scale * 4096 + a->c1 * h1 + a->c2 * h2) >> 12;   /* one shift for the sum */
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            out[i * stride + first + c] = (int16_t)s;
            h2 = h1; h1 = s;
        }
        a->hist[c][0] = h1; a->hist[c][1] = h2;
    }
    fifo_pop(&a->in, 18 * a->ch);
    return 1;
}

/* ---- the media --------------------------------------------------------------------------------------------------- */
struct Media {
    int mp4;
    /* Sofdec */
    WCHAR **paths;
    int nparts, part;
    HANDLE f;
    uint8_t *rb;
    size_t rb_pos, rb_len;
    unsigned long long done, total;
    int eof;
    plm_buffer_t *vbuf;
    plm_video_t *video;
    plm_frame_t *frame;
    int frames;
    int tmpgexs;            /* the video's user data says TMPGEXS: 11-bit intra DC (pl_mpeg.h) */
    int audio_id;           /* 0xC0 or 0xC1 once known */
    int aix;                /* AIX: three ADX streams */
    Fifo araw;              /* the audio stream's bytes (ADX, or AIX chunks) */
    Adx adx[3];
    int aix_hdr_done, audio_end;
    int16_t pcm[32 * 6];
    int pcm_pos, pcm_n;
    /* MPEG-4 */
    IMFSourceReader *rd;
    int w, h, stride, ch, rate;
    double fps, t;
    uint8_t *bgrx;
    int16_t *apcm;
    int apcm_pos, apcm_n, a_end;
    LONGLONG dur;
};

/* ---- Sofdec: reading the system stream ---------------------------------------------------------------------------- */
static int sfd_open_part(Media *m)
{
    if (m->f != INVALID_HANDLE_VALUE && m->f) CloseHandle(m->f);
    m->f = CreateFileW(m->paths[m->part], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    m->rb_pos = m->rb_len = 0;
    return m->f != INVALID_HANDLE_VALUE;
}

/* Makes at least n bytes readable at rb_pos (fewer at the end of the part). */
static size_t sfd_fill(Media *m, size_t n)
{
    DWORD got;
    if (m->rb_len - m->rb_pos >= n) return n;
    memmove(m->rb, m->rb + m->rb_pos, m->rb_len - m->rb_pos);
    m->rb_len -= m->rb_pos;
    m->rb_pos = 0;
    while (m->rb_len < n && ReadFile(m->f, m->rb + m->rb_len, (DWORD)((1 << 20) - m->rb_len), &got, NULL) && got) {
        m->rb_len += got;
        m->done += got;
    }
    return m->rb_len < n ? m->rb_len : n;
}

/* A new part's audio starts with its own ADX or AIX header. */
static void audio_reset(Media *m)
{
    int i;
    fifo_free(&m->araw);
    for (i = 0; i < 3; i++) { fifo_free(&m->adx[i].in); memset(&m->adx[i], 0, sizeof m->adx[i]); }
    m->aix_hdr_done = 0;
}

/* Reads one packet. 1 one read, 0 the end of the last part. */
static int sfd_packet(Media *m)
{
    for (;;) {
        const uint8_t *p;
        unsigned id, len;
        if (m->eof) return 0;
        if (sfd_fill(m, 6) < 4) {
            if (m->part + 1 < m->nparts) {
                m->part++;
                if (!sfd_open_part(m)) { m->eof = 1; return 0; }
                /* the audio of the part before is played out first: its PCM stays queued */
                audio_reset(m);
                continue;
            }
            m->eof = 1;
            plm_buffer_signal_end(m->vbuf);
            return 0;
        }
        p = m->rb + m->rb_pos;
        if (p[0] || p[1] || p[2] != 1) { m->rb_pos++; continue; }
        id = p[3];
        if (id == 0xB9) { m->rb_pos += 4; continue; }             /* end code */
        if (id == 0xBA) {                                         /* pack header */
            if (sfd_fill(m, 14) < 12) { m->rb_pos = m->rb_len; continue; }
            p = m->rb + m->rb_pos;
            m->rb_pos += (p[4] >> 6) == 1 ? 14 + (p[13] & 7) : 12;
            continue;
        }
        if (id < 0xBB) { m->rb_pos++; continue; }
        if (sfd_fill(m, 6) < 6) { m->rb_pos = m->rb_len; continue; }
        len = be16(m->rb + m->rb_pos + 4);
        if (sfd_fill(m, 6 + len) < 6 + len) { m->rb_pos = m->rb_len; continue; }
        p = m->rb + m->rb_pos + 6;
        if (id == 0xE0 || id == 0xC0 || id == 0xC1) {
            unsigned j = 0;
            while (j < len && p[j] == 0xFF) j++;                  /* stuffing */
            if (j < len && (p[j] >> 6) == 1) j += 2;              /* STD buffer */
            if (j < len) {
                if ((p[j] >> 4) == 2) j += 5;                     /* PTS */
                else if ((p[j] >> 4) == 3) j += 10;               /* PTS and DTS */
                else j += 1;
            }
            if (j < len) {
                if (id == 0xE0) {
                    /* CRI Sofdec video (made with TMPGEnc) marks 11-bit intra DC in user data after the sequence
                     * header: "\0TMPGEXS\0", as FFmpeg detects it */
                    if (!m->tmpgexs && m->part == 0 && m->done <= (8u << 20)) {
                        unsigned k;
                        for (k = j; k + 9 <= len; k++)
                            if (!memcmp(p + k, "\0TMPGEXS\0", 9)) { m->tmpgexs = 1; break; }
                    }
                    plm_buffer_write(m->vbuf, (uint8_t *)p + j, len - j);
                }
                else {
                    if (!m->audio_id) m->audio_id = (int)id;
                    if ((int)id == m->audio_id) fifo_push(&m->araw, p + j, len - j);
                }
            }
        }
        m->rb_pos += 6 + len;
        return 1;
    }
}

/* pl_mpeg asks for more video: read packets until some video came. */
static void vbuf_load(plm_buffer_t *b, void *user)
{
    Media *m = (Media *)user;
    size_t before = plm_buffer_get_remaining(b);
    while (plm_buffer_get_remaining(b) == before && sfd_packet(m)) {}
}

/* AIX: hands each AIXP chunk's payload to its stream's ADX decoder. */
static void aix_route(Media *m)
{
    for (;;) {
        const uint8_t *b = fifo_peek(&m->araw);
        unsigned sz;
        if (m->araw.len < 8) return;
        sz = be32(b + 4);
        if (m->araw.len < 8 + (size_t)sz) return;
        if (!memcmp(b, "AIXP", 4) && sz >= 8 && b[8] < 3)
            fifo_push(&m->adx[b[8]].in, b + 16, sz - 8);
        fifo_pop(&m->araw, 8 + sz);    /* AIXF (the header), AIXE (the end) and anything else are skipped */
    }
}

/* Decodes the next 32 frames of audio into m->pcm. 1 ok, 0 needs more input, -1 the end. */
static int audio_decode(Media *m)
{
    if (!m->araw.len && !m->adx[0].in.len) return 0;
    if (!m->aix_hdr_done && m->araw.len >= 4) {
        m->aix = !memcmp(fifo_peek(&m->araw), "AIXF", 4);
        m->aix_hdr_done = 1;
    }
    if (!m->aix) {
        int r;
        if (m->araw.len) { fifo_push(&m->adx[0].in, fifo_peek(&m->araw), m->araw.len); fifo_pop(&m->araw, m->araw.len); }
        if (adx_header(&m->adx[0]) <= 0) return 0;
        if (m->adx[0].ch != 2) return -1;
        r = adx_frame(&m->adx[0], m->pcm, 2, 0);
        if (r > 0) { m->pcm_pos = 0; m->pcm_n = 32; }
        return r;
    } else {
        /* the three stereo streams make 5.1 (FL FR FC LFE BL BR): stream 0 front left and right, stream 1 back
         * left and right, stream 2 centre and LFE (CRI's order, as the disc's audio test speaks them) */
        static const int first[3] = { 0, 4, 2 };
        int s;
        aix_route(m);
        for (s = 0; s < 3; s++) {
            if (adx_header(&m->adx[s]) <= 0) return 0;
            if (m->adx[s].ch != 2) return -1;
            if (!m->adx[s].ended && m->adx[s].in.len < 36) return 0;
        }
        for (s = 0; s < 3; s++) {
            int r = adx_frame(&m->adx[s], m->pcm, 6, first[s]);
            if (r < 0) return -1;
        }
        m->pcm_pos = 0;
        m->pcm_n = 32;
        return 1;
    }
}

/* ---- MPEG-4 through Media Foundation ------------------------------------------------------------------------------- */
static int mf_started;
static void mf_up(void)
{
    if (!mf_started) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        mf_started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
}

static int mp4_open(Media *m, const WCHAR *path)
{
    IMFAttributes *at = NULL;
    IMFMediaType *t = NULL, *cur = NULL;
    UINT32 num = 0, den = 0;
    UINT64 v;
    PROPVARIANT pv;
    mf_up();
    if (FAILED(MFCreateAttributes(&at, 2))) return 0;
    IMFAttributes_SetUINT32(at, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(MFCreateSourceReaderFromURL(path, at, &m->rd))) { IMFAttributes_Release(at); return 0; }
    IMFAttributes_Release(at);
    /* video as RGB32, audio as 16-bit PCM */
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, t))) {
        IMFMediaType_Release(t);
        return 0;
    }
    IMFMediaType_Release(t);
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFSourceReader_SetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, t);
    IMFMediaType_Release(t);
    if (FAILED(IMFSourceReader_GetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) return 0;
    IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &v);
    m->w = (int)(v >> 32);
    m->h = (int)(v & 0xFFFFFFFF);
    if (SUCCEEDED(IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_RATE, &v))) { num = (UINT32)(v >> 32); den = (UINT32)v; }
    m->fps = den ? (double)num / den : 29.97;
    {
        UINT32 st = 0;
        m->stride = SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &st)) ? (int)(INT32)st : m->w * 4;
    }
    IMFMediaType_Release(cur);
    m->ch = 2; m->rate = 48000;
    if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) {
        UINT32 x;
        if (SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &x))) m->ch = (int)x;
        if (SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &x))) m->rate = (int)x;
        IMFMediaType_Release(cur);
    } else m->a_end = 1;
    PropVariantInit(&pv);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(m->rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                                           &MF_PD_DURATION, &pv)))
        m->dur = (LONGLONG)pv.uhVal.QuadPart;
    PropVariantClear(&pv);
    m->bgrx = (uint8_t *)calloc((size_t)m->w * m->h, 4);
    return m->w > 0 && m->h > 0 && m->bgrx;
}

static int mp4_frame(Media *m, double *t)
{
    for (;;) {
        DWORD idx, flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = NULL;
        IMFMediaBuffer *b = NULL;
        BYTE *p;
        DWORD len;
        if (FAILED(IMFSourceReader_ReadSample(m->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s)))
            return 0;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (s) IMFSample_Release(s); return 0; }
        if (!s) continue;
        /* The decoder's rows are padded (a width that is not a multiple of 16, like a cropped 572: rows 576 pixels
         * apart): the 2D buffer's own pitch says where each row starts; the media type's default stride does not. */
        if (SUCCEEDED(IMFSample_GetBufferByIndex(s, 0, &b))) {
            IMF2DBuffer *b2 = NULL;
            BYTE *scan0;
            LONG pitch;
            if (SUCCEEDED(IMFMediaBuffer_QueryInterface(b, &IID_IMF2DBuffer, (void **)&b2)) &&
                SUCCEEDED(IMF2DBuffer_Lock2D(b2, &scan0, &pitch))) {
                int y;
                for (y = 0; y < m->h; y++)
                    memcpy(m->bgrx + (size_t)y * m->w * 4, scan0 + (ptrdiff_t)y * pitch, (size_t)m->w * 4);
                IMF2DBuffer_Unlock2D(b2);
            } else if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
                int y, st = m->stride < 0 ? -m->stride : m->stride;
                if (len >= (DWORD)m->h * (DWORD)m->w * 4 && len % (DWORD)m->h == 0 && (int)(len / m->h) > st)
                    st = (int)(len / m->h);   /* no 2D buffer: the rows are as far apart as the buffer says */
                for (y = 0; y < m->h; y++) {
                    const BYTE *row = m->stride < 0 ? p + (size_t)(m->h - 1 - y) * st : p + (size_t)y * st;
                    if ((size_t)(y + 1) * st <= len) memcpy(m->bgrx + (size_t)y * m->w * 4, row, (size_t)m->w * 4);
                }
                IMFMediaBuffer_Unlock(b);
            }
            if (b2) IMF2DBuffer_Release(b2);
            IMFMediaBuffer_Release(b);
        }
        IMFSample_Release(s);
        m->t = ts / 1e7;
        *t = m->t;
        return 1;
    }
}

static int mp4_audio(Media *m, int16_t *out, int frames)
{
    int got = 0;
    while (got < frames) {
        if (m->apcm_pos < m->apcm_n) {
            int n = m->apcm_n - m->apcm_pos;
            if (n > frames - got) n = frames - got;
            memcpy(out + (size_t)got * m->ch, m->apcm + (size_t)m->apcm_pos * m->ch, (size_t)n * m->ch * 2);
            m->apcm_pos += n;
            got += n;
            continue;
        }
        if (m->a_end) break;
        {
            DWORD idx, flags = 0, len;
            LONGLONG ts;
            IMFSample *s = NULL;
            IMFMediaBuffer *b = NULL;
            BYTE *p;
            if (FAILED(IMFSourceReader_ReadSample(m->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &idx, &flags, &ts, &s))
                || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                if (s) IMFSample_Release(s);
                m->a_end = 1;
                break;
            }
            if (!s) continue;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &b))) {
                if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
                    m->apcm = (int16_t *)realloc(m->apcm, len);
                    memcpy(m->apcm, p, len);
                    m->apcm_n = (int)(len / (2 * m->ch));
                    m->apcm_pos = 0;
                    IMFMediaBuffer_Unlock(b);
                }
                IMFMediaBuffer_Release(b);
            }
            IMFSample_Release(s);
        }
    }
    return got;
}

/* ---- the interface ----------------------------------------------------------------------------------------------- */
Media *media_open(const WCHAR *const *paths, int n)
{
    Media *m;
    int i;
    const WCHAR *dot;
    if (n < 1) return NULL;
    m = (Media *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->f = INVALID_HANDLE_VALUE;
    dot = wcsrchr(paths[0], L'.');
    if (dot && !_wcsicmp(dot, L".mp4")) {
        m->mp4 = 1;
        if (!mp4_open(m, paths[0])) { media_close(m); return NULL; }
        return m;
    }
    m->paths = (WCHAR **)calloc((size_t)n, sizeof *m->paths);
    m->nparts = n;
    for (i = 0; i < n; i++) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        m->paths[i] = _wcsdup(paths[i]);
        if (GetFileAttributesExW(paths[i], GetFileExInfoStandard, &fa))
            m->total += ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    m->rb = (uint8_t *)malloc(1 << 20);
    if (!m->rb || !sfd_open_part(m)) { media_close(m); return NULL; }
    m->vbuf = plm_buffer_create_with_capacity(1 << 20);
    plm_buffer_set_load_callback(m->vbuf, vbuf_load, m);
    m->video = plm_video_create_with_buffer(m->vbuf, 0);
    if (!m->video || !plm_video_has_header(m->video)) { media_close(m); return NULL; }
    plm_video_set_dc_precision_11(m->video, m->tmpgexs);
    return m;
}

void media_close(Media *m)
{
    int i;
    if (!m) return;
    if (m->video) plm_video_destroy(m->video);
    if (m->vbuf) plm_buffer_destroy(m->vbuf);
    if (m->f && m->f != INVALID_HANDLE_VALUE) CloseHandle(m->f);
    for (i = 0; i < m->nparts; i++) free(m->paths[i]);
    free(m->paths);
    free(m->rb);
    fifo_free(&m->araw);
    for (i = 0; i < 3; i++) fifo_free(&m->adx[i].in);
    if (m->rd) IMFSourceReader_Release(m->rd);
    free(m->bgrx);
    free(m->apcm);
    free(m);
}

int media_width(const Media *m) { return m->mp4 ? m->w : plm_video_get_width(m->video); }
int media_height(const Media *m) { return m->mp4 ? m->h : plm_video_get_height(m->video); }
double media_fps(const Media *m)
{
    double f = m->mp4 ? m->fps : plm_video_get_framerate(m->video);
    return f > 1 ? f : 29.97;
}

int media_channels(const Media *m)
{
    if (m->mp4) return m->ch;
    return m->audio_id == 0xC1 ? 6 : 2;
}
int media_rate(const Media *m) { return m->mp4 ? m->rate : (m->adx[0].rate ? m->adx[0].rate : 48000); }

double media_progress(const Media *m)
{
    if (m->mp4) return m->dur > 0 ? m->t * 1e7 / (double)m->dur : 0;
    return m->total ? (double)m->done / (double)m->total : 0;
}

int media_next_frame(Media *m, double *t)
{
    if (m->mp4) return mp4_frame(m, t);
    plm_video_set_dc_precision_11(m->video, m->tmpgexs);
    m->frame = plm_video_decode(m->video);
    if (!m->frame) return 0;
    *t = m->frames++ / media_fps(m);
    return 1;
}

void media_frame_bgrx(Media *m, uint8_t *dst, int stride)
{
    int y;
    if (m->mp4) {
        for (y = 0; y < m->h; y++) memcpy(dst + (size_t)y * stride, m->bgrx + (size_t)y * m->w * 4, (size_t)m->w * 4);
        return;
    }
    if (m->frame) plm_frame_to_bgra(m->frame, dst, stride);
}

int media_frame_nv12(Media *m, uint8_t *dst)
{
    plm_frame_t *f = m->frame;
    int w, h, y, x;
    uint8_t *uv;
    if (m->mp4 || !f) return 0;
    w = (int)f->width;
    h = (int)f->height;
    for (y = 0; y < h; y++) memcpy(dst + (size_t)y * w, f->y.data + (size_t)y * f->y.width, (size_t)w);
    uv = dst + (size_t)w * h;
    for (y = 0; y < h / 2; y++)
        for (x = 0; x < w / 2; x++) {
            uv[(size_t)y * w + 2 * x] = f->cb.data[(size_t)y * f->cb.width + x];
            uv[(size_t)y * w + 2 * x + 1] = f->cr.data[(size_t)y * f->cr.width + x];
        }
    return 1;
}

int media_audio(Media *m, int16_t *out, int frames)
{
    int got = 0, ch;
    if (m->mp4) return mp4_audio(m, out, frames);
    while (got < frames) {
        int r;
        if (m->pcm_pos < m->pcm_n) {
            int n = m->pcm_n - m->pcm_pos;
            ch = media_channels(m);
            if (n > frames - got) n = frames - got;
            memcpy(out + (size_t)got * ch, m->pcm + (size_t)m->pcm_pos * ch, (size_t)n * ch * 2);
            m->pcm_pos += n;
            got += n;
            continue;
        }
        if (m->audio_end) break;
        r = audio_decode(m);
        if (r > 0) continue;
        if (r < 0) {
            /* this part's audio ended: the next part (if any) brings its own */
            if (m->eof) { m->audio_end = 1; break; }
            if (!sfd_packet(m)) { m->audio_end = 1; break; }
            continue;
        }
        if (!sfd_packet(m)) {
            if (audio_decode(m) > 0) continue;
            m->audio_end = 1;
            break;
        }
    }
    return got;
}

/* ---- re-encoding to H.264 + AAC ---------------------------------------------------------------------------------- */
static void seterr(char *err, size_t n, const char *what, HRESULT hr)
{
    if (err && n) snprintf(err, n, "%s (0x%08lX)", what, (unsigned long)hr);
}

/* The picture's area: the disc's videos are 640x480 with black bars where the film is wider than 4:3 (or the picture
 * smaller than the frame). A first pass decodes the whole video and marks every row and column that has picture in
 * any frame looked at (every 3rd): a row or column counts when more than 2% of it is brighter than black (luma above
 * 50, black being 16). The crop keeps all of those, so no frame loses picture; it is used only when it takes 8 pixels
 * or more off a side, and its edges are even (NV12). The measured bars (2026-11-27): Beach rows 66-413, Killtacular
 * 25-455, the trailer 48-431, the audio test 87-479, E3 2000 none. */
static int find_picture(const WCHAR *const *src, int n, int *x0, int *y0, int *cw, int *ch_, void (*progress)(double, void *),
                        void *user, volatile LONG *cancel)
{
    Media *m = media_open(src, n);
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * 6 * 4800);
    unsigned char *row = NULL, *col = NULL;
    int w, h, frames = 0, top, bot, lft, rgt, ok = 0;
    long long apos = 0;
    double t;
    if (!m || !pcm || m->mp4 || !media_next_frame(m, &t)) goto out;
    w = media_width(m); h = media_height(m);
    row = (unsigned char *)calloc((size_t)h, 1);
    col = (unsigned char *)calloc((size_t)w, 1);
    if (!row || !col) goto out;
    do {
        if (cancel && *cancel) goto out;
        if (frames % 3 == 0 && m->frame) {
            const plm_plane_t *yp = &m->frame->y;
            int x, y, need_r = (w - 16) / 50, need_c = (h - 16) / 50;
            int *cc = (int *)calloc((size_t)w, sizeof(int));
            if (!cc) goto out;
            for (y = 8; y < h - 8; y++) {
                const uint8_t *py = yp->data + (size_t)y * yp->width;
                int c = 0;
                for (x = 8; x < w - 8; x++)
                    if (py[x] > 50) { c++; cc[x]++; }
                if (c > need_r) row[y] = 1;
            }
            for (x = 8; x < w - 8; x++) if (cc[x] > need_c) col[x] = 1;
            free(cc);
        }
        frames++;
        /* the sound is read alongside, or its bytes would pile up */
        while (apos * media_fps(m) < (long long)frames * media_rate(m)) {
            int got = media_audio(m, pcm, 1600);
            if (got <= 0) break;
            apos += got;
        }
        if (progress && frames % 60 == 0) progress(media_progress(m), user);
    } while (media_next_frame(m, &t));
    for (top = 8; top < h - 8 && !row[top]; top++) {}
    for (bot = h - 9; bot > top && !row[bot]; bot--) {}
    for (lft = 8; lft < w - 8 && !col[lft]; lft++) {}
    for (rgt = w - 9; rgt > lft && !col[rgt]; rgt--) {}
    if (top >= bot || lft >= rgt) goto out;   /* all black: keep the frame */
    /* a side with less than 8 pixels of bar, or only the 8-pixel margin looked past, keeps its edge */
    if (top <= 8) top = 0;
    if (bot >= h - 9) bot = h - 1;
    if (lft <= 8) lft = 0;
    if (rgt >= w - 9) rgt = w - 1;
    top &= ~1; lft &= ~1;
    bot |= 1; rgt |= 1;
    if (bot >= h) bot = h - 1;
    if (rgt >= w) rgt = w - 1;
    *x0 = lft; *y0 = top; *cw = rgt - lft + 1; *ch_ = bot - top + 1;
    ok = 1;
out:
    if (m) media_close(m);
    free(pcm); free(row); free(col);
    return ok;
}

/* The picture cropped to x0,y0 cw x ch as NV12 (Y rows of cw, then Cb Cr pairs). */
static void frame_nv12_crop(Media *m, uint8_t *dst, int x0, int y0, int cw, int ch)
{
    plm_frame_t *f = m->frame;
    int y, x;
    uint8_t *uv = dst + (size_t)cw * ch;
    if (!f) return;
    for (y = 0; y < ch; y++) memcpy(dst + (size_t)y * cw, f->y.data + (size_t)(y0 + y) * f->y.width + x0, (size_t)cw);
    for (y = 0; y < ch / 2; y++)
        for (x = 0; x < cw / 2; x++) {
            uv[(size_t)y * cw + 2 * x] = f->cb.data[(size_t)(y0 / 2 + y) * f->cb.width + x0 / 2 + x];
            uv[(size_t)y * cw + 2 * x + 1] = f->cr.data[(size_t)(y0 / 2 + y) * f->cr.width + x0 / 2 + x];
        }
}

struct crop_progress { void (*fn)(double, void *); void *user; double base, span; };
static void crop_progress_fn(double p, void *u)
{
    struct crop_progress *c = (struct crop_progress *)u;
    if (c->fn) c->fn(c->base + p * c->span, c->user);
}

int media_encode_mp4(const WCHAR *const *src, int n, const WCHAR *dst, void (*progress)(double, void *), void *user,
                     volatile LONG *cancel, char *err, size_t errn)
{
    Media *m = media_open(src, n);
    IMFSinkWriter *wr = NULL;
    IMFAttributes *at = NULL;
    IMFMediaType *t = NULL;
    DWORD vs = 0, as = 0;
    HRESULT hr;
    WCHAR part[MAX_PATH];
    int w, h, ch, rate, ok = 0, frames = 0, have_audio = 1;
    LONGLONG apos = 0, fdur;
    double fps, vt;
    uint8_t *nv12 = NULL;
    int16_t *pcm = NULL;
    int cx = 0, cy = 0, cw = 0, chh = 0;
    struct crop_progress cp;
    cp.fn = progress; cp.user = user;
    if (m && !m->mp4) {   /* the first pass (about a fifth of the time): where the picture is */
        cp.base = 0; cp.span = 0.2;
        if (!find_picture(src, n, &cx, &cy, &cw, &chh, progress ? crop_progress_fn : NULL, &cp, cancel)) cw = 0;
        if (cancel && *cancel) { media_close(m); return -1; }
    }
    cp.base = 0.2; cp.span = 0.8;
    if (!m) { seterr(err, errn, "The video could not be read", E_FAIL); return 0; }
    if (m->mp4) { media_close(m); seterr(err, errn, "Already re-encoded", E_FAIL); return 0; }
    mf_up();
    /* the first picture and some audio first: the size, rate and channel count are known after them */
    if (!media_next_frame(m, &vt)) { media_close(m); seterr(err, errn, "The video has no pictures", E_FAIL); return 0; }
    w = media_width(m); h = media_height(m); fps = media_fps(m);
    if (cw <= 0 || chh <= 0) { cx = 0; cy = 0; cw = w; chh = h; }
    w = cw; h = chh;   /* the size written: the picture's area */
    pcm = (int16_t *)malloc(sizeof(int16_t) * 6 * 4800);
    nv12 = (uint8_t *)malloc((size_t)w * h * 3 / 2);
    {
        /* the audio's format: decode a frame now (it stays queued) */
        int16_t probe[6];
        (void)probe;
        if (audio_decode(m) == 0) while (sfd_packet(m) && audio_decode(m) == 0) {}
    }
    ch = media_channels(m);
    rate = media_rate(m);
    swprintf_s(part, MAX_PATH, L"%s.part.mp4", dst);
    DeleteFileW(part);
    MFCreateAttributes(&at, 1);
    IMFAttributes_SetUINT32(at, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    hr = MFCreateSinkWriterFromURL(part, NULL, at, &wr);
    IMFAttributes_Release(at);
    if (FAILED(hr)) { seterr(err, errn, "Windows could not create the MP4 file", hr); goto done; }
    /* H.264: about 2.5 Mbit/s for the disc's 640x480 (less for a cropped picture) */
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(t, &MF_MT_AVG_BITRATE, (UINT32)(2500000.0 * w * h / (640.0 * 480.0)));
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, fps > 29.9 && fps < 30.0 ? ((UINT64)30000 << 32) | 1001
                                                                          : ((UINT64)(UINT32)(fps * 1000) << 32) | 1000);
    IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    IMFMediaType_SetUINT32(t, &MF_MT_MPEG2_PROFILE, 100);   /* eAVEncH264VProfile_High */
    hr = IMFSinkWriter_AddStream(wr, t, &vs);
    IMFMediaType_Release(t);
    if (FAILED(hr)) { seterr(err, errn, "Windows has no H.264 encoder (a Windows N edition needs the Media Feature Pack)", hr); goto done; }
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, fps > 29.9 && fps < 30.0 ? ((UINT64)30000 << 32) | 1001
                                                                          : ((UINT64)(UINT32)(fps * 1000) << 32) | 1000);
    IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    hr = IMFSinkWriter_SetInputMediaType(wr, vs, t, NULL);
    IMFMediaType_Release(t);
    if (FAILED(hr)) { seterr(err, errn, "The H.264 encoder did not take the pictures", hr); goto done; }
    /* AAC, 192 kbit/s */
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    hr = IMFSinkWriter_AddStream(wr, t, &as);
    IMFMediaType_Release(t);
    if (FAILED(hr)) { seterr(err, errn, "Windows has no AAC encoder", hr); goto done; }
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(2 * ch));
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(2 * ch * rate));
    if (ch == 6) IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_CHANNEL_MASK, 0x3F);   /* FL FR FC LFE BL BR */
    hr = IMFSinkWriter_SetInputMediaType(wr, as, t, NULL);
    IMFMediaType_Release(t);
    if (FAILED(hr)) { seterr(err, errn, "The AAC encoder did not take the sound", hr); goto done; }
    if (FAILED(hr = IMFSinkWriter_BeginWriting(wr))) { seterr(err, errn, "The MP4 file could not be started", hr); goto done; }
    fdur = (LONGLONG)(1e7 / fps + 0.5);
    for (;;) {
        IMFSample *s;
        IMFMediaBuffer *b;
        BYTE *p;
        LONGLONG vts = (LONGLONG)(frames * 1e7 / fps + 0.5);
        if (cancel && *cancel) { ok = -1; break; }
        /* one picture */
        if (FAILED(MFCreateMemoryBuffer((DWORD)((size_t)w * h * 3 / 2), &b))) break;
        IMFMediaBuffer_Lock(b, &p, NULL, NULL);
        frame_nv12_crop(m, p, cx, cy, cw, chh);
        IMFMediaBuffer_Unlock(b);
        IMFMediaBuffer_SetCurrentLength(b, (DWORD)((size_t)w * h * 3 / 2));
        MFCreateSample(&s);
        IMFSample_AddBuffer(s, b);
        IMFSample_SetSampleTime(s, vts);
        IMFSample_SetSampleDuration(s, fdur);
        hr = IMFSinkWriter_WriteSample(wr, vs, s);
        IMFSample_Release(s);
        IMFMediaBuffer_Release(b);
        if (FAILED(hr)) { seterr(err, errn, "The H.264 encoder stopped", hr); break; }
        frames++;
        /* the sound up to the next picture's time */
        while (have_audio && apos * 1e7 / rate < (frames) * 1e7 / fps) {
            int want = rate / 10, got = media_audio(m, pcm, want);
            if (got <= 0) { have_audio = 0; break; }
            if (FAILED(MFCreateMemoryBuffer((DWORD)(got * ch * 2), &b))) break;
            IMFMediaBuffer_Lock(b, &p, NULL, NULL);
            memcpy(p, pcm, (size_t)got * ch * 2);
            IMFMediaBuffer_Unlock(b);
            IMFMediaBuffer_SetCurrentLength(b, (DWORD)(got * ch * 2));
            MFCreateSample(&s);
            IMFSample_AddBuffer(s, b);
            IMFSample_SetSampleTime(s, (LONGLONG)(apos * 1e7 / rate + 0.5));
            IMFSample_SetSampleDuration(s, (LONGLONG)(got * 1e7 / rate + 0.5));
            hr = IMFSinkWriter_WriteSample(wr, as, s);
            IMFSample_Release(s);
            IMFMediaBuffer_Release(b);
            if (FAILED(hr)) { have_audio = 0; break; }
            apos += got;
        }
        if (progress && (frames % 30) == 0) crop_progress_fn(media_progress(m), &cp);
        if (!media_next_frame(m, &vt)) { ok = 1; break; }
    }
    if (ok == 1) {
        /* the rest of the sound */
        int got;
        while ((got = media_audio(m, pcm, rate / 10)) > 0) {
            IMFSample *s;
            IMFMediaBuffer *b;
            BYTE *p;
            if (FAILED(MFCreateMemoryBuffer((DWORD)(got * ch * 2), &b))) break;
            IMFMediaBuffer_Lock(b, &p, NULL, NULL);
            memcpy(p, pcm, (size_t)got * ch * 2);
            IMFMediaBuffer_Unlock(b);
            IMFMediaBuffer_SetCurrentLength(b, (DWORD)(got * ch * 2));
            MFCreateSample(&s);
            IMFSample_AddBuffer(s, b);
            IMFSample_SetSampleTime(s, (LONGLONG)(apos * 1e7 / rate + 0.5));
            IMFSample_SetSampleDuration(s, (LONGLONG)(got * 1e7 / rate + 0.5));
            IMFSinkWriter_WriteSample(wr, as, s);
            IMFSample_Release(s);
            IMFMediaBuffer_Release(b);
            apos += got;
        }
        if (FAILED(hr = IMFSinkWriter_Finalize(wr))) { seterr(err, errn, "The MP4 file could not be finished", hr); ok = 0; }
    }
done:
    if (wr) IMFSinkWriter_Release(wr);
    media_close(m);
    free(nv12);
    free(pcm);
    if (ok == 1) {
        if (!MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)) { seterr(err, errn, "The MP4 file could not be renamed", E_FAIL); ok = 0; }
    }
    if (ok != 1) DeleteFileW(part);
    if (progress && ok == 1) progress(1.0, user);
    return ok;
}
