/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On POSIX hosts apu_sdl2.c provides
 * the same xa2_* functions over SDL2. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <math.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
/* Device buffer size and count. Defaults 512 x 4 (10.7 ms each, 43 ms
 * worst case queued); RECOMP_AUDIO_BUF_SAMPLES and RECOMP_AUDIO_BUF_COUNT
 * override them at xa2_init, clamped to the static storage below. */
#define XA2_BUF_SAMPLES_DEFAULT 512
#define XA2_NUM_BUFS_DEFAULT    4
#define XA2_BUF_SAMPLES_MIN     128
#define XA2_BUF_SAMPLES_MAX     4096
#define XA2_NUM_BUFS_MIN        2
#define XA2_NUM_BUFS_MAX        8

static int g_xa2_buf_samples = XA2_BUF_SAMPLES_DEFAULT;
static int g_xa2_num_bufs    = XA2_NUM_BUFS_DEFAULT;
#define XA2_BUF_SAMPLES g_xa2_buf_samples
#define XA2_NUM_BUFS    g_xa2_num_bufs

static int xa2_env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !*e) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
/* One slot more than can be queued: with every buffer on the voice, the
 * next one is staged in a slot the voice does not hold. With only
 * XA2_NUM_BUFS slots the staging buffer was the oldest queued one, and
 * filling it overwrote audio the device had not played yet. */
#define XA2_RING (XA2_NUM_BUFS + 1)
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS_MAX + 1][XA2_BUF_SAMPLES_MAX][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_fill = 0;  /* samples staged in bufs[next_buf] */
static unsigned  g_xa2_underruns;
static int       g_xa2_dry;
static ULONGLONG g_xa2_underrun_logged;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

/* ---- device health and the audio report -----------------------------------
 *
 * A player reported no sound on a log that showed nothing wrong (1.0.7). The
 * engine had no error callback, so a lost device (unplugged, disabled, a
 * driver reset) or a change of the default output (headphones, HDMI, a
 * Bluetooth headset) left the game playing into nothing with no trace.
 *
 *  - XAudio2's OnCriticalError is logged, and the audio thread restarts the
 *    engine on the current default device at its next submit.
 *  - A watcher thread asks Windows for the default output every 2 s (one COM
 *    call, off the audio thread) and asks for the same restart when it changed,
 *    or logs when there is no output device at all.
 *  - At start the output's kind (speakers, headphones, HDMI...), channels and
 *    rate are logged. Not its name: Bluetooth names often carry the owner's.
 *  - Every 30 s one line: the game's peak level, how long it was silent,
 *    buffers played, underruns and refused buffers in that time. The level is
 *    the largest sample of what the game handed over, found in the copy the
 *    submit already makes; a silent game (all buffers near zero) and a silent
 *    device read differently in it.
 * RECOMP_AUDIO_FOLLOW_DEFAULT=0 keeps the first device (no restarts on a
 * default change). */
static volatile LONG g_xa2_restart;         /* 1: restart at the next submit */
static volatile LONG g_xa2_crit_hr;
static const char   *g_xa2_restart_why;
static ULONGLONG     g_xa2_restart_last;
static unsigned      g_xa2_restarts;
static uint64_t      g_xa2_played_base;     /* SamplesPlayed before the last restart */
static int           g_xa2_peak, g_xa2_buf_peak;
static unsigned      g_xa2_silent_bufs, g_xa2_rep_bufs, g_xa2_rep_refused, g_xa2_rep_under;
static ULONGLONG     g_xa2_rep_at;
static int           g_xa2_heard;           /* the game has produced sound */
static WCHAR         g_xa2_dev_id[256];
static CRITICAL_SECTION g_xa2_dev_lock;
static int           g_xa2_watch_started;

static void STDMETHODCALLTYPE xa2_cb_pass_start(IXAudio2EngineCallback *self)
{
    (void)self;
}
static void STDMETHODCALLTYPE xa2_cb_pass_end(IXAudio2EngineCallback *self)
{
    (void)self;
}
static void STDMETHODCALLTYPE xa2_cb_critical(IXAudio2EngineCallback *self, HRESULT error)
{
    (void)self;
    InterlockedExchange(&g_xa2_crit_hr, (LONG)error);
    InterlockedExchange(&g_xa2_restart, 1);
}
static IXAudio2EngineCallbackVtbl g_xa2_cb_vtbl = {
    xa2_cb_pass_start, xa2_cb_pass_end, xa2_cb_critical
};
static IXAudio2EngineCallback g_xa2_cb = { &g_xa2_cb_vtbl };

/* GUIDs from mmdeviceapi.h / functiondiscoverykeys, here so no INITGUID unit is needed. */
static const CLSID xa2_CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID xa2_IID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const PROPERTYKEY xa2_PKEY_FormFactor =
    {{0x1DA5D803, 0xD492, 0x4EDD, {0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E}}, 0};

/* The default render endpoint's id into out (empty: none), and its kind. */
static HRESULT xa2_default_device(WCHAR *out, size_t cap, const char **kind)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    LPWSTR id = NULL;
    HRESULT hr;
    out[0] = 0;
    if (kind) *kind = "unknown";
    hr = CoCreateInstance(&xa2_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &xa2_IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr))
        return hr;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr) && SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) {
        lstrcpynW(out, id, (int)cap);
        CoTaskMemFree(id);
    }
    if (SUCCEEDED(hr) && kind) {
        static const char *const names[] = {
            "remote network device", "speakers", "line level", "headphones", "microphone",
            "headset", "handset", "unknown digital passthrough", "S/PDIF", "HDMI or DisplayPort"
        };
        IPropertyStore *ps = NULL;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &xa2_PKEY_FormFactor, &v)) &&
                v.vt == VT_UI4 && v.ulVal < sizeof names / sizeof names[0])
                *kind = names[v.ulVal];
            PropVariantClear(&v);
            IPropertyStore_Release(ps);
        }
    }
    if (dev) IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    return hr;
}

