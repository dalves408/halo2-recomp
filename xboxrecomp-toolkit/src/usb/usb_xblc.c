/*
 * usb_xblc.c -- an Xbox Live Communicator. See usb_xblc.h.
 *
 * Everything here runs on the OHCI controller thread (ohci.c), which is the
 * only caller, so nothing is locked. The host microphone is read by polling
 * its finished capture buffers from that same thread rather than from a
 * callback.
 */
#include "usb_xblc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <mmsystem.h>
#  pragma comment(lib, "winmm.lib")
#endif

/* ---- descriptors ------------------------------------------------------- */

static const uint8_t s_device_desc[18] = {
    18,             /* bLength                                    */
    0x01,           /* bDescriptorType: DEVICE                    */
    0x10, 0x01,     /* bcdUSB 1.10                                */
    0x00,           /* bDeviceClass: per interface                */
    0x00,           /* bDeviceSubClass                            */
    0x00,           /* bDeviceProtocol                            */
    0x08,           /* bMaxPacketSize0: 8                         */
    0x5E, 0x04,     /* idVendor  0x045E Microsoft                 */
    0x83, 0x02,     /* idProduct 0x0283 Xbox Live Communicator    */
    0x10, 0x01,     /* bcdDevice                                  */
    0x00,           /* iManufacturer: none                        */
    0x00,           /* iProduct: none                             */
    0x00,           /* iSerialNumber: none                        */
    0x01            /* bNumConfigurations                         */
};

/* Configuration, both interfaces and their endpoints. The endpoint
 * descriptors are the 9-byte audio form (bRefresh, bSynchAddress), as the
 * real device reports them. */
static const uint8_t s_config_desc[45] = {
    /* configuration: 2 interfaces, bus powered, 100 mA */
    9, 0x02, 45, 0x00, 0x02, 0x01, 0x00, 0x80, 50,
    /* interface 0: the earpiece, class 0x78 */
    9, 0x04, 0x00, 0x00, 0x01, 0x78, 0x00, 0x00, 0x00,
    /* endpoint 0x04 OUT, isochronous asynchronous, 48 bytes, every frame */
    9, 0x05, 0x04, 0x05, 0x30, 0x00, 0x01, 0x00, 0x00,
    /* interface 1: the microphone, class 0x78 */
    9, 0x04, 0x01, 0x00, 0x01, 0x78, 0x00, 0x00, 0x00,
    /* endpoint 0x85 IN, isochronous asynchronous, 48 bytes, every frame */
    9, 0x05, 0x85, 0x05, 0x30, 0x00, 0x01, 0x00, 0x00
};

static const unsigned s_rates[5] = { 8000, 11025, 16000, 22050, 24000 };

static uint8_t  s_address;
static uint8_t  s_configuration;
static unsigned s_rate = 16000;         /* the device's default */
static int      s_trace = -1;

static unsigned long s_mic_frames, s_mic_underruns, s_speaker_frames;
static int s_tone;                      /* RECOMP_XBLC_MIC=tone */

uint8_t usb_xblc_address(void)   { return s_address; }
int usb_xblc_configured(void)    { return s_configuration != 0; }

static int trace_on(void)
{
    if (s_trace < 0)
        s_trace = getenv("RECOMP_USB_TRACE") != NULL;
    return s_trace;
}

/* ---- the host microphone ----------------------------------------------- */

/* Captured samples waiting for the console, oldest first. Kept to at most
 * 100 ms, so a console that reads slower than the microphone records (the
 * controller thread runs late now and then) costs a skip, not latency that
 * grows for the rest of the session. */
#define RING_SAMPLES 8192u
static int16_t  s_ring[RING_SAMPLES];
static unsigned s_ring_head, s_ring_count;

static void ring_push(const int16_t *src, unsigned n)
{
    unsigned cap = s_rate / 10u;        /* 100 ms */
    unsigned i;

    for (i = 0; i < n; i++) {
        s_ring[(s_ring_head + s_ring_count) % RING_SAMPLES] = src[i];
        if (s_ring_count < RING_SAMPLES)
            s_ring_count++;
        else
            s_ring_head = (s_ring_head + 1u) % RING_SAMPLES;
    }
    if (s_ring_count > cap) {
        /* Keep the newest half of the cap: dropping only the excess would
         * leave the queue sitting at the cap and skipping every frame. */
        unsigned drop = s_ring_count - cap / 2u;
        s_ring_head = (s_ring_head + drop) % RING_SAMPLES;
        s_ring_count -= drop;
    }
}

