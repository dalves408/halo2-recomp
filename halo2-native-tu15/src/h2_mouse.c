/*
 * h2_mouse.c -- mouse aim for Halo 2, and keeping it off Xbox Live.
 *
 * Hand-written. Generated code untouched apart from entry-hook calls
 * (analysis/apply-gen-patches.py), no game state written but the two
 * arguments below.
 *
 * Mouse aim is the toolkit's mouse layer (input/mkb_input.c) in its
 * mouse-only mode: raw mouse movement turns the player directly, and the
 * buttons and wheel are keys in the key map (keymap.ini: mouse1..mouse5,
 * mwheelup, mwheeldown), counted only while mouse aim is on. It is a setting
 * ([input] mouse_aim in SaveData\settings.ini, off by default; the F1 menu),
 * with a sensitivity and an inversion.
 *
 * Turning: the game's per-player control update (title update 1.5
 * 0x001898A0) turns each local player by the look stick's yaw and pitch
 * deltas (radians) through 0x0018A300(yaw_delta, pitch_delta), the local
 * player in eax, the two deltas on the stack (callee pops). That function owns the facing: it wraps the yaw, applies a
 * vehicle seat's turn limits and pins the pitch to the unit's bounds. So the
 * mouse is added to its two arguments, at its entry, when the control update
 * calls it (return address 0x00189BF3) for local player 0; recoil and other
 * callers (the weapon code's call returns to 0x001088E1) are left alone.
 * Matched by code bytes to the retail function the Halo 2 decompilation
 * documents; the TU 1.5 function is the same 1,323 bytes.
 *
 * Xbox Live on Insignia is shared with players on Xboxes and xemu, so mouse
 * aim does not go there. The game signs in to Xbox Live on its own at start,
 * so signing in is left alone; instead the mouse turns off -- aim, buttons,
 * menu clicks, the captured pointer -- once the player picks XBOX LIVE in the
 * main menu, and back on when they pick Campaign, Split Screen or System
 * Link. Entry hooks on those four menu choices (TU 1.5 0x0023737C, 0x00237324,
 * 0x00237488, 0x00237500, matched by code bytes to the retail functions the
 * decompilation documents) call h2_mouse_live_menu. As a backstop, an Xbox
 * Live match's traffic (nat_live_match_recent: a game joined from an invite
 * without the menu) turns it off too.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen/recomp_types.h"
#include "settings.h"
#include "overlay.h"

/* toolkit: input/mkb_input.h, input/keyboard.h, kernel/nvnet_nat.h */
extern void mkb_set_enabled(int on);
extern void mkb_set_mouse_only(int on);
extern void mkb_set_mouse_stick(int on);
extern int  mkb_take_mouse(long *dx, long *dy);
extern void mkb_set_aim(double sens, int invert_y);
extern void xbox_InputSetMouseKeys(int on);
extern int  xbox_FramebufferKeyDown(int vk);
extern int  nat_live_match_recent(unsigned ms);

#define TURN_FROM_CONTROL_UPDATE 0x00189BF3u    /* return address of the look-stick call */
/* One count turns 0.066 degrees at 100% (Half-Life's 0.022 x 3, as
 * CounterStrikeXB): 800 counts per inch is about 53 degrees per inch. */
#define RAD_PER_COUNT (0.066f * 3.14159265f / 180.0f)

static volatile LONG s_on, s_inited;
static volatile LONG s_live_menu;   /* the player went into the Xbox Live menus */
static volatile LONG s_active;      /* mouse aim working now: on, and not in Xbox Live */
static DWORD s_last_turn;
static volatile LONG s_play_tick;   /* GetTickCount of the last control update: in play */

/* In play: the control update turned a player in the last 300 ms (it runs
 * every frame while anyone controls a unit). Menus, intros, cutscenes and
 * the pause menu are not. */
static int in_play(void) { return GetTickCount() - (DWORD)s_play_tick < 300; }

/* Called by the pad poll (overrides/xinput_device.c) for port 0, before the
 * keyboard is read (begin) and after the state is complete (end). With mouse
 * aim on, in play the mouse buttons and wheel are the key map's; outside play
 * they are not, and left click is START and right click is B, to get through
 * the intros and menus. */
