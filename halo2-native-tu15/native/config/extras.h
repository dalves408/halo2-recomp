/* extras.h: config.exe's EXTRAS page -- the Multiplayer Map Pack disc's maps and bonus videos. */
#ifndef H2_EXTRAS_H
#define H2_EXTRAS_H

#include <windows.h>
#include "halo_ui.h"

/* root: the install folder. owner: config.exe's window (gets WM_APP_EXTRAS while a job runs). */
void extras_init(HWND owner, const char *root);
void extras_draw(HDC dc, const UiFonts *f, const RECT *area);
/* 1 when the pointer is over something new (repaint) */
int  extras_hover(int x, int y);
/* 1 when the click was the page's */
int  extras_click(HWND owner, int x, int y);
/* the footer's status line while a job runs or after it ("" otherwise) */
const char *extras_status(void);
/* 1 while copying or re-encoding; extras_cancel asks the job to stop and waits for it */
int  extras_busy(void);
void extras_cancel(void);

/* The game's F1 menu: the same page without ADD MAP PACK DISC and re-encoding (config.exe keeps those). */
void extras_init_game(HWND game, const char *root);
/* Reads again what is installed (extras_init does it). */
void extras_refresh(void);
/* The next installed video from cur in dir (+1 / -1), else cur when installed, else the first; -1 = none. */
int  extras_next_video(int cur, int dir);
int  extras_selected(void);
void extras_select(int i);

/* The player plays in the page's own window: while extras_playing(), the window draws extras_draw_player over its
 * whole client area and passes its keys to extras_key (Space pause, S subtitles, Esc close; config.exe also F11 and
 * Enter full screen). A video that ends closes by itself (config.exe gets WM_APP_EXTRAS). */
void extras_play(int i);
int  extras_playing(void);
void extras_stop(void);
void extras_draw_player(HDC dc, const RECT *area);
int  extras_key(int vk);
void extras_player_mouse(int dblclick);

#define WM_APP_EXTRAS (WM_APP + 21)

#endif