static unsigned ring_pop(int16_t *dst, unsigned n)
{
    unsigned i;
    if (n > s_ring_count)
        n = s_ring_count;
    for (i = 0; i < n; i++)
        dst[i] = s_ring[(s_ring_head + i) % RING_SAMPLES];
    s_ring_head = (s_ring_head + n) % RING_SAMPLES;
    s_ring_count -= n;
    return n;
}

/* ---- the high-pass filter ---------------------------------------------- */

/* Laptop mics carry fan and mains hum (74 Hz on one test laptop, 85% of its
 * background below 300 Hz). The 3.2 kbit/s SC3 codec turns that into a
 * buzz under the voice, and it hides speech from the gate. Xbox voice is
 * a 300-3400 Hz channel anyway, so a 4th-order Butterworth high-pass at
 * RECOMP_XBLC_HIGHPASS Hz (default 150; 0 = off) costs the voice nothing:
 * on that laptop it took the background from -34 to -41 dBFS and left
 * speech within 1 dB. */
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } Biquad;
static Biquad   s_hp[2];
static unsigned s_hp_rate;
static double   s_hp_hz = -1.0;

static void hp_design(Biquad *q, double f0, double qf, unsigned rate)
{
    double w0 = 2.0 * 3.14159265358979 * f0 / (double)rate;
    double al = sin(w0) / (2.0 * qf), c = cos(w0), a0 = 1.0 + al;
    q->b0 = (1.0 + c) / 2.0 / a0;
    q->b1 = -(1.0 + c) / a0;
    q->b2 = (1.0 + c) / 2.0 / a0;
    q->a1 = -2.0 * c / a0;
    q->a2 = (1.0 - al) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0;
}

static void highpass_block(int16_t *pcm, unsigned n, unsigned rate)
{
    unsigned i;
    int k;

    if (s_hp_hz < 0.0) {
        const char *e = getenv("RECOMP_XBLC_HIGHPASS");
        s_hp_hz = e && *e ? atof(e) : 150.0;
        if (s_hp_hz > 0.0)
            fprintf(stderr, "  [XBLC] high-pass %.0f Hz on the microphone "
                            "(RECOMP_XBLC_HIGHPASS)\n", s_hp_hz);
        else
            fprintf(stderr, "  [XBLC] high-pass off (RECOMP_XBLC_HIGHPASS=0)\n");
        fflush(stderr);
    }
    if (s_hp_hz <= 0.0 || s_hp_hz >= rate / 2.0)
        return;
    if (s_hp_rate != rate) {
        /* The two Q values of a 4th-order Butterworth. */
        hp_design(&s_hp[0], s_hp_hz, 0.5412, rate);
        hp_design(&s_hp[1], s_hp_hz, 1.3066, rate);
        s_hp_rate = rate;
    }
    for (i = 0; i < n; i++) {
        double v = (double)pcm[i];
        for (k = 0; k < 2; k++) {
            Biquad *q = &s_hp[k];
            double y = q->b0 * v + q->b1 * q->x1 + q->b2 * q->x2
                     - q->a1 * q->y1 - q->a2 * q->y2;
            q->x2 = q->x1; q->x1 = v;
            q->y2 = q->y1; q->y1 = y;
            v = y;
        }
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        pcm[i] = (int16_t)v;
    }
}

/* ---- the noise gate ---------------------------------------------------- */

/* A real Communicator is a close-talk boom mic. A laptop's built-in mic
 * hears its own fan and hiss at a level XHV's voice detector takes for
 * speech, so the console transmits a steady noise to everyone. The gate
 * passes a block only when it is clearly louder than the background, and
 * otherwise sends silence.
 *
 * The background is the quietest 20 ms block of the last 3 s (minimum
 * statistics): speech has gaps, so the minimum stays at the noise level
 * while someone talks, and it follows a fan that speeds up within 3 s.
 * The gate opens when a block is RECOMP_XBLC_GATE dB above that (default
 * 10; 0 turns the gate off), holds 400 ms after the last loud block so
 * word endings and short pauses go through, and fades over one block
 * instead of clicking. */