static void refresh(void)
{
    int live = s_live_menu || nat_live_match_recent(10000);
    int active = s_on && !live;
    static int was_live;                         /* the mouse is off because of Xbox Live */
    if (InterlockedExchange(&s_active, active) != active) {
        mkb_set_enabled(active);                 /* off: the pointer is given back */
        if (s_on && live && !was_live)
            fprintf(stderr, "[H2-MOUSE] mouse aim off in Xbox Live (controller only there)\n");
        else if (active && was_live)
            fprintf(stderr, "[H2-MOUSE] mouse aim back on (left Xbox Live)\n");
    }
    was_live = s_on && live;
    xbox_InputSetMouseKeys(active && in_play());
}

void h2_mouse_pad_begin(void)
{
    if (s_inited)
        refresh();
}

/* Main menu choices (entry hooks, analysis/apply-gen-patches.py): 1 XBOX LIVE,
 * 0 Campaign, Split Screen or System Link. */
void h2_mouse_live_menu(int live)
{
    InterlockedExchange(&s_live_menu, live != 0);
}

void h2_mouse_pad_end(unsigned short *buttons, unsigned char *analog)
{
    if (!s_active || in_play())
        return;
    if (xbox_FramebufferKeyDown(VK_LBUTTON)) *buttons |= 0x0010;      /* XBOX_GAMEPAD_START */
    if (xbox_FramebufferKeyDown(VK_RBUTTON)) analog[1] = 255;         /* XBOX_BUTTON_B */
}

static float getf(uint32_t va) { uint32_t u = MEM32(va); float f; memcpy(&f, &u, 4); return f; }
static void putf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

/* Entry hook of the generated sub_0018A300 (its first line). The local player
 * comes in eax (the TU 1.5 build passes it in a register: `mov ecx, eax;
 * imul ecx, 0x94`); the stack is the caller's: [esp] return address,
 * [esp+4] yaw delta, [esp+8] pitch delta. No register is changed. */
void sub_0018A300_enter(void)
{
    const Halo2Settings *s;
    long dx, dy;
    DWORD now;
    float scale;
    uint32_t esp = g_esp;

    if (MEM32(esp) != TURN_FROM_CONTROL_UPDATE)
        return;
    InterlockedExchange(&s_play_tick, (LONG)GetTickCount());   /* a player is in control */
    if (!s_active || g_eax != 0)
        return;
    now = GetTickCount();
    if (!mkb_take_mouse(&dx, &dy) || halo2_overlay_active()) {
        s_last_turn = now;
        return;
    }
    /* Movement made while not playing (menus, a cutscene, a pause) is not a
     * turn: drop what built up before the first control update after it. */
    if (now - s_last_turn > 250) {
        s_last_turn = now;
        return;
    }
    s_last_turn = now;
    s = halo2_settings_get();
    scale = RAD_PER_COUNT * (float)s->mouse_sens / 100.0f;
    /* Halo's yaw grows to the left and its pitch upwards; the mouse's x grows
     * to the right and its y downwards. */
    putf(esp + 4, getf(esp + 4) - (float)dx * scale);
    putf(esp + 8, getf(esp + 8) + (float)dy * scale * (s->mouse_invert ? 1.0f : -1.0f));
}

void h2_mouse_apply(void)
{
    const Halo2Settings *s = halo2_settings_get();
    int on = s->mouse_aim != 0;
    if (!s_inited)               /* the downloader, or before h2_mouse_init: not ours */
        return;
    mkb_set_aim(s->mouse_sens / 100.0, s->mouse_invert);
    if (InterlockedExchange(&s_on, on) != on)
        fprintf(stderr, "[H2-MOUSE] mouse aim %s (sensitivity %d%%%s)\n", on ? "on" : "off",
                s->mouse_sens, s->mouse_invert ? ", inverted" : "");
    refresh();
}

/* The F1 menu saved new mouse bindings to SaveData\keymap.ini: use that file
 * from now on and read it again on the next poll. */
void h2_mouse_binds_changed(void)
{
    extern void xbox_InputKeymapReload(void);
    char root[MAX_PATH], km[MAX_PATH];
    halo2_install_root(root, sizeof root);
    snprintf(km, sizeof km, "%s\\SaveData\\keymap.ini", root);
    _putenv_s("RECOMP_KEYMAP", km);
    xbox_InputKeymapReload();
}

void h2_mouse_init(void)
{
    mkb_set_mouse_only(1);       /* the keyboard stays the key map's */
    mkb_set_mouse_stick(0);      /* the mouse turns the player, not the stick */
    InterlockedExchange(&s_inited, 1);
    h2_mouse_apply();
}
