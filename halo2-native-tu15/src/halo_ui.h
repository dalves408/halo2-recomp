/*
 * halo_ui.h -- GDI drawing in the look of the Halo 2 manuals (deep blue
 * gradient, chamfered panels, white headings, yellow single-wield and orange
 * dual-wield notes), shared by Config.exe and the in-game F1 menu.
 */
#ifndef H2_HALO_UI_H
#define H2_HALO_UI_H

#include <windows.h>
#include "keybinds.h"

typedef struct {
    float s;                         /* scale: 1.0 at a 960x640 layout */
    HFONT title, head, body, fine, key;
} UiFonts;

void ui_fonts_make(UiFonts *f, float scale);
void ui_fonts_free(UiFonts *f);
int  ui_px(const UiFonts *f, int v);

void ui_background(HDC dc, const RECT *r);
void ui_chamfer(HDC dc, const RECT *r, int c, COLORREF fill, COLORREF border);
void ui_text(HDC dc, HFONT font, COLORREF color, const RECT *r, const char *s, UINT fmt);

/* The title band: title at the left, tabs after it, the wield legend at the
 * right (legend != 0). tab_rects gets each tab's rectangle. */
void ui_header(HDC dc, const UiFonts *f, const RECT *band, const char *title,
               const char *const *tabs, int ntabs, int active, int tabs_focused,
               int legend, RECT *tab_rects);

/* A setting: label, then "<  value  >". */
void ui_option_row(HDC dc, const UiFonts *f, const RECT *row, const char *label, const char *value,
                   int selected, int hover, RECT *left, RECT *right);
void ui_button(HDC dc, const UiFonts *f, const RECT *r, const char *text, int selected, int hover);
void ui_section(HDC dc, const UiFonts *f, const RECT *row, const char *text);
/* The "new version" strip: highlighted text and an UPDATE button (its
 * rectangle in *button). */
void ui_update_banner(HDC dc, const UiFonts *f, const RECT *r, const char *text, int selected, int hover,
                      RECT *button);

/* The controls page. Rows 0..kb_action_count-1 are actions (columns 0..2 are
 * key slots); the next UI_MOUSE_ROWS are the mouse settings. */
#define UI_MOUSE_ROWS 3
#define UI_HIT_PREV  (-1)            /* a setting's "<" */
#define UI_HIT_NEXT  (-2)            /* a setting's ">" */
typedef struct { int row, col; RECT r; } UiHit;
typedef struct {
    const KbMap *map;
    int sel_row, sel_col;            /* -1: nothing selected */
    int listening;                   /* the selected slot waits for a key */
    int hover_row, hover_col;
    const char *opt_label[UI_MOUSE_ROWS];
    const char *opt_val[UI_MOUSE_ROWS];
} UiControls;
int ui_controls_rows(void);
int ui_controls(HDC dc, const UiFonts *f, const RECT *area, const UiControls *c, UiHit *hits, int max);

#endif
