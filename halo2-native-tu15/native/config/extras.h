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

#define WM_APP_EXTRAS (WM_APP + 21)

#endif
