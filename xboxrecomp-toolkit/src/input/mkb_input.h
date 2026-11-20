/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, WASD drives movement, mouse
 * buttons drive the triggers -- regardless of which path the title
 * takes to read its pad (the emulated USB device or the kernel's
 * XInput ordinals), because the merge happens in xbox_InputGetState,
 * which both go through.
 *
 * All knobs are env vars (and an optional mkb.ini), so no rebuild is
 * needed to tune the feel:
 *
 *   RECOMP_MKB=0|1        master switch            (default off, opt-in)
 *   RECOMP_MKB_SENS=f     mouse sensitivity        (default 1.0)
 *   RECOMP_MKB_FULL=n     mouse counts for a full  (default 8)
 *                         stick throw, per poll
 *   RECOMP_MKB_DZ=f       assumed game deadzone    (default 0.18)
 *   RECOMP_MKB_ACC=f      aim acceleration         (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1 invert vertical aim      (default off)
 *   RECOMP_MKB_INI=path   config file (same keys, one per line)
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window, so the mouse keeps working normally in other apps.
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0", or mkb_set_enabled). Cheap. */
int mkb_enabled(void);

/* For a title's own settings, at any time:
 *   mkb_set_enabled   turn the layer on or off (starts its input thread once);
 *   mkb_set_mouse_only  1: only the mouse (aim, buttons, wheel); the keyboard is
 *                     left to the key map (keyboard.c) instead of this layer's
 *                     fixed layout;
 *   mkb_set_aim       sensitivity multiplier and vertical inversion. */
void mkb_set_enabled(int on);
void mkb_set_mouse_only(int on);
void mkb_set_aim(double sens, int invert_y);

/* Mouse aim through the title's own view angles instead of the stick:
 *   mkb_set_mouse_stick(0)  the mouse no longer deflects the aim stick;
 *   mkb_take_mouse          the raw counts since the last call (x right, y
 *                           down), 0 when there are none or the layer is off,
 *                           released (F9) or the game is not in front. */
void mkb_set_mouse_stick(int on);
int  mkb_take_mouse(long *dx, long *dy);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and the aim
 * stick is taken over by the mouse whenever the mouse moved since the
 * last poll. Bumps dwPacketNumber when the virtual pad contributed
 * anything, so a title watching for edges sees the presses.
 *
 * Returns 1 when the virtual pad is active and produced a usable
 * state (so a caller treating "no host pad" as an error can still
 * report a connected controller), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */
