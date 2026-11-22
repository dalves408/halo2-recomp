/**
 * Xbox Input Compatibility Layer -- Halo 2 1.5, Windows, XInput plus SDL3.
 *
 * Used instead of the toolkit's xinput_device.c when the player is built with
 * HALO2_SDL3. Xbox controllers stay on XInput exactly as before. Every
 * other controller (PlayStation, Switch, generic pads...) comes from SDL3's
 * gamepad layer and takes a port no XInput pad is using. Rumble works on both:
 * XInputSetState for XInput pads, SDL_RumbleGamepad for the others.
 *
 * Port rule: the connected XInput pads take ports 0, 1, ... in XInput slot
 * order, with no gaps; an SDL pad holds a port no XInput pad is using. An
 * XInput pad that appears takes its port back and the SDL pad moves to a free
 * one.
 *
 * Why not port N = XInput slot N: Windows numbers XInput pads by connection
 * history, so the only pad can sit in slot 1 or 2 (a Bluetooth pad after a
 * virtual one, a re-paired pad) with slot 0 empty -- and then player 1 never
 * saw it. In order, the first pad connected is player 1, the second player 2.
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"

/* src/h2_mouse.c (the game and the downloader both link it) */
void h2_mouse_pad_begin(void);
void h2_mouse_pad_end(unsigned short *buttons, unsigned char *analog);
extern int halo2_overlay_active(void);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

#include <SDL3/SDL.h>

static BOOL g_xi_ok[XBOX_MAX_CONTROLLERS];          /* XInput answered on this port last time */
static BOOL g_controller_connected[XBOX_MAX_CONTROLLERS];

/* Port -> XInput slot (-1: no XInput pad on that port), rescanned once a
 * second: XInputGetCapabilities on an empty slot is slow, so not every read. */
static volatile LONG g_xslot[XBOX_MAX_CONTROLLERS] = { 0, 1, 2, 3 };
static volatile ULONGLONG g_xscan;

static void xi_map(int force)
{
    ULONGLONG now = GetTickCount64();
    LONG slot[XBOX_MAX_CONTROLLERS];
    int port = 0, s;
    if (!force && g_xscan && now - g_xscan < 1000)
        return;
    g_xscan = now ? now : 1;
    for (s = 0; s < XUSER_MAX_COUNT && port < XBOX_MAX_CONTROLLERS; s++) {
        XINPUT_CAPABILITIES caps;
        if (XInputGetCapabilities((DWORD)s, 0, &caps) == ERROR_SUCCESS)
            slot[port++] = s;
    }
    while (port < XBOX_MAX_CONTROLLERS)
        slot[port++] = -1;
    for (s = 0; s < XBOX_MAX_CONTROLLERS; s++) {
        LONG old = InterlockedExchange(&g_xslot[s], slot[s]);
        if (old != slot[s] && slot[s] >= 0)
            fprintf(stderr, "[PAD] player %d: Xbox pad in XInput slot %ld\n", s + 1, (long)slot[s]);
    }
}

static int xi_slot(DWORD port)
{
    if (port >= XBOX_MAX_CONTROLLERS) return -1;
    xi_map(0);
    return (int)g_xslot[port];
}