#define GATE_WINDOW   150               /* blocks of history: 3 s at 20 ms */
#define GATE_HOLD_MS  400u
#define GATE_MIN_RMS  12.0              /* about -69 dBFS: never open below.
                                         * An Xbox controller headset at full
                                         * Windows level speaks at -60 dBFS. */

static double   s_gate_ratio = -1.0;    /* amplitude ratio; 0 = off */
static double   s_gate_hist[GATE_WINDOW];
static unsigned s_gate_n, s_gate_pos;
static unsigned s_gate_hold_ms;
static double   s_gate_gain;            /* gain at the end of the last block */
static unsigned long s_gate_blocks, s_gate_open_blocks;
static double   s_gate_floor;

static void gate_config(void)
{
    const char *e = getenv("RECOMP_XBLC_GATE");
    double db = 10.0;
    if (e && *e)
        db = atof(e);
    s_gate_ratio = db > 0.0 ? pow(10.0, db / 20.0) : 0.0;
    if (s_gate_ratio > 0.0)
        fprintf(stderr, "  [XBLC] noise gate %.1f dB over the background "
                        "(RECOMP_XBLC_GATE)\n", db);
    else
        fprintf(stderr, "  [XBLC] noise gate off (RECOMP_XBLC_GATE=0)\n");
    fflush(stderr);
}

/* Gate one block of n samples covering ms milliseconds, in place. Returns
 * nonzero when the block was passed. */
static int gate_block(int16_t *pcm, unsigned n, unsigned ms)
{
    double sum = 0.0, rms, floor_rms, gain, step;
    unsigned i;
    int open;

    if (s_gate_ratio < 0.0)
        gate_config();
    if (s_gate_ratio == 0.0 || n == 0)
        return 1;

    for (i = 0; i < n; i++)
        sum += (double)pcm[i] * (double)pcm[i];
    rms = sqrt(sum / (double)n);

    s_gate_hist[s_gate_pos] = rms;
    s_gate_pos = (s_gate_pos + 1u) % GATE_WINDOW;
    if (s_gate_n < GATE_WINDOW)
        s_gate_n++;
    floor_rms = rms;
    for (i = 0; i < s_gate_n; i++)
        if (s_gate_hist[i] < floor_rms)
            floor_rms = s_gate_hist[i];
    /* The quietest block sits a little under the noise's average level. */
    floor_rms *= 1.5;
    s_gate_floor = floor_rms;

    open = rms > floor_rms * s_gate_ratio && rms > GATE_MIN_RMS;
    if (open)
        s_gate_hold_ms = GATE_HOLD_MS;
    else if (s_gate_hold_ms > ms)
        s_gate_hold_ms -= ms;
    else
        s_gate_hold_ms = 0;
    gain = (open || s_gate_hold_ms) ? 1.0 : 0.0;

    s_gate_blocks++;
    if (gain > 0.0)
        s_gate_open_blocks++;

    /* Ramp from the last block's gain to this one across the block. */
    step = (gain - s_gate_gain) / (double)n;
    if (s_gate_gain != 1.0 || gain != 1.0) {
        double g = s_gate_gain;
        for (i = 0; i < n; i++) {
            g += step;
            pcm[i] = (int16_t)((double)pcm[i] * g);
        }
    }
    s_gate_gain = gain;
    return gain > 0.0;
}

/* ---- automatic gain ---------------------------------------------------- */

/* The real Communicator has automatic gain control, which XHV switches on
 * (vendor request index 1). Host mics vary by 40 dB: a laptop mic
 * speaks at -24 dBFS, an Xbox controller headset at -60 to -65 dBFS even at
 * full Windows level, which the SC3 codec renders as near silence. After the
 * gate, the gain brings speech toward -20 dBFS: it learns the speech level
 * only from blocks the gate passed, so background never pumps it, moves up
 * at about 13 dB a second and down within a block, stays between 1x
 * and 100x (+40 dB), and a soft limiter keeps peaks from clipping.
 * RECOMP_XBLC_AGC=0 turns it off. */
#define AGC_TARGET_RMS 3277.0           /* -20 dBFS */
#define AGC_MAX_GAIN   100.0

static int    s_agc_on = -1;
static double s_agc_level;              /* speech level, rms */
static double s_agc_gain = 1.0;

