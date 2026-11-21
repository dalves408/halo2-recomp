/*
 * halo_ui.c -- see halo_ui.h. Plain GDI, so the same code draws Config.exe's
 * window and the game's F1 overlay.
 */
#include <stdio.h>
#include <string.h>

#include "halo_ui.h"

/* The manuals' palette. */
#define C_BG_TOP    RGB(14, 52, 104)
#define C_BG_MID    RGB(8, 33, 72)
#define C_BG_BOT    RGB(3, 13, 32)
#define C_STREAK    RGB(18, 58, 112)
#define C_BAND      RGB(16, 62, 122)
#define C_BAND_EDGE RGB(70, 140, 214)
#define C_PANEL     RGB(6, 24, 54)
#define C_EDGE      RGB(38, 92, 156)
#define C_LIT       RGB(20, 74, 140)
#define C_LIT_EDGE  RGB(130, 206, 255)
#define C_WHITE     RGB(242, 246, 251)
#define C_TEXT      RGB(210, 224, 240)
#define C_DIM       RGB(120, 150, 186)
#define C_CYAN      RGB(130, 206, 255)
#define C_YELLOW    RGB(236, 214, 96)
#define C_ORANGE    RGB(247, 148, 29)
#define C_LISTEN    RGB(70, 40, 6)

int ui_px(const UiFonts *f, int v) { return (int)(v * f->s + 0.5f); }

static int CALLBACK has_face_cb(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM l)
{
    (void)lf; (void)tm; (void)type;
    *(int *)l = 1;
    return 0;
}

static const char *face(void)
{
    static const char *pick;
    if (!pick) {
        LOGFONTA lf;
        HDC dc = GetDC(NULL);
        int found = 0;
        memset(&lf, 0, sizeof lf);
        lf.lfCharSet = DEFAULT_CHARSET;
        strcpy(lf.lfFaceName, "Bahnschrift");      /* Windows 10 and later: close to the manuals' type */
        EnumFontFamiliesExA(dc, &lf, has_face_cb, (LPARAM)&found, 0);
        ReleaseDC(NULL, dc);
        pick = found ? "Bahnschrift" : "Segoe UI";
    }
    return pick;
}

static HFONT mkfont(int px, int weight)
{
    return CreateFontA(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0,
                       CLEARTYPE_QUALITY, 0, face());
}

void ui_fonts_make(UiFonts *f, float scale)
{
    if (scale < 0.5f) scale = 0.5f;
    f->s = scale;
    f->title = mkfont(ui_px(f, 26), FW_BOLD);
    f->head = mkfont(ui_px(f, 17), FW_BOLD);
    f->body = mkfont(ui_px(f, 14), FW_NORMAL);
    f->fine = mkfont(ui_px(f, 12), FW_NORMAL);
    f->key = mkfont(ui_px(f, 12), FW_SEMIBOLD);
}

void ui_fonts_free(UiFonts *f)
{
    DeleteObject(f->title); DeleteObject(f->head); DeleteObject(f->body);
    DeleteObject(f->fine); DeleteObject(f->key);
    memset(f, 0, sizeof *f);
}

static COLORREF lerp(COLORREF a, COLORREF b, int t, int n)
{
    if (n <= 0) return a;
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / n,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / n,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / n);
}