static int xa2_follow_default(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_AUDIO_FOLLOW_DEFAULT");
        on = !(e && e[0] == '0');
    }
    return on;
}

static DWORD WINAPI xa2_watch_thread(LPVOID unused)
{
    WCHAR now[256];
    int missing_logged = 0;
    const char *test = getenv("RECOMP_AUDIO_TEST_RESTART");   /* test: one restart after n s */
    int test_left = test && *test ? atoi(test) : 0;
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        HRESULT hr;
        int changed;
        Sleep(2000);
        if (test_left > 0 && (test_left -= 2) <= 0) {
            g_xa2_restart_why = "test restart (RECOMP_AUDIO_TEST_RESTART)";
            InterlockedExchange(&g_xa2_restart, 1);
        }
        hr = xa2_default_device(now, sizeof now / sizeof now[0], NULL);
        if (FAILED(hr) || !now[0]) {
            if (!missing_logged) {
                missing_logged = 1;
                fprintf(stderr, "[XA2] Windows reports no audio output device (0x%08lX):"
                                " nothing can be heard until one is connected or enabled\n",
                        (unsigned long)hr);
            }
            continue;
        }
        if (missing_logged) {
            missing_logged = 0;
            fprintf(stderr, "[XA2] an audio output device is available again\n");
        }
        EnterCriticalSection(&g_xa2_dev_lock);
        changed = g_xa2_dev_id[0] && lstrcmpW(now, g_xa2_dev_id) != 0;
        LeaveCriticalSection(&g_xa2_dev_lock);
        if (changed && xa2_follow_default() && !g_xa2_restart) {
            g_xa2_restart_why = "Windows' default output device changed";
            InterlockedExchange(&g_xa2_restart, 1);
        }
    }
}

/* Once per engine start: what the sound goes to. */
static void xa2_log_device(void)
{
    const char *kind = "unknown";
    WCHAR id[256];
    XAUDIO2_VOICE_DETAILS det;
    DWORD mask = 0;
    HRESULT hr = xa2_default_device(id, sizeof id / sizeof id[0], &kind);
    memset(&det, 0, sizeof det);
    if (g_xa2_master) {
        IXAudio2MasteringVoice_GetVoiceDetails(g_xa2_master, &det);
        IXAudio2MasteringVoice_GetChannelMask(g_xa2_master, &mask);
    }
    EnterCriticalSection(&g_xa2_dev_lock);
    lstrcpynW(g_xa2_dev_id, SUCCEEDED(hr) ? id : L"", 256);
    LeaveCriticalSection(&g_xa2_dev_lock);
    if (FAILED(hr) || !id[0])
        fprintf(stderr, "[XA2] output: Windows has no default audio device (0x%08lX)\n",
                (unsigned long)hr);
    else
        fprintf(stderr, "[XA2] output: default device, %s; mixing %u channels at %u Hz"
                        " (channel mask 0x%lX)\n",
                kind, det.InputChannels, det.InputSampleRate, (unsigned long)mask);
}