static void agc_block(int16_t *pcm, unsigned n, int passed)
{
    double sum = 0.0, rms, want, g0, step;
    unsigned i;

    if (s_agc_on < 0) {
        const char *e = getenv("RECOMP_XBLC_AGC");
        s_agc_on = !(e && *e == '0');
        fprintf(stderr, "  [XBLC] automatic gain %s (RECOMP_XBLC_AGC)\n",
                s_agc_on ? "on, speech to -20 dBFS" : "off");
        fflush(stderr);
    }
    if (!s_agc_on || n == 0)
        return;

    if (passed) {
        for (i = 0; i < n; i++)
            sum += (double)pcm[i] * (double)pcm[i];
        rms = sqrt(sum / (double)n);
        if (rms > GATE_MIN_RMS) {
            /* Loud blocks count at once; quieter ones bring the estimate
             * down over about a second of speech, so one loud bump (the
             * headset being handled) does not hold the gain down. */
            if (rms > s_agc_level)
                s_agc_level = s_agc_level * 0.5 + rms * 0.5;
            else
                s_agc_level = s_agc_level * 0.95 + rms * 0.05;
        }
    }
    want = s_agc_level > 0.0 ? AGC_TARGET_RMS / s_agc_level : 1.0;
    if (want > AGC_MAX_GAIN) want = AGC_MAX_GAIN;
    if (want < 1.0) want = 1.0;
    g0 = s_agc_gain;
    if (want < s_agc_gain)
        s_agc_gain = want;                       /* down at once */
    else
        s_agc_gain *= want / s_agc_gain > 1.03 ? 1.03 : want / s_agc_gain;
    step = (s_agc_gain - g0) / (double)n;
    for (i = 0; i < n; i++) {
        double v = (double)pcm[i] * (g0 + step * (double)(i + 1));
        /* Soft limit above -6 dBFS. */
        if (v > 16384.0)
            v = 16384.0 + 16383.0 * tanh((v - 16384.0) / 16383.0);
        else if (v < -16384.0)
            v = -16384.0 - 16384.0 * tanh((-v - 16384.0) / 16384.0);
        pcm[i] = (int16_t)v;
    }
}

/* One line every 30 s: how loud the background is and how often the gate
 * was open, for tuning RECOMP_XBLC_GATE. */
static void gate_report(void)
{
    static unsigned long last_blocks;
    static unsigned long last_open;
    unsigned long b = s_gate_blocks - last_blocks;
    if (s_gate_ratio <= 0.0 || b < 1500u)          /* 30 s of 20 ms blocks */
        return;
    fprintf(stderr, "  [XBLC] gate: background %.0f rms (%.1f dBFS), open %lu%% "
                    "of the last 30 s; gain %.1fx, speech %.1f dBFS\n", s_gate_floor,
            s_gate_floor > 0.0 ? 20.0 * log10(s_gate_floor / 32768.0) : -99.0,
            (s_gate_open_blocks - last_open) * 100u / b, s_agc_gain,
            s_agc_level > 0.0 ? 20.0 * log10(s_agc_level / 32768.0) : -99.0);
    fflush(stderr);
    last_blocks = s_gate_blocks;
    last_open = s_gate_open_blocks;
}

#if defined(_WIN32)
/* Eight 20 ms capture buffers, sized for the highest rate. */
#define CAP_BUFS     8
#define CAP_MS       20
#define CAP_SAMPLES  (24000 * CAP_MS / 1000)
static HWAVEIN  s_wavein;
static WAVEHDR  s_hdr[CAP_BUFS];
static int16_t  s_capbuf[CAP_BUFS][CAP_SAMPLES];
static unsigned s_open_rate;
static int      s_mic_wanted;           /* plugged in, and not switched off */

static void mic_close(void)
{
    int i;
    if (!s_wavein)
        return;
    waveInReset(s_wavein);
    for (i = 0; i < CAP_BUFS; i++)
        waveInUnprepareHeader(s_wavein, &s_hdr[i], sizeof s_hdr[i]);
    waveInClose(s_wavein);
    s_wavein = NULL;
    s_open_rate = 0;
}