void ui_background(HDC dc, const RECT *r)
{
    int h = r->bottom - r->top, w = r->right - r->left, y, mid = h * 2 / 5;
    HPEN pen, old;
    for (y = 0; y < h; y += 2) {
        RECT b;
        HBRUSH br = CreateSolidBrush(y < mid ? lerp(C_BG_TOP, C_BG_MID, y, mid)
                                             : lerp(C_BG_MID, C_BG_BOT, y - mid, h - mid));
        SetRect(&b, r->left, r->top + y, r->right, r->top + y + 2);
        FillRect(dc, &b, br);
        DeleteObject(br);
    }
    /* The faint sweeping arcs behind the manuals' text. */
    pen = CreatePen(PS_SOLID, 2, C_STREAK);
    old = (HPEN)SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Arc(dc, r->left - w / 3, r->top + h / 5, r->left + w * 4 / 3, r->top + h * 2,
        r->right, r->top + h / 3, r->left, r->top + h / 2);
    Arc(dc, r->left - w / 2, r->top + h / 3, r->left + w * 5 / 4, r->top + h * 9 / 4,
        r->right, r->top + h / 2, r->left, r->top + h * 3 / 4);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void ui_chamfer(HDC dc, const RECT *r, int c, COLORREF fill, COLORREF border)
{
    POINT p[6];
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    p[0].x = r->left + c;  p[0].y = r->top;
    p[1].x = r->right - 1; p[1].y = r->top;
    p[2].x = r->right - 1; p[2].y = r->bottom - 1 - c;
    p[3].x = r->right - 1 - c; p[3].y = r->bottom - 1;
    p[4].x = r->left;      p[4].y = r->bottom - 1;
    p[5].x = r->left;      p[5].y = r->top + c;
    Polygon(dc, p, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void ui_text(HDC dc, HFONT font, COLORREF color, const RECT *r, const char *s, UINT fmt)
{
    RECT t = *r;
    HGDIOBJ o = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextA(dc, s, -1, &t, fmt | DT_NOPREFIX);
    SelectObject(dc, o);
}

static int text_w(HDC dc, HFONT font, const char *s)
{
    SIZE sz;
    HGDIOBJ o = SelectObject(dc, font);
    GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

static void hline(HDC dc, int x0, int x1, int y, COLORREF c, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ o = SelectObject(dc, pen);
    MoveToEx(dc, x0, y, NULL);
    LineTo(dc, x1, y);
    SelectObject(dc, o);
    DeleteObject(pen);
}

static void square(HDC dc, int x, int y, int s, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    SetRect(&r, x, y, x + s, y + s);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void ui_header(HDC dc, const UiFonts *f, const RECT *band, const char *title,
               const char *const *tabs, int ntabs, int active, int tabs_focused,
               int legend, RECT *tab_rects)
{
    RECT r = *band, t;
    int x, i, pad = ui_px(f, 18);
    ui_chamfer(dc, &r, ui_px(f, 22), C_BAND, C_BAND_EDGE);
    hline(dc, r.left + ui_px(f, 22), r.right, r.bottom + ui_px(f, 3), C_EDGE, 1);
    SetRect(&t, r.left + pad + ui_px(f, 8), r.top, r.right, r.bottom);
    ui_text(dc, f->title, C_WHITE, &t, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x = t.left + text_w(dc, f->title, title) + ui_px(f, 34);
    for (i = 0; i < ntabs; i++) {
        int w = text_w(dc, f->head, tabs[i]) + ui_px(f, 36);
        RECT tr;
        SetRect(&tr, x, r.top + ui_px(f, 12), x + w, r.bottom - ui_px(f, 12));
        if (i == active)
            ui_chamfer(dc, &tr, ui_px(f, 8), C_LIT, tabs_focused ? C_LIT_EDGE : C_BAND_EDGE);
        ui_text(dc, f->head, i == active ? C_WHITE : C_DIM, &tr, tabs[i],
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (tab_rects) tab_rects[i] = tr;
        x += w + ui_px(f, 6);
    }
    if (legend) {
        int s = ui_px(f, 10), y0 = r.top + (r.bottom - r.top) / 2 - ui_px(f, 15), lx = r.right - ui_px(f, 130);
        RECT lr;
        square(dc, lx, y0 + ui_px(f, 4), s, C_YELLOW);
        SetRect(&lr, lx + s + ui_px(f, 8), y0, r.right, y0 + ui_px(f, 18));
        ui_text(dc, f->fine, C_YELLOW, &lr, "Single wield", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        square(dc, lx, y0 + ui_px(f, 22), s, C_ORANGE);
        SetRect(&lr, lx + s + ui_px(f, 8), y0 + ui_px(f, 18), r.right, y0 + ui_px(f, 36));
        ui_text(dc, f->fine, C_ORANGE, &lr, "Dual wield", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void tri(HDC dc, int cx, int cy, int s, int dir, COLORREF c)
{
    POINT p[3];
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    p[0].x = cx + dir * s; p[0].y = cy;
    p[1].x = cx - dir * s; p[1].y = cy - s;
    p[2].x = cx - dir * s; p[2].y = cy + s;
    Polygon(dc, p, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pen);
}

static void spinner(HDC dc, const UiFonts *f, const RECT *box, const char *value, int lit,
                    RECT *left, RECT *right)
{
    RECT l, rr, v;
    int a = box->bottom - box->top;
    ui_chamfer(dc, box, ui_px(f, 6), lit ? C_LIT : C_PANEL, lit ? C_LIT_EDGE : C_EDGE);
    SetRect(&l, box->left, box->top, box->left + a, box->bottom);
    SetRect(&rr, box->right - a, box->top, box->right, box->bottom);
    tri(dc, (l.left + l.right) / 2, (l.top + l.bottom) / 2, ui_px(f, 4), -1, lit ? C_CYAN : C_DIM);
    tri(dc, (rr.left + rr.right) / 2, (rr.top + rr.bottom) / 2, ui_px(f, 4), 1, lit ? C_CYAN : C_DIM);
    SetRect(&v, l.right, box->top, rr.left, box->bottom);
    ui_text(dc, f->key, C_WHITE, &v, value, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (left) *left = l;
    if (right) *right = rr;
}

void ui_option_row(HDC dc, const UiFonts *f, const RECT *row, const char *label, const char *value,
                   int selected, int hover, RECT *left, RECT *right)
{
    RECT lr = *row, box;
    int bw = (row->right - row->left) * 45 / 100;
    if (selected || hover) {
        RECT hl = *row;
        ui_chamfer(dc, &hl, ui_px(f, 6), selected ? RGB(10, 40, 84) : RGB(8, 32, 68),
                   selected ? C_EDGE : RGB(8, 32, 68));
    }
    lr.left += ui_px(f, 12);
    lr.right = row->right - bw - ui_px(f, 8);
    ui_text(dc, f->body, selected ? C_WHITE : C_TEXT, &lr, label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetRect(&box, row->right - bw, row->top + ui_px(f, 2), row->right - ui_px(f, 4), row->bottom - ui_px(f, 2));
    spinner(dc, f, &box, value, selected || hover, left, right);
}

void ui_button(HDC dc, const UiFonts *f, const RECT *r, const char *text, int selected, int hover)
{
    ui_chamfer(dc, r, ui_px(f, 8), selected || hover ? C_LIT : C_PANEL,
               selected ? C_LIT_EDGE : (hover ? C_CYAN : C_EDGE));
    ui_text(dc, f->head, selected || hover ? C_WHITE : C_TEXT, r, text,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void ui_section(HDC dc, const UiFonts *f, const RECT *row, const char *text)
{
    RECT r = *row;
    int w;
    r.left += ui_px(f, 4);
    ui_text(dc, f->head, C_WHITE, &r, text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    w = text_w(dc, f->head, text);
    hline(dc, r.left + w + ui_px(f, 10), row->right, r.bottom - ui_px(f, 7), C_EDGE, 1);
}

void ui_update_banner(HDC dc, const UiFonts *f, const RECT *r, const char *text, int selected, int hover,
                      RECT *button)
{
    RECT t = *r, b;
    int bw = ui_px(f, 110);
    ui_chamfer(dc, r, ui_px(f, 8), RGB(58, 34, 6), C_ORANGE);
    SetRect(&b, r->right - bw - ui_px(f, 5), r->top + ui_px(f, 5), r->right - ui_px(f, 5), r->bottom - ui_px(f, 5));
    ui_chamfer(dc, &b, ui_px(f, 6), selected || hover ? C_ORANGE : RGB(150, 84, 10),
               selected ? C_WHITE : C_ORANGE);
    ui_text(dc, f->head, selected || hover ? RGB(30, 16, 0) : C_WHITE, &b, "UPDATE",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    t.left += ui_px(f, 14);
    t.right = b.left - ui_px(f, 8);
    ui_text(dc, f->body, RGB(255, 214, 150), &t, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (button) *button = b;
}

/* ---- the controls page ---------------------------------------------------- */

int ui_controls_rows(void) { return kb_action_count + UI_MOUSE_ROWS; }

static void key_slot(HDC dc, const UiFonts *f, const RECT *r, const char *name, int sel, int listen, int hover)
{
    char show[32];
    COLORREF fill = listen ? C_LISTEN : (sel ? C_LIT : (hover ? RGB(10, 38, 80) : C_PANEL));
    COLORREF edge = listen ? C_ORANGE : (sel ? C_LIT_EDGE : (hover ? C_CYAN : C_EDGE));
    ui_chamfer(dc, r, ui_px(f, 5), fill, edge);
    if (listen) {
        ui_text(dc, f->fine, C_ORANGE, r, "Press a key", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (!name) {
        ui_text(dc, f->fine, C_DIM, r, "-", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    kb_display(name, show, sizeof show);
    ui_text(dc, f->key, C_WHITE, r, show, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void action_label(HDC dc, const UiFonts *f, const RECT *r, const KbAction *a, int sel)
{
    RECT t = *r;
    int w;
    t.left += ui_px(f, 12);
    ui_text(dc, f->body, a->tone == 1 ? C_YELLOW : (sel ? C_WHITE : C_TEXT), &t, a->label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!a->dual) return;
    w = text_w(dc, f->body, a->label);
    t.left += w + ui_px(f, 8);
    ui_text(dc, f->fine, C_ORANGE, &t, a->dual, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

int ui_controls(HDC dc, const UiFonts *f, const RECT *area, const UiControls *c, UiHit *hits, int max)
{
    /* Left column: Movement, Look, Combat. Right column: Actions, Menus, Mouse. */
    int split = 14, nh = 0, col, gap = ui_px(f, 28);
    int cw = (area->right - area->left - gap) / 2;
    int rows_l = split + 3, rows_r = (kb_action_count - split) + UI_MOUSE_ROWS + 3;
    int units = rows_l > rows_r ? rows_l : rows_r;
    int rh = (area->bottom - area->top) / units;
    if (rh > ui_px(f, 30)) rh = ui_px(f, 30);

    for (col = 0; col < 2; col++) {
        int x0 = area->left + col * (cw + gap), x1 = x0 + cw, y = area->top;
        int first = col ? split : 0, last = col ? kb_action_count : split, i;
        int labw = cw * 44 / 100, sw = (cw - labw - ui_px(f, 4) * KB_SLOTS) / KB_SLOTS;
        for (i = first; i < last; i++) {
            const KbAction *a = &kb_actions[i];
            RECT row;
            int s, sel = c->sel_row == i;
            if (a->section) {
                RECT hr;
                SetRect(&hr, x0, y, x1, y + rh);
                ui_section(dc, f, &hr, a->section);
                y += rh;
            }
            SetRect(&row, x0, y, x1, y + rh - ui_px(f, 3));
            if (sel || c->hover_row == i)
                ui_chamfer(dc, &row, ui_px(f, 6), sel ? RGB(10, 40, 84) : RGB(8, 32, 68),
                           sel ? C_EDGE : RGB(8, 32, 68));
            {
                RECT lr = row;
                lr.right = x0 + labw;
                action_label(dc, f, &lr, a, sel);
            }
            for (s = 0; s < KB_SLOTS; s++) {
                RECT sr;
                int sx = x0 + labw + s * (sw + ui_px(f, 4));
                SetRect(&sr, sx, row.top + ui_px(f, 2), sx + sw, row.bottom - ui_px(f, 2));
                key_slot(dc, f, &sr, kb_slot(c->map, i, s), sel && c->sel_col == s,
                         sel && c->sel_col == s && c->listening, c->hover_row == i && c->hover_col == s);
                if (hits && nh < max) { hits[nh].row = i; hits[nh].col = s; hits[nh].r = sr; nh++; }
            }
            y += rh;
        }
        if (col == 1) {
            RECT hr;
            SetRect(&hr, x0, y, x1, y + rh);
            ui_section(dc, f, &hr, "Mouse");
            y += rh;
            for (i = 0; i < UI_MOUSE_ROWS; i++) {
                RECT row, l, r;
                int id = kb_action_count + i;
                SetRect(&row, x0, y, x1, y + rh - ui_px(f, 3));
                ui_option_row(dc, f, &row, c->opt_label[i] ? c->opt_label[i] : "",
                              c->opt_val[i] ? c->opt_val[i] : "", c->sel_row == id,
                              c->hover_row == id, &l, &r);
                if (hits && nh + 1 < max) {
                    hits[nh].row = id; hits[nh].col = UI_HIT_PREV; hits[nh].r = l; nh++;
                    hits[nh].row = id; hits[nh].col = UI_HIT_NEXT; hits[nh].r = r; nh++;
                }
                y += rh;
            }
        }
    }
    return nh;
}