static DWORD xi_get(DWORD port, XINPUT_STATE *s)
{
    int slot = xi_slot(port);
    return slot >= 0 ? XInputGetState((DWORD)slot, s) : ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD xi_set(DWORD port, XINPUT_VIBRATION *v)
{
    int slot = xi_slot(port);
    return slot >= 0 ? XInputSetState((DWORD)slot, v) : ERROR_DEVICE_NOT_CONNECTED;
}

/* SDL takes every pad that XInput does not already drive: an Xbox pad would
 * appear in both, so those stay with XInput. */
static int sdlpad_wanted_pad(SDL_Gamepad *pad)
{
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return 0;
    default:
        return 1;
    }
}
#define SDLPAD_WANTED(pad)    sdlpad_wanted_pad(pad)
#define SDLPAD_SLOT_FREE(slot) (!g_xi_ok[(slot)])
#include "sdl3_pad.inc"

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one (see the toolkit's xinput_device.c for the history). */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

static void set_sdl_hints(void)
{
    /* Xbox pads belong to XInput here. SDL's own Xbox drivers would open them
     * exclusively and the same pad would show up twice. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE, "0");
}

/* The Windows player never calls xbox_InputInit, so the first use starts
 * everything. The pad state is read from the game thread and, for the
 * overlays, from the window thread: one lock covers SDL's side of it. */
static CRITICAL_SECTION g_lock;

/* Timing of the pad reads (debug builds, or RECOMP_PAD_LOG=1): a stutter that
 * comes from the input layer shows here. One line every 5 s. */
static LARGE_INTEGER g_qpf;
static struct { unsigned long n_xi, n_sdl; double us_xi, us_sdl, max_xi, max_sdl; } g_pf;
static int pad_perf_on(void)
{
    static int v = -1;
    if (v < 0) {
#if 0
        v = 1;
#else
        v = getenv("RECOMP_PAD_LOG") != NULL;
#endif
        QueryPerformanceFrequency(&g_qpf);
    }
    return v;
}
static double qpc_us(LARGE_INTEGER a, LARGE_INTEGER b) { return (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)g_qpf.QuadPart; }
static void pad_perf_report(void)
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (now - last < 5000) return;
    last = now;
    if (g_pf.n_xi || g_pf.n_sdl)
        fprintf(stderr, "[PAD-PERF] XInput reads %lu (avg %.0f us, max %.0f us), SDL reads %lu (avg %.0f us, max %.0f us)\n",
                g_pf.n_xi, g_pf.n_xi ? g_pf.us_xi / g_pf.n_xi : 0.0, g_pf.max_xi,
                g_pf.n_sdl, g_pf.n_sdl ? g_pf.us_sdl / g_pf.n_sdl : 0.0, g_pf.max_sdl);
    memset(&g_pf, 0, sizeof g_pf);
}
static volatile LONG g_state;       /* 0 = not started, 1 = starting, 2 = ready */

static void ensure_init(void)
{
    if (g_state == 2)
        return;
    if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        g_xscan = 0;
        for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++)
            g_xslot[i] = -1;
        xi_map(1);
        for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
            g_xi_ok[i] = g_xslot[i] >= 0;
            g_controller_connected[i] = g_xi_ok[i];
        }
        set_sdl_hints();
        sdlpad_init();
        {
            int nxi = 0, i;
            for (i = 0; i < (int)XBOX_MAX_CONTROLLERS; i++)
                if (g_xi_ok[i]) nxi++;
            if (nxi)
                fprintf(stderr, "[PAD] XInput: %d Xbox pad(s) connected\n", nxi);
            if (!sdlpad_any_open())
                fprintf(stderr, "[PAD] SDL started; no non-Xbox gamepad found yet (one plugged in later is picked up)\n");
        }
        InterlockedExchange(&g_state, 2);
        return;
    }
    while (g_state != 2)
        Sleep(1);
}

void xbox_InputInit(void)
{
    ensure_init();
}

/* XInput first, then an SDL pad on the same port. 1 when a pad answered. */
static int pad_read(DWORD dwPort, XBOX_INPUT_STATE *pState, DWORD *xi_result)
{
    XINPUT_STATE xi_state;
    DWORD result;

    LARGE_INTEGER t0, t1;
    ensure_init();
    if (pad_perf_on()) QueryPerformanceCounter(&t0);
    result = xi_get(dwPort, &xi_state);
    if (pad_perf_on()) {
        double us;
        QueryPerformanceCounter(&t1);
        us = qpc_us(t0, t1);
        g_pf.n_xi++; g_pf.us_xi += us; if (us > g_pf.max_xi) g_pf.max_xi = us;
    }
    if (result == ERROR_SUCCESS) {
        g_xi_ok[dwPort] = TRUE;
        memset(pState, 0, sizeof(XBOX_INPUT_STATE));
        pState->dwPacketNumber = xi_state.dwPacketNumber;
        pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;
        pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
        pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
        pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
        pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;
        return 1;
    }
    if (xi_result)
        *xi_result = result;
    EnterCriticalSection(&g_lock);
    if (g_xi_ok[dwPort]) {
        /* The XInput pad left: its port may go to an SDL pad now. */
        g_xi_ok[dwPort] = FALSE;
        sdlpad_rescan(1);
    }
    if (pad_perf_on() && sdlpad_slot_used((int)dwPort)) {
        LARGE_INTEGER s0, s1;
        double us;
        QueryPerformanceCounter(&s0);
        result = (DWORD)sdlpad_get(dwPort, pState);
        QueryPerformanceCounter(&s1);
        us = qpc_us(s0, s1);
        g_pf.n_sdl++; g_pf.us_sdl += us; if (us > g_pf.max_sdl) g_pf.max_sdl = us;
        pad_perf_report();
    } else {
        result = (DWORD)sdlpad_get(dwPort, pState);
    }
    LeaveCriticalSection(&g_lock);
    return (int)result;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    if (dwPort == 0)
        h2_mouse_pad_begin();      /* src/h2_mouse.c: mouse buttons in play or in menus */
    if (dwPort == 0) {
        EnterCriticalSection(&g_lock);
        sdlpad_rescan(0);
        LeaveCriticalSection(&g_lock);
    }

    if (!pad_read(dwPort, pState, &result)) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            h2_mouse_pad_end(&pState->Gamepad.wButtons, pState->Gamepad.bAnalogButtons);
            if (kb || mkb_enabled()) {
                if (halo2_overlay_active())
                    memset(pState, 0, sizeof *pState);
                return ERROR_SUCCESS;
            }
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0) {
        mkb_merge(dwPort, pState);
        h2_mouse_pad_end(&pState->Gamepad.wButtons, pState->Gamepad.bAnalogButtons);
    }

    if (halo2_overlay_active() && dwPort == 0)
        memset(pState, 0, sizeof *pState);
    return ERROR_SUCCESS;
}