static void mic_open(unsigned rate)
{
    WAVEFORMATEX wf;
    MMRESULT r;
    unsigned frames = rate * CAP_MS / 1000u;
    int i;

    mic_close();
    s_ring_head = s_ring_count = 0;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = rate;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 2;
    wf.nAvgBytesPerSec = rate * 2u;

    r = waveInOpen(&s_wavein, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        static int warned;
        s_wavein = NULL;
        if (!warned++) {
            fprintf(stderr, "  [XBLC] no microphone (waveInOpen %u Hz: error %u); "
                            "the headset sends silence\n", rate, (unsigned)r);
            fflush(stderr);
        }
        return;
    }
    for (i = 0; i < CAP_BUFS; i++) {
        memset(&s_hdr[i], 0, sizeof s_hdr[i]);
        s_hdr[i].lpData = (LPSTR)s_capbuf[i];
        s_hdr[i].dwBufferLength = frames * 2u;
        waveInPrepareHeader(s_wavein, &s_hdr[i], sizeof s_hdr[i]);
        waveInAddBuffer(s_wavein, &s_hdr[i], sizeof s_hdr[i]);
    }
    waveInStart(s_wavein);
    s_open_rate = rate;
    fprintf(stderr, "  [XBLC] microphone open at %u Hz\n", rate);
    fflush(stderr);
}

/* RECOMP_XBLC_MIC_WAV=<file>: record the microphone as a stereo WAV, left
 * as captured and right after the high-pass and the noise gate (what the
 * console is given),
 * for RECOMP_XBLC_MIC_WAV_SECS seconds (default 300). The header's sizes
 * are rewritten every second, so the file plays even if the game is
 * killed. */
static FILE    *s_wav;
static int      s_wav_state = -1;       /* -1 not checked, 0 off, 1 recording */
static unsigned long s_wav_frames, s_wav_max, s_wav_rate;

static void wav_header(unsigned long frames)
{
    uint8_t h[44];
    unsigned long data = frames * 4ul;
    memcpy(h, "RIFF", 4);
    h[4] = (uint8_t)(data + 36); h[5] = (uint8_t)((data + 36) >> 8);
    h[6] = (uint8_t)((data + 36) >> 16); h[7] = (uint8_t)((data + 36) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0;                       /* PCM */
    h[22] = 2; h[23] = 0;                       /* stereo */
    h[24] = (uint8_t)s_wav_rate; h[25] = (uint8_t)(s_wav_rate >> 8);
    h[26] = (uint8_t)(s_wav_rate >> 16); h[27] = 0;
    h[28] = (uint8_t)(s_wav_rate * 4); h[29] = (uint8_t)((s_wav_rate * 4) >> 8);
    h[30] = (uint8_t)((s_wav_rate * 4) >> 16); h[31] = 0;
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)data; h[41] = (uint8_t)(data >> 8);
    h[42] = (uint8_t)(data >> 16); h[43] = (uint8_t)(data >> 24);
    fseek(s_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, s_wav);
    fseek(s_wav, 0, SEEK_END);
}

static void wav_write(const int16_t *raw, const int16_t *gated, unsigned n,
                      unsigned rate)
{
    int16_t st[2 * 480];
    unsigned i;

    if (s_wav_state < 0) {
        const char *p = getenv("RECOMP_XBLC_MIC_WAV");
        const char *s = getenv("RECOMP_XBLC_MIC_WAV_SECS");
        s_wav_state = 0;
        if (p && *p && (s_wav = fopen(p, "wb")) != NULL) {
            s_wav_rate = rate;
            s_wav_max = (unsigned long)rate * (unsigned long)(s && atoi(s) > 0 ? atoi(s) : 300);
            wav_header(0);
            s_wav_state = 1;
            fprintf(stderr, "  [XBLC] recording the microphone to %s "
                            "(left as captured, right after the gate)\n", p);
            fflush(stderr);
        }
    }
    if (s_wav_state != 1 || rate != s_wav_rate || n > 480)
        return;
    for (i = 0; i < n; i++) {
        st[2 * i] = raw[i];
        st[2 * i + 1] = gated[i];
    }
    fwrite(st, 4, n, s_wav);
    s_wav_frames += n;
    if (s_wav_frames / rate != (s_wav_frames - n) / rate || s_wav_frames >= s_wav_max)
        wav_header(s_wav_frames);
    if (s_wav_frames >= s_wav_max) {
        fclose(s_wav);
        s_wav = NULL;
        s_wav_state = 0;
        fprintf(stderr, "  [XBLC] microphone recording finished\n");
        fflush(stderr);
    }
}