static double xa2_dbfs(int peak)
{
    return peak > 0 ? 20.0 * log10(peak / 32767.0) : -120.0;
}

static void xa2_report(ULONGLONG now)
{
    double secs = (now - g_xa2_rep_at) / 1000.0;
    double silent = (double)g_xa2_silent_bufs * XA2_BUF_SAMPLES / XA2_SAMPLE_RATE;
    if (g_xa2_peak > 0)
        fprintf(stderr, "[XA2] last %.0f s: game peak %.1f dBFS, silent %.1f s, %u buffers played,"
                        " %u underruns, %u refused; %u restarts\n",
                secs, xa2_dbfs(g_xa2_peak), silent, g_xa2_rep_bufs, g_xa2_rep_under,
                g_xa2_rep_refused, g_xa2_restarts);
    else
        fprintf(stderr, "[XA2] last %.0f s: the game sent only silence (%u buffers played,"
                        " %u underruns, %u refused; %u restarts)%s\n",
                secs, g_xa2_rep_bufs, g_xa2_rep_under, g_xa2_rep_refused, g_xa2_restarts,
                g_xa2_heard ? "" : "; no sound from the game yet");
    g_xa2_peak = 0;
    g_xa2_silent_bufs = g_xa2_rep_bufs = g_xa2_rep_refused = g_xa2_rep_under = 0;
    g_xa2_rep_at = now;
}