/* The pad on `port` in XInput's own layout, with no keyboard or mouse merged
 * in: the settings and quit overlays are driven by pad 1 only. Returns 0 when
 * there is no pad (XInput or SDL) on that port. */
int hitz_PadPeek(DWORD port, WORD *buttons, BYTE *lt, BYTE *rt,
                 SHORT *lx, SHORT *ly, SHORT *rx, SHORT *ry)
{
    XBOX_INPUT_STATE st;
    WORD b;
    if (port >= XBOX_MAX_CONTROLLERS || !pad_read(port, &st, NULL))
        return 0;
    b = st.Gamepad.wButtons & 0x00FF;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_A]) b |= 0x1000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_B]) b |= 0x2000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_X]) b |= 0x4000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y]) b |= 0x8000;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]) b |= 0x0100;
    if (st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]) b |= 0x0200;
    *buttons = b;
    *lt = st.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER];
    *rt = st.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    *lx = st.Gamepad.sThumbLX; *ly = st.Gamepad.sThumbLY;
    *rx = st.Gamepad.sThumbRX; *ry = st.Gamepad.sThumbRY;
    return 1;
}

/* Minimum rumble pulse on XInput, as on the SDL pads (RECOMP_RUMBLE_MIN_MS, default 250,
 * 0 turns it off): Hitz pulses both motors at full strength and stops them a few frames
 * later, which a pad barely feels. A stop that arrives sooner than the minimum after its
 * start is held until the minimum has passed; a new start cancels a held stop. */
static HANDLE g_stop_timer[XBOX_MAX_CONTROLLERS];
static volatile LONG g_stop_gen[XBOX_MAX_CONTROLLERS];
static ULONGLONG g_on_at[XBOX_MAX_CONTROLLERS];
static LONG g_stop_arg[XBOX_MAX_CONTROLLERS];

static VOID CALLBACK stop_cb(PVOID arg, BOOLEAN fired)
{
    DWORD port = (DWORD)(ULONG_PTR)arg;
    XINPUT_VIBRATION off = { 0, 0 };
    (void)fired;
    /* Only the stop that was scheduled last still counts. */
    if ((LONG)g_stop_arg[port] == g_stop_gen[port])
        xi_set(port, &off);
}

static int xi_min_ms(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_RUMBLE_MIN_MS"); v = e ? atoi(e) : 250; if (v < 0) v = 0; }
    return v;
}

static DWORD xi_rumble(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    ULONGLONG now = GetTickCount64();
    int min_ms = xi_min_ms();

    if (pVibration->wLeftMotorSpeed || pVibration->wRightMotorSpeed) {
        InterlockedIncrement(&g_stop_gen[dwPort]);          /* a held stop no longer applies */
        g_on_at[dwPort] = now ? now : 1;
    } else if (min_ms > 0 && g_on_at[dwPort] && now - g_on_at[dwPort] < (ULONGLONG)min_ms) {
        DWORD wait = (DWORD)((ULONGLONG)min_ms - (now - g_on_at[dwPort]));
        g_stop_arg[dwPort] = InterlockedIncrement(&g_stop_gen[dwPort]);
        g_on_at[dwPort] = 0;
        if (g_stop_timer[dwPort]) {
            DeleteTimerQueueTimer(NULL, g_stop_timer[dwPort], NULL);
            g_stop_timer[dwPort] = NULL;
        }
        if (CreateTimerQueueTimer(&g_stop_timer[dwPort], NULL, stop_cb, (PVOID)(ULONG_PTR)dwPort,
                                  wait, 0, WT_EXECUTEONLYONCE))
            return ERROR_SUCCESS;                            /* the motors keep going until then */
    } else {
        g_on_at[dwPort] = 0;
    }
    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return xi_set(dwPort, &xi_vib);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    if (!g_xi_ok[dwPort] && sdlpad_slot_used((int)dwPort)) {
        int rc;
        EnterCriticalSection(&g_lock);
        rc = sdlpad_rumble(dwPort, pVibration);
        LeaveCriticalSection(&g_lock);
        return rc < 0 ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS;
    }

    return xi_rumble(dwPort, pVibration);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort] || sdlpad_slot_used((int)dwPort);
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    {
        int slot = xi_slot(dwPort);
        result = slot >= 0 ? XInputGetCapabilities((DWORD)slot, dwFlags, &xi_caps) : ERROR_DEVICE_NOT_CONNECTED;
    }
    if (result != ERROR_SUCCESS) {
        if (!sdlpad_slot_used((int)dwPort))
            return result;
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        pCaps->Type = 1;       /* XINPUT_DEVTYPE_GAMEPAD */
        pCaps->SubType = 1;    /* XINPUT_DEVSUBTYPE_GAMEPAD */
        return ERROR_SUCCESS;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}