/* Move every finished capture buffer into the ring and hand it back. */
static void mic_poll(void)
{
    int i;

    if (!s_mic_wanted)
        return;
    if (s_open_rate != s_rate)
        mic_open(s_rate);
    if (!s_wavein)
        return;
    for (i = 0; i < CAP_BUFS; i++) {
        WAVEHDR *h = &s_hdr[i];
        if (!(h->dwFlags & WHDR_DONE))
            continue;
        {
            static int16_t raw[CAP_SAMPLES];
            unsigned n = h->dwBytesRecorded / 2u;
            if (n > CAP_SAMPLES) n = CAP_SAMPLES;
            memcpy(raw, h->lpData, n * 2u);
            highpass_block((int16_t *)h->lpData, n, s_open_rate);
            agc_block((int16_t *)h->lpData, n,
                      gate_block((int16_t *)h->lpData, n, n * 1000u / s_open_rate));
            gate_report();
            wav_write(raw, (const int16_t *)h->lpData, n, s_open_rate);
        }
        ring_push((const int16_t *)h->lpData, h->dwBytesRecorded / 2u);
        h->dwFlags &= ~WHDR_DONE;
        h->dwBytesRecorded = 0;
        waveInAddBuffer(s_wavein, h, sizeof *h);
    }
}

void usb_xblc_start(void)
{
    const char *e = getenv("RECOMP_XBLC_MIC");
    s_mic_wanted = !(e && (*e == '0' || strcmp(e, "tone") == 0));
    s_tone = e && strcmp(e, "tone") == 0;
    if (s_tone) {
        fprintf(stderr, "  [XBLC] microphone replaced by a test tone "
                        "(RECOMP_XBLC_MIC=tone)\n");
        fflush(stderr);
    } else if (!s_mic_wanted) {
        fprintf(stderr, "  [XBLC] microphone off (RECOMP_XBLC_MIC=0)\n");
        fflush(stderr);
    }
}
#else
static void mic_poll(void) {}
void usb_xblc_start(void)
{
    const char *e = getenv("RECOMP_XBLC_MIC");
    s_tone = e && strcmp(e, "tone") == 0;
}
#endif

/* RECOMP_XBLC_MIC=tone: a test signal in place of the microphone, for
 * checking the voice path with nobody talking. Bursts of a warbling tone,
 * 400 ms on and 300 ms off, so a voice-activity detector hears "speech" with
 * gaps between. */
static void tone_fill(int16_t *pcm, unsigned n)
{
    static double phase;
    static unsigned long t;           /* samples since start */
    unsigned i;

    for (i = 0; i < n; i++, t++) {
        double sec = (double)t / (double)s_rate;
        unsigned long ms = (unsigned long)(sec * 1000.0) % 700ul;
        double f = 180.0 + 60.0 * sin(sec * 2.0 * 3.14159265 * 3.0);
        phase += 2.0 * 3.14159265 * f / (double)s_rate;
        if (phase > 2.0 * 3.14159265) phase -= 2.0 * 3.14159265;
        pcm[i] = ms < 400ul
               ? (int16_t)(6000.0 * (sin(phase) + 0.5 * sin(2.0 * phase)
                                     + 0.25 * sin(3.0 * phase)))
               : 0;
    }
}

/* ---- isochronous data -------------------------------------------------- */

int usb_xblc_mic_frame(uint8_t *out, int max)
{
    /* One millisecond of samples. 11025 and 22050 Hz do not divide into
     * whole milliseconds, so the remainder is carried and a frame now and
     * then has one sample more, as the device does it. */
    static unsigned carry;
    unsigned n = s_rate / 1000u, got;
    int16_t pcm[24];

    carry += s_rate % 1000u;
    if (carry >= 1000u) {
        carry -= 1000u;
        n++;
    }
    if (n > 24u) n = 24u;
    if ((int)(n * 2u) > max)
        n = (unsigned)max / 2u;

    if (s_tone) {
        tone_fill(pcm, n);
        s_mic_frames++;
        memcpy(out, pcm, n * 2u);
        return (int)(n * 2u);
    }
    mic_poll();
    got = ring_pop(pcm, n);
    if (got < n) {
        memset(pcm + got, 0, (n - got) * sizeof pcm[0]);
        s_mic_underruns++;
    }
    s_mic_frames++;
    memcpy(out, pcm, n * 2u);
    return (int)(n * 2u);
}