static void xa2_restart_now(void)
{
    ULONGLONG now = GetTickCount64();
    HRESULT crit = (HRESULT)InterlockedExchange(&g_xa2_crit_hr, 0);
    const char *why = g_xa2_restart_why;
    if (now - g_xa2_restart_last < 2000)
        return;                         /* at most every 2 s; the flag stays set */
    g_xa2_restart_last = now;
    InterlockedExchange(&g_xa2_restart, 0);
    g_xa2_restart_why = NULL;
    if (crit)
        fprintf(stderr, "[XA2] audio engine error 0x%08lX (the output device was lost or"
                        " changed); restarting audio on the current default device\n",
                (unsigned long)crit);
    else
        fprintf(stderr, "[XA2] %s; restarting audio on it\n", why ? why : "restart asked");
    if (g_xa2_source) {
        XAUDIO2_VOICE_STATE st;
        IXAudio2SourceVoice_GetState(g_xa2_source, &st, 0);
        g_xa2_played_base += st.SamplesPlayed;
    }
    xa2_shutdown();
    g_xa2_restarts++;
    if (!xa2_init()) {
        fprintf(stderr, "[XA2] restart failed; trying again in 2 s\n");
        InterlockedExchange(&g_xa2_restart, 1);
    }
}

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    g_xa2_buf_samples = xa2_env_int("RECOMP_AUDIO_BUF_SAMPLES",
                                    XA2_BUF_SAMPLES_DEFAULT,
                                    XA2_BUF_SAMPLES_MIN, XA2_BUF_SAMPLES_MAX);
    g_xa2_num_bufs = xa2_env_int("RECOMP_AUDIO_BUF_COUNT", XA2_NUM_BUFS_DEFAULT,
                                 XA2_NUM_BUFS_MIN, XA2_NUM_BUFS_MAX);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }
    hr = IXAudio2_RegisterForCallbacks(g_xa2, &g_xa2_cb);
    if (FAILED(hr))
        fprintf(stderr, "[XA2] RegisterForCallbacks failed: 0x%08lX (device loss will not be seen)\n", hr);

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_fill = 0;
    g_xa2_dry = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers, %.1f ms queued max)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES,
            1000.0 * XA2_NUM_BUFS * XA2_BUF_SAMPLES / XA2_SAMPLE_RATE);
    if (!g_xa2_watch_started) {
        HANDLE t;
        InitializeCriticalSection(&g_xa2_dev_lock);
        g_xa2_watch_started = 1;
        g_xa2_rep_at = GetTickCount64();
        t = CreateThread(NULL, 0, xa2_watch_thread, NULL, 0, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_LOWEST);
            CloseHandle(t);
        }
    }
    xa2_log_device();
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_UnregisterForCallbacks(g_xa2, &g_xa2_cb);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Hand mixed samples to XAudio2. Called from the APU frame thread with one
 * EP period (256 samples) at a time. Samples accumulate in the current device
 * buffer, which is submitted when full, so a call shorter than a buffer is
 * kept, not padded or dropped. Returns 1 if every sample was accepted; 0 if
 * the voice already had every buffer queued when one filled, in which case
 * that buffer is discarded (pacing by queue depth is what prevents it). */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx, n, ok = 1, i, pk;
    HRESULT hr;

    if (g_xa2_restart)
        xa2_restart_now();
    if (!g_xa2_initialized || !g_xa2_source) return 0;
    {
        ULONGLONG now = GetTickCount64();
        if (now - g_xa2_rep_at >= 30000)
            xa2_report(now);
    }

    while (num_samples > 0) {
        idx = g_xa2_next_buf;
        n = XA2_BUF_SAMPLES - g_xa2_fill;
        if (n > num_samples) n = num_samples;
        memcpy(g_xa2_bufs[idx][g_xa2_fill], samples,
               n * XA2_CHANNELS * sizeof(int16_t));
        pk = g_xa2_buf_peak;
        for (i = 0; i < n * XA2_CHANNELS; i++) {
            int v = samples[i] < 0 ? -(int)samples[i] : samples[i];
            if (v > pk) pk = v;
        }
        g_xa2_buf_peak = pk;
        g_xa2_fill += n;
        samples += n * XA2_CHANNELS;
        num_samples -= n;
        if (g_xa2_fill < XA2_BUF_SAMPLES)
            break;

        g_xa2_fill = 0;
        if (g_xa2_buf_peak > g_xa2_peak) g_xa2_peak = g_xa2_buf_peak;
        if (g_xa2_buf_peak < 8) g_xa2_silent_bufs++;
        else if (!g_xa2_heard) {
            g_xa2_heard = 1;
            fprintf(stderr, "[XA2] first sound from the game (peak %.1f dBFS)\n",
                    xa2_dbfs(g_xa2_buf_peak));
        }
        g_xa2_buf_peak = 0;
        IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                     XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if ((int)state.BuffersQueued >= XA2_NUM_BUFS) { ok = 0; g_xa2_rep_refused++; continue; }

        memset(&xbuf, 0, sizeof(xbuf));
        xbuf.AudioBytes = XA2_BUF_SAMPLES * XA2_CHANNELS * sizeof(int16_t);
        xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
        hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
        if (FAILED(hr)) { ok = 0; continue; }

        g_xa2_next_buf = (idx + 1) % XA2_RING;
        g_xa2_frames_written++;
        g_xa2_rep_bufs++;
    }
    return ok;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

int xa2_get_buffer_count(void)
{
    return XA2_NUM_BUFS;
}

/* Samples handed over and not yet played: the buffers the voice still
 * holds plus what is staged in the next one. Also the underrun detector:
 * the voice running dry after audio has started is one underrun, counted
 * once per dry spell and reported at most once a second. A drain announced
 * by xa2_expect_drain (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                 XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written > 0) {
        if (!g_xa2_dry) {
            ULONGLONG now = GetTickCount64();
            g_xa2_dry = 1;
            g_xa2_underruns++;
            g_xa2_rep_under++;
            if (now - g_xa2_underrun_logged >= 1000) {
                g_xa2_underrun_logged = now;
                fprintf(stderr, "[XA2] underrun x%u\n", g_xa2_underruns);
            }
        }
    } else {
        g_xa2_dry = 0;
    }
    return (int)state.BuffersQueued * XA2_BUF_SAMPLES + g_xa2_fill;
}

/* Mark the coming dry spell as already seen; the first call that finds
 * buffers queued again clears it, so later dry spells count as before. */
void xa2_expect_drain(void)
{
    g_xa2_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_xa2_underruns;
}

uint64_t xa2_samples_played(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    return g_xa2_played_base + state.SamplesPlayed;   /* continuous across restarts */
}

#else /* !_WIN32: apu_sdl2.c implements the same contract over SDL2 */

/* ISO C wants at least one declaration in a translation unit. */
typedef int apu_xaudio2_posix_unused;

#endif /* _WIN32 */
