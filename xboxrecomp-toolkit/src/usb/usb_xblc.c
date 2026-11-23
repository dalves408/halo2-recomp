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