void usb_xblc_speaker_frame(const uint8_t *data, int len)
{
    (void)data;
    (void)len;
    s_speaker_frames++;
}

void usb_xblc_stats(unsigned long *mic_frames, unsigned long *mic_underruns,
                    unsigned long *speaker_frames, unsigned *rate)
{
    *mic_frames = s_mic_frames;
    *mic_underruns = s_mic_underruns;
    *speaker_frames = s_speaker_frames;
    *rate = s_rate;
}

/* ---- control transfers ------------------------------------------------- */

#define REQ_GET_STATUS         0x00
#define REQ_CLEAR_FEATURE      0x01
#define REQ_SET_FEATURE        0x03
#define REQ_SET_ADDRESS        0x05
#define REQ_GET_DESCRIPTOR     0x06
#define REQ_GET_CONFIGURATION  0x08
#define REQ_SET_CONFIGURATION  0x09
#define REQ_GET_INTERFACE      0x0A
#define REQ_SET_INTERFACE      0x0B

static int copy_out(uint8_t *out, int max, const uint8_t *src, int len,
                    uint16_t wLength)
{
    if (len > (int)wLength) len = (int)wLength;
    if (len > max)          len = max;
    if (len > 0)            memcpy(out, src, (size_t)len);
    return len;
}

int usb_xblc_control(const UsbSetup *setup, uint8_t *out, int max)
{
    int is_in = (setup->bmRequestType & 0x80) != 0;
    int type  = (setup->bmRequestType >> 5) & 3;   /* 0 standard, 2 vendor */

    if (trace_on()) {
        fprintf(stderr, "  [XBLC] SETUP %02X %02X value %04X index %04X len %u\n",
                setup->bmRequestType, setup->bRequest, setup->wValue,
                setup->wIndex, setup->wLength);
        fflush(stderr);
    }

    if (type == 0) {
        switch (setup->bRequest) {
        case REQ_GET_DESCRIPTOR:
            switch (setup->wValue >> 8) {
            case 0x01:
                return copy_out(out, max, s_device_desc,
                                (int)sizeof s_device_desc, setup->wLength);
            case 0x02:
                return copy_out(out, max, s_config_desc,
                                (int)sizeof s_config_desc, setup->wLength);
            default:
                return -1;              /* no strings, nothing else */
            }

        case REQ_SET_ADDRESS:
            s_address = (uint8_t)(setup->wValue & 0x7F);
            return 0;

        case REQ_SET_CONFIGURATION:
            s_configuration = (uint8_t)(setup->wValue & 0xFF);
            fprintf(stderr, "  [XBLC] configured (%u) at address %u\n",
                    s_configuration, s_address);
            fflush(stderr);
            return 0;

        case REQ_GET_CONFIGURATION:
            if (!is_in || max < 1) return -1;
            out[0] = s_configuration;
            return 1;

        case REQ_GET_INTERFACE:
            if (!is_in || max < 1) return -1;
            out[0] = 0;                 /* one alternate setting each */
            return 1;

        case REQ_GET_STATUS:
            if (!is_in || max < 2) return -1;
            out[0] = 0; out[1] = 0;
            return 2;

        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
        case REQ_SET_INTERFACE:
            return 0;

        default:
            return -1;
        }
    }

    /* Vendor, to the device: 41 03 SET_FEATURE.
     *   index 0: sample rate, value & 0xFF indexing s_rates
     *   index 1: automatic gain control on/off, accepted and ignored */
    if (type == 2 && !is_in && setup->bRequest == REQ_SET_FEATURE) {
        if (setup->wIndex == 0) {
            unsigned i = setup->wValue & 0xFFu;
            if (i < sizeof s_rates / sizeof s_rates[0]) {
                if (s_rate != s_rates[i]) {
                    fprintf(stderr, "  [XBLC] sample rate %u Hz\n", s_rates[i]);
                    fflush(stderr);
                }
                s_rate = s_rates[i];
            }
            return 0;
        }
        if (setup->wIndex == 1)
            return 0;
    }

    fprintf(stderr, "  [XBLC] unhandled request %02X %02X value %04X index %04X\n",
            setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex);
    fflush(stderr);
    return -1;
}
