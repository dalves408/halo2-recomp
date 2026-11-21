/*
 * Insignia Setup.exe -- puts a player's own Xbox console identity and Xbox
 * Live (Insignia) account into this Halo 2 install.
 *
 * Xbox Live knows a console by its EEPROM and its hard-drive key, and keeps
 * the console's accounts in a hidden area of its drive (partition 0, sectors
 * 8 and up). The game reads, all in the install folder:
 *
 *   SaveData\Identity\eeprom.bin     the console's EEPROM
 *   SaveData\Identity\hddinfo.txt    "XboxHDKey: <32 hex digits>"
 *   SaveData\Identity\eepromkey.bin  the BIOS's EEPROM key (16 bytes): the game
 *                                    re-checks the identity with it at start
 *   SaveData\Identity\titlekeys.bin  Halo 2's per-title keys (LAN, signature,
 *                                    alternate), as the console's kernel makes them
 *   SaveData\DiskConfig.bin          the drive's config sectors 8-23 (accounts)
 *
 * Nothing goes to Insignia until the identity is verified against real
 * hardware: the player's own BIOS and MCPX boot ROM must unlock the EEPROM's
 * sealed section (only an EEPROM a real console wrote passes its HMAC check),
 * and the hard-drive key given must be the one sealed inside. Until then
 * Save and Recover stay disabled, and this program itself never opens a
 * network connection: Recover runs the game, whose router only lets a
 * verified identity out (console_id.c, nvnet_nat.c).
 *
 * An account comes from Insignia (Recover: the gamertag and the 16-digit
 * number on the insignia.live Web Dashboard; the game does the recovery,
 * h2_account.c) or from a dump of the console's drive, a raw drive image or an
 * xemu drive (.qcow2): sectors 9-19, accepted only when the hard-drive key
 * signed those accounts. Nothing secret is shown or logged: keys and the
 * recovery number are masked, the serial number shows its last digits only.
 */
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h2_layout.h"
#include "halo_ui.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "bcrypt.lib")

#define APP_TITLE "Halo 2 - Insignia Setup"

enum { ID_EE_EDIT = 100, ID_EE_BROWSE, ID_EE_STATUS, ID_BIOS, ID_BIOS_STATUS, ID_KEY_EDIT, ID_KEY_SHOW,
       ID_KEY_FILE, ID_KEY_STATUS, ID_ACC_EDIT, ID_ACC_BROWSE, ID_ACC_STATUS, ID_RECOVER, ID_SAVE,
       ID_CLOSE, ID_STATUS };

static HWND s_main, s_ee, s_ee_st, s_bios_st, s_key, s_key_st, s_acc, s_acc_st, s_status;
static HWND s_btn_save, s_btn_recover;
static char s_root[MAX_PATH];               /* the install folder */
static uint8_t s_eeprom[256];
static int s_ee_ok;
static int s_unlocked;                      /* the BIOS's EEPROM key opened this EEPROM */
static uint8_t s_eekey[16], s_hd[16];       /* that key, and the hard-drive key sealed in the EEPROM */
static uint8_t s_titlekeys[16 + 16 + 256];
static int s_have_titlekeys;
#define ACCOUNT_SECTORS 11                  /* 9 machine account, 10, 11 user directory, 12-19 user slots */
static uint8_t s_sectors[ACCOUNT_SECTORS][512];
static int s_acc_ok;
static char s_gamertags[256];
static int s_cli;

static void say(HWND w, const char *t) { SetWindowTextA(w, t); }

static void tell(const char *text, UINT icon)
{
    if (s_cli) { fprintf(stderr, "%s\n", text); return; }
    MessageBoxA(s_main, text, APP_TITLE, icon);
}

static void path_data(const char *rel, char *out, size_t n) { h2_data_path(s_root, rel, out, n); }
static void path_in(const char *rel, char *out, size_t n) { snprintf(out, n, "%s\\%s", s_root, rel); }

/* ---- look: the Halo 2 manuals' style, as Config.exe and the F1 menu (halo_ui.c) ------------- */
/* The standard controls stay (the key and recovery number are masked edit boxes); the window
 * paints the background, the title band and the panels behind them, and draws the buttons. */
#define SK_PANEL RGB(6, 24, 54)
#define SK_EDGE  RGB(38, 92, 156)
#define SK_EDIT  RGB(4, 16, 38)
typedef struct { RECT r; const char *title; } SkPanel;
typedef struct { SkPanel p[8]; int n; const char *title; int band_h; } Skin;
static Skin s_skin_main, s_skin_rec;
static UiFonts s_f;
static HBRUSH s_br_panel, s_br_edit;
static int s_show_key;                      /* the hard-drive key shown in clear */
static int s_yoff;                          /* the main window's controls sit below its title band */

static void skin_init(void)
{
    if (s_br_panel) return;
    ui_fonts_make(&s_f, 0.80f);
    s_br_panel = CreateSolidBrush(SK_PANEL);
    s_br_edit = CreateSolidBrush(SK_EDIT);
}

static void skin_panel(Skin *s, int x, int y, int w, int h, const char *title)
{
    if (s->n >= 8) return;
    SetRect(&s->p[s->n].r, x, y, x + w, y + h);
    s->p[s->n].title = title;
    s->n++;
}

static void skin_paint(HWND h, HDC out, const Skin *s)
{
    RECT cr, band;
    HDC dc;
    HBITMAP bm;
    HGDIOBJ old;
    int i;
    GetClientRect(h, &cr);
    dc = CreateCompatibleDC(out);
    bm = CreateCompatibleBitmap(out, cr.right, cr.bottom);
    old = SelectObject(dc, bm);
    ui_background(dc, &cr);
    if (s->title) {
        SetRect(&band, 8, 8, cr.right - 8, 8 + s->band_h);
        ui_header(dc, &s_f, &band, s->title, NULL, 0, 0, 0, 0, NULL);
    }
    for (i = 0; i < s->n; i++) {
        ui_chamfer(dc, &s->p[i].r, 12, SK_PANEL, SK_EDGE);
        if (s->p[i].title) {
            RECT t = s->p[i].r;
            t.left += 14; t.top += 2; t.bottom = t.top + 20;
            ui_text(dc, s_f.head, RGB(242, 246, 251), &t, s->p[i].title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    BitBlt(out, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
}

/* Status lines read green when a step passed, yellow while something is needed. */
static COLORREF skin_text_color(HWND c)
{
    int id = GetDlgCtrlID(c);
    char t[24];
    if (id != ID_EE_STATUS && id != ID_BIOS_STATUS && id != ID_KEY_STATUS && id != ID_ACC_STATUS && id != ID_STATUS)
        return RGB(210, 224, 240);
    GetWindowTextA(c, t, sizeof t);
    if (!strncmp(t, "OK", 2) || !strncmp(t, "Verified", 8) || !strncmp(t, "Saved", 5) ||
        !strncmp(t, "Identity saved", 14) || !strncmp(t, "Recovered", 9))
        return RGB(120, 222, 150);
    if (!strncmp(t, "This is NOT", 11) || !strncmp(t, "Not a", 5) || strstr(t, "failed") || strstr(t, "Failed"))
        return RGB(247, 148, 29);
    return RGB(236, 214, 96);
}

static LRESULT skin_ctlcolor(HDC dc, HWND c)
{
    char cls[16];
    GetClassNameA(c, cls, sizeof cls);
    if (!_stricmp(cls, "Edit")) {
        SetTextColor(dc, RGB(242, 246, 251));
        SetBkColor(dc, SK_EDIT);
        return (LRESULT)s_br_edit;
    }
    SetTextColor(dc, skin_text_color(c));
    SetBkColor(dc, SK_PANEL);
    return (LRESULT)s_br_panel;
}

static void skin_drawitem(const DRAWITEMSTRUCT *d)
{
    char t[64];
    RECT r = d->rcItem;
    int dis = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0;
    int foc = (d->itemState & ODS_FOCUS) != 0;
    GetWindowTextA(d->hwndItem, t, sizeof t);
    if (d->CtlID == ID_KEY_SHOW) strcpy_s(t, sizeof t, s_show_key ? "Hide" : "Show");
    FillRect(d->hDC, &r, s_br_panel);
    ui_chamfer(d->hDC, &r, 6, dis ? RGB(8, 22, 46) : down ? RGB(30, 96, 170) : RGB(16, 62, 122),
               dis ? RGB(30, 52, 84) : foc ? RGB(130, 206, 255) : RGB(70, 140, 214));
    ui_text(d->hDC, s_f.body, dis ? RGB(90, 110, 140) : RGB(242, 246, 251), &r, t,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static HWND skin_mk(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c;
    if (cls[0] == 'E') style |= WS_BORDER;
    if (cls[0] == 'B') style = (style & ~(DWORD)0xF) | BS_OWNERDRAW;
    c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                        (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)s_f.body, TRUE);
    return c;
}

/* The messages the look needs, for both windows; returns 1 when handled. */
static int skin_msg(HWND h, UINT m, WPARAM w, LPARAM l, const Skin *s, LRESULT *out)
{
    switch (m) {
    case WM_ERASEBKGND: skin_paint(h, (HDC)w, s); *out = 1; return 1;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: *out = skin_ctlcolor((HDC)w, (HWND)l); return 1;
    case WM_DRAWITEM: skin_drawitem((const DRAWITEMSTRUCT *)l); *out = TRUE; return 1;
    }
    return 0;
}

/* ---- crypto ----------------------------------------------------------------------------------- */
static BCRYPT_ALG_HANDLE s_hmac_alg, s_sha_alg;

static int hmac_sha1(const uint8_t *key, ULONG kl, const uint8_t *d, ULONG n, uint8_t out[20])
{
    if (!s_hmac_alg && BCryptOpenAlgorithmProvider(&s_hmac_alg, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))
        return 0;
    return BCryptHash(s_hmac_alg, (PUCHAR)key, kl, (PUCHAR)d, n, out, 20) == 0;
}

static int sha1(const uint8_t *d, ULONG n, uint8_t out[20])
{
    if (!s_sha_alg && BCryptOpenAlgorithmProvider(&s_sha_alg, BCRYPT_SHA1_ALGORITHM, NULL, 0))
        return 0;
    return BCryptHash(s_sha_alg, NULL, 0, (PUCHAR)d, n, out, 20) == 0;
}

static void rc4(const uint8_t *key, unsigned kl, uint8_t *d, unsigned n)
{
    uint8_t s[256], t;
    unsigned i, j = 0, k;
    for (i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) { j = (j + s[i] + key[i % kl]) & 255; t = s[i]; s[i] = s[j]; s[j] = t; }
    i = j = 0;
    for (k = 0; k < n; k++) {
        i = (i + 1) & 255; j = (j + s[i]) & 255;
        t = s[i]; s[i] = s[j]; s[j] = t;
        d[k] ^= s[(s[i] + s[j]) & 255];
    }
    SecureZeroMemory(s, sizeof s);
}

static int des3_cbc_decrypt(const uint8_t key[24], const uint8_t iv_in[8], uint8_t *d, ULONG n)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE k = NULL;
    uint8_t iv[8];
    ULONG got = 0;
    int ok = 0;
    memcpy(iv, iv_in, 8);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_3DES_ALGORITHM, NULL, 0) == 0 &&
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof BCRYPT_CHAIN_MODE_CBC, 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key, 24, 0) == 0)
        ok = BCryptDecrypt(k, d, n, NULL, iv, 8, d, n, &got, 0) == 0 && got == n;
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* ---- files ------------------------------------------------------------------------------------ */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024 || !(b = (uint8_t *)malloc((size_t)sz))) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

static int write_file(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int hex_key(const char *s, uint8_t out[16])
{
    int n = 0;
    for (; *s && n < 32; s++) {
        int v;
        if (*s == ' ' || *s == ':' || *s == '-') continue;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else return 0;
        if (n & 1) out[n / 2] |= (uint8_t)v; else out[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    while (*s == ' ' || *s == '\r' || *s == '\n') s++;
    return n == 32 && !*s;
}

/* The game's default.xbe (game_files\default.xbe; game\ in a development tree), or NULL. */
static uint8_t *read_xbe(size_t *n)
{
    char path[MAX_PATH];
    uint8_t *x;
    snprintf(path, sizeof path, "%s\\%s\\default.xbe", s_root, h2_game_name());
    if (!(x = read_file(path, n)) || *n < 0x200 || memcmp(x, "XBEH", 4)) { free(x); return NULL; }
    return x;
}

/* 16 bytes at virtual address va of the XBE. */
static int xbe_va(const uint8_t *x, size_t n, uint32_t va, uint8_t out[16])
{
    uint32_t base = rd32(x + 0x104), nsec = rd32(x + 0x11C), sec = rd32(x + 0x120) - base, i;
    for (i = 0; i < nsec && sec + (i + 1) * 0x38 <= n; i++) {
        const uint8_t *h = x + sec + i * 0x38;
        uint32_t v = rd32(h + 4), raw = rd32(h + 12), rs = rd32(h + 16);
        if (va >= v && va + 16 <= v + rs && raw + (va - v) + 16 <= n) {
            memcpy(out, x + raw + (va - v), 16);
            return 1;
        }
    }
    return 0;
}

/* ---- 1. the EEPROM ---------------------------------------------------------------------------- */
static uint32_t ee_sum(const uint8_t *p, unsigned n)
{
    uint32_t hi = 0, lo = 0;
    unsigned i;
    for (i = 0; i + 4 <= n; i += 4) {
        uint64_t s = ((uint64_t)hi << 32 | lo) + rd32(p + i);
        hi = (uint32_t)(s >> 32); lo = (uint32_t)s;
    }
    return ~(hi + lo);
}

static void update_state(void);

static void check_eeprom(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    char msg[160];
    s_ee_ok = 0;
    s_unlocked = 0;                          /* a new EEPROM needs the BIOS check again */
    SecureZeroMemory(s_hd, sizeof s_hd); SecureZeroMemory(s_eekey, sizeof s_eekey);
    s_have_titlekeys = 0;
    say(s_bios_st, "Needed: your console's BIOS and MCPX boot ROM, to prove the EEPROM is a real console's.");
    if (f) { n = fread(s_eeprom, 1, sizeof s_eeprom, f); fclose(f); }
    if (n != 256) { say(s_ee_st, "Not an Xbox EEPROM (it must be a 256-byte eeprom.bin)."); update_state(); return; }
    if (ee_sum(s_eeprom + 0x34, 0x2C) != rd32(s_eeprom + 0x30) || ee_sum(s_eeprom + 0x64, 0x5C) != rd32(s_eeprom + 0x60)) {
        say(s_ee_st, "This EEPROM's checksums are wrong: it is damaged or not an Xbox EEPROM.");
        update_state();
        return;
    }
    s_ee_ok = 1;
    snprintf(msg, sizeof msg, "Read: console serial number ending ...%.4s, network address %02X:%02X:%02X:.. "
             "(not verified yet)", (const char *)s_eeprom + 0x34 + 8, s_eeprom[0x40], s_eeprom[0x41], s_eeprom[0x42]);
    say(s_ee_st, msg);
    update_state();
}

/* ---- 2. the console's BIOS: proof the EEPROM is real, and its keys ----------------------------- */
/* The EEPROM's sealed section (hard-drive key, region) is RC4-encrypted and
 * HMAC-SHA1-signed with the "EEPROM key" that every console keeps in its
 * BIOS's second-stage loader (2BL), itself encrypted by the MCPX boot ROM:
 *   MCPX 1.0: RC4 with the ROM's key at 0x1A5 over the 0x6000-byte 2BL at
 *     flash 0xFFFF9E00;
 *   MCPX 1.1: RC4 with SHA-1(ROM key at 0x19C | flash key at 0xFFFFFDF0 |
 *     ROM key ^ 0x5C) over the 2BL up to 0xFFFFD400.
 * The EEPROM key is whichever 16 bytes of the decrypted 2BL reproduce the
 * EEPROM's own HMAC; the CERT key follows it. A forged or edited EEPROM, or
 * another console's BIOS, finds nothing. Player-supplied files only. */
static int try_eeprom_key(const uint8_t *k, uint8_t hd[16])
{
    uint8_t rk[20], sec[28], check[20];
    if (!hmac_sha1(k, 16, s_eeprom, 20, rk)) return 0;
    memcpy(sec, s_eeprom + 0x14, sizeof sec);
    rc4(rk, 20, sec, sizeof sec);
    if (!hmac_sha1(k, 16, sec, sizeof sec, check) || memcmp(check, s_eeprom, 20)) { SecureZeroMemory(sec, sizeof sec); return 0; }
    memcpy(hd, sec + 8, 16);
    SecureZeroMemory(sec, sizeof sec); SecureZeroMemory(rk, sizeof rk);
    return 1;
}

static unsigned search_2bl(const uint8_t *dec, unsigned n, uint8_t hd[16])
{
    unsigned p;
    for (p = 0; p + 32 <= n; p++)
        if (try_eeprom_key(dec + p, hd)) return p + 1;
    return 0;
}

/* Halo 2's title keys from the CERT key and the XBE's certificate, as the
 * kernel makes them when the game starts: HMAC-SHA1(CERT key, key)[0..15]. */
static int title_keys(const uint8_t cert_key[16])
{
    size_t n = 0;
    uint8_t *x = read_xbe(&n), d[20];
    uint32_t c;
    int i, ok = 0;
    if (!x) return 0;
    c = rd32(x + 0x118) - rd32(x + 0x104);
    if (c + 0x1D0 <= n) {
        ok = hmac_sha1(cert_key, 16, x + c + 0xB0, 16, d);
        memcpy(s_titlekeys, d, 16);
        ok = ok && hmac_sha1(cert_key, 16, x + c + 0xC0, 16, d);
        memcpy(s_titlekeys + 16, d, 16);
        for (i = 0; ok && i < 16; i++) {
            ok = hmac_sha1(cert_key, 16, x + c + 0xD0 + 16 * i, 16, d);
            memcpy(s_titlekeys + 32 + 16 * i, d, 16);
        }
    }
    SecureZeroMemory(d, sizeof d);
    free(x);
    return ok;
}

/* 0 unlocked; 1 files unreadable or the wrong size; 2 they do not unlock this EEPROM. */
static int unlock_with_bios(const char *bios_path, const char *mcpx_path, const char **rev)
{
    size_t bn = 0, mn = 0;
    uint8_t *bios = read_file(bios_path, &bn), *mcpx = read_file(mcpx_path, &mn);
    const uint8_t *im;
    uint8_t *dec = NULL, hd[16];
    int r = 1;
    unsigned at = 0, len = 0;
    if (!bios || !mcpx || bn < 0x40000 || mn != 512) goto out;
    im = bios + bn - 0x40000;                    /* flash is mirrored up to the top of memory */
    if (!(dec = (uint8_t *)malloc(0x6000))) goto out;
    r = 2;
    {   /* MCPX 1.1 */
        uint8_t blk[48], rk[20];
        int i;
        memcpy(blk, mcpx + 0x19C, 16);
        memcpy(blk + 16, im + 0x3FDF0, 16);
        for (i = 0; i < 16; i++) blk[32 + i] = (uint8_t)(mcpx[0x19C + i] ^ 0x5C);
        if (sha1(blk, sizeof blk, rk)) {
            memcpy(dec, im + 0x39E00, 0x3600);
            rc4(rk, 20, dec, 0x3600);
            if ((at = search_2bl(dec, 0x3600, hd)) != 0) { *rev = "1.1"; len = 0x3600; }
        }
        SecureZeroMemory(blk, sizeof blk); SecureZeroMemory(rk, sizeof rk);
    }
    if (!at) {  /* MCPX 1.0 */
        memcpy(dec, im + 0x39E00, 0x6000);
        rc4(mcpx + 0x1A5, 16, dec, 0x6000);
        if ((at = search_2bl(dec, 0x6000, hd)) != 0) { *rev = "1.0"; len = 0x6000; }
    }
    if (at && at - 1 + 32 <= len) {
        memcpy(s_eekey, dec + at - 1, 16);
        memcpy(s_hd, hd, 16);
        s_unlocked = 1;
        s_have_titlekeys = title_keys(dec + at - 1 + 16);
        r = 0;
    }
out:
    SecureZeroMemory(hd, sizeof hd);
    if (dec) { SecureZeroMemory(dec, 0x6000); free(dec); }
    if (bios) { SecureZeroMemory(bios, bn); free(bios); }
    if (mcpx) { SecureZeroMemory(mcpx, mn); free(mcpx); }
    return r;
}

/* A saved eepromkey.bin opens the EEPROM again: verified without the BIOS. */
static void unlock_with_saved_key(void)
{
    char path[MAX_PATH];
    size_t n = 0;
    uint8_t *k;
    path_data("Identity\\eepromkey.bin", path, sizeof path);
    if (!s_ee_ok || !(k = read_file(path, &n))) return;
    if (n == 16 && try_eeprom_key(k, s_hd)) {
        memcpy(s_eekey, k, 16);
        s_unlocked = 1;
        path_data("Identity\\titlekeys.bin", path, sizeof path);
        {
            size_t tn = 0;
            uint8_t *t = read_file(path, &tn);
            if (t && tn == sizeof s_titlekeys) { memcpy(s_titlekeys, t, tn); s_have_titlekeys = 1; }
            if (t) { SecureZeroMemory(t, tn); free(t); }
        }
    }
    SecureZeroMemory(k, n);
    free(k);
}

/* ---- 3. the hard-drive key -------------------------------------------------------------------- */
static int key_matches_accounts(const uint8_t hd[16]);

/* 1 verified (the key is the one sealed in this real console's EEPROM);
 * 0 not this EEPROM's key; -1 not checked yet (no BIOS); -2 not a key. */
static int key_state(void)
{
    char t[128];
    uint8_t k[16];
    int r;
    GetWindowTextA(s_key, t, sizeof t);
    if (!hex_key(t, k)) { SecureZeroMemory(t, sizeof t); return -2; }
    SecureZeroMemory(t, sizeof t);
    r = !s_unlocked ? -1 : memcmp(k, s_hd, 16) == 0;
    SecureZeroMemory(k, sizeof k);
    return r;
}

static int verified(void) { return s_ee_ok && s_unlocked && s_have_titlekeys && key_state() == 1; }

static void update_state(void)
{
    char t[128];
    int v;
    GetWindowTextA(s_key, t, sizeof t);
    if (!t[0]) say(s_key_st, "Needed: your console's hard-drive key (32 hex digits), or let your BIOS fill it in.");
    else switch (key_state()) {
    case -2: say(s_key_st, "Not a hard-drive key: it must be 32 hex digits (0-9, A-F)."); break;
    case -1: say(s_key_st, "Not checked yet: add your console's BIOS and MCPX ROM in step 2."); break;
    case 0:  say(s_key_st, "This is NOT the hard-drive key sealed in this EEPROM: it is another console's, or mistyped."); break;
    case 1:  say(s_key_st, "OK: verified. It is the hard-drive key sealed in your console's EEPROM."); break;
    }
    SecureZeroMemory(t, sizeof t);
    v = verified();
    EnableWindow(s_btn_save, v);
    EnableWindow(s_btn_recover, v);
    if (!v) {
        say(s_status, "Locked: nothing is saved or sent until steps 1-3 are verified.");
    } else {
        t[0] = 0;
        GetWindowTextA(s_status, t, sizeof t);
        if (!t[0] || strncmp(t, "Locked", 6) == 0)
            say(s_status, "Verified against your console's hardware. Save, then add your account.");
    }
}

static void bios_step(const char *bios_path, const char *mcpx_path)
{
    const char *rev = "";
    char t[128], msg[220];
    int i, r;
    if (!s_ee_ok) { say(s_bios_st, "Choose your console's EEPROM in step 1 first: the BIOS is checked against it."); return; }
    say(s_bios_st, "Checking...");
    UpdateWindow(s_bios_st);
    r = unlock_with_bios(bios_path, mcpx_path, &rev);
    if (r == 1) { say(s_bios_st, "Those are not a BIOS image (256 KB-1 MB) and an MCPX boot ROM (512 bytes)."); update_state(); return; }
    if (r == 2) {
        say(s_bios_st, "That BIOS and MCPX ROM do not unlock this EEPROM: it is not this console's BIOS, or the "
                       "EEPROM is not a real console's. Use MCPX 1.0 for an Xbox 1.0, MCPX 1.1 for 1.1-1.6.");
        update_state();
        return;
    }
    if (!s_have_titlekeys) {
        say(s_bios_st, "EEPROM verified, but Halo 2's title keys could not be made: game\\default.xbe is missing.");
        update_state();
        return;
    }
    snprintf(msg, sizeof msg, "OK: your BIOS (MCPX %s) unlocked this EEPROM: it was made by a real console. "
             "Halo 2's title keys are ready.", rev);
    say(s_bios_st, msg);
    GetWindowTextA(s_key, t, sizeof t);
    if (!t[0]) {                                  /* no key typed: the sealed one, masked */
        char hex[33];
        for (i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02X", s_hd[i]);
        SetWindowTextA(s_key, hex);
        SecureZeroMemory(hex, sizeof hex);
    }
    SecureZeroMemory(t, sizeof t);
    update_state();
}

/* EvoX/UnleashX hddinfo.txt: the "XboxHDKey" line (not "HDD Key", the drive's lock password). */
static void key_from_hddinfo(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512], out[40];
    int n = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "XboxHDKey");
        if (!p) continue;
        for (p += 9; *p && n < 32; p++)
            if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) out[n++] = *p;
        break;
    }
    fclose(f);
    out[n] = 0;
    if (n == 32) SetWindowTextA(s_key, out);
    else say(s_key_st, "No XboxHDKey line in that file.");
    SecureZeroMemory(out, sizeof out);
    SecureZeroMemory(line, sizeof line);
    update_state();
}

/* ---- 4. an account from a drive: raw, or xemu's qcow2 ------------------------------------------ */
static uint64_t be64(const uint8_t *p) { uint64_t v = 0; int i; for (i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int read_at(HANDLE f, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
}

static int read_sector(HANDLE f, uint64_t lba, uint8_t out[512])
{
    uint8_t h[48];
    if (!read_at(f, 0, h, sizeof h)) return 0;
    if (memcmp(h, "QFI\xfb", 4) == 0) {
        uint32_t cbits = be32(h + 20), l2bits = cbits - 3;
        uint64_t off = lba * 512, l1off = be64(h + 40), l1e, l2e, cl;
        uint32_t l1size = be32(h + 36);
        uint64_t l1i = off >> (cbits + l2bits), l2i = (off >> cbits) & ((1ull << l2bits) - 1);
        uint8_t e[8];
        if (be32(h + 32) != 0 || cbits < 9 || cbits > 21 || l1i >= l1size) return 0;
        if (!read_at(f, l1off + l1i * 8, e, 8)) return 0;
        l1e = be64(e) & 0x00FFFFFFFFFFFE00ull;
        if (!l1e) { memset(out, 0, 512); return 1; }
        if (!read_at(f, l1e + l2i * 8, e, 8)) return 0;
        l2e = be64(e);
        if (l2e & (1ull << 62)) return 0;
        cl = l2e & 0x00FFFFFFFFFFFE00ull;
        if (!cl) { memset(out, 0, 512); return 1; }
        return read_at(f, cl + (off & ((1ull << cbits) - 1)), out, 512);
    }
    return read_at(f, lba * 512, out, 512);
}

static int config_sector_ok(const uint8_t *s)
{
    uint8_t t[512];
    uint64_t sum = 0;
    uint32_t v;
    int i;
    if (rd32(s) != 0x79132568u || rd32(s + 0x1FC) != 0xAA550000u) return 0;
    memcpy(t, s, 512);
    memset(t + 0x1F8, 0, 4);
    for (i = 0; i < 512; i += 4) sum += rd32(t + i);
    v = (uint32_t)sum + (uint32_t)(sum >> 32);
    if (v < (uint32_t)sum) v++;
    return (~v) == rd32(s + 0x1F8);
}

/* XOnline signs each account record with the hard-drive key (h2_account.c):
 * the account key at +0x50 is 3DES-CBC encrypted (IV: the directory entry's
 * confounder; key HMAC-SHA1(K1, hd)[0..3] | HMAC-SHA1(K2, hd)), and +0x64 is
 * HMAC-SHA1(hd, record[0..0x63])[0..7] over the record in the clear. K1/K2
 * are read from the game's own default.xbe (title update 1.5 addresses).
 * 1 the key signed an account on the drive, 0 none, -1 nothing to check. */
#define XO_K1_VA   0x0057AB4Cu
#define XO_K2_VA   0x0057AB38u
#define REC_SIZE   0x6C
#define SECTOR_HDR 0x0C

static int key_matches_accounts(const uint8_t hd[16])
{
    uint8_t k1[16], k2[16], h1[20], h2[20], dk[24], rec[REC_SIZE], sig[20];
    const uint8_t *dir;
    size_t n = 0;
    uint8_t *x = read_xbe(&n);
    int i, tried = 0, match = 0, ok;
    ok = x && xbe_va(x, n, XO_K1_VA, k1) && xbe_va(x, n, XO_K2_VA, k2);
    free(x);
    if (!s_acc_ok || !ok || !hmac_sha1(k1, 16, hd, 16, h1) || !hmac_sha1(k2, 16, hd, 16, h2)) return -1;
    memcpy(dk, h1, 4); memcpy(dk + 4, h2, 20);
    dir = s_sectors[11 - 9] + SECTOR_HDR;
    for (i = 0; i < 8 && !match; i++) {
        const uint8_t *e = dir + 12 * i;
        if (!rd32(e)) continue;
        memcpy(rec, s_sectors[12 + i - 9] + SECTOR_HDR, REC_SIZE);
        tried = 1;
        if (des3_cbc_decrypt(dk, e + 4, rec + 0x50, 16) && hmac_sha1(hd, 16, rec, 0x64, sig) && !memcmp(sig, rec + 0x64, 8))
            match = 1;
    }
    SecureZeroMemory(dk, sizeof dk); SecureZeroMemory(h1, sizeof h1); SecureZeroMemory(h2, sizeof h2);
    SecureZeroMemory(rec, sizeof rec); SecureZeroMemory(k1, sizeof k1); SecureZeroMemory(k2, sizeof k2);
    return tried ? match : -1;
}

static void check_accounts(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    int i, valid = 0, users = 0;
    char msg[400];
    s_acc_ok = 0;
    s_gamertags[0] = 0;
    if (f == INVALID_HANDLE_VALUE) { say(s_acc_st, "That file could not be opened."); return; }
    for (i = 0; i < ACCOUNT_SECTORS; i++)
        if (!read_sector(f, 9 + (uint64_t)i, s_sectors[i])) { CloseHandle(f); say(s_acc_st, "That file is too short, or a drive image this program cannot read."); return; }
    CloseHandle(f);
    for (i = 0; i < ACCOUNT_SECTORS; i++) {
        const uint8_t *s = s_sectors[i];
        int k, empty = 1;
        for (k = 0; k < 512; k++) if (s[k]) { empty = 0; break; }
        if (empty) continue;
        if (!config_sector_ok(s)) { say(s_acc_st, "Those sectors are not an Xbox drive's account area (wrong file?)."); return; }
        valid++;
        if (i >= 3 && s[24] && memcmp(s + 24, "SN.", 3) != 0) {
            char tag[17];
            memcpy(tag, s + 24, 16); tag[16] = 0;
            for (k = 0; tag[k]; k++) if (tag[k] < 32 || tag[k] > 126) { tag[k] = 0; break; }
            if (tag[0]) {
                if (users++) strncat(s_gamertags, ", ", sizeof s_gamertags - strlen(s_gamertags) - 1);
                strncat(s_gamertags, tag, sizeof s_gamertags - strlen(s_gamertags) - 1);
            }
        }
    }
    if (!valid) { say(s_acc_st, "No Xbox Live accounts in that file (sectors 9-19 are empty)."); return; }
    s_acc_ok = 1;
    snprintf(msg, sizeof msg, users ? "Found account(s) %s; they are added when you Save." : "Found the machine account but no player accounts.",
             s_gamertags);
    say(s_acc_st, msg);
}

static int write_accounts(void)
{
    char path[MAX_PATH], bak[MAX_PATH];
    uint8_t cfg[16 * 512];
    FILE *f;
    int i;
    path_data("DiskConfig.bin", path, sizeof path);
    memset(cfg, 0, sizeof cfg);
    if ((f = fopen(path, "rb")) != NULL) {
        fread(cfg, 1, sizeof cfg, f);
        fclose(f);
        snprintf(bak, sizeof bak, "%s.before-insignia-setup", path);
        if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) CopyFileA(path, bak, TRUE);
    }
    for (i = 0; i < ACCOUNT_SECTORS; i++)
        memcpy(cfg + (1 + i) * 512, s_sectors[i], 512);
    return write_file(path, cfg, sizeof cfg);
}

/* ---- saving ----------------------------------------------------------------------------------- */
static int save_identity(void)
{
    char t[128], path[MAX_PATH], line[64];
    uint8_t k[16];
    int i, ok;
    if (!verified()) {
        tell("Not saved: the EEPROM and hard-drive key are not verified against your console's BIOS yet.\n\n"
             "Nothing is saved or sent to Insignia until they are.", MB_ICONWARNING);
        return 0;
    }
    GetWindowTextA(s_key, t, sizeof t);
    hex_key(t, k);
    SecureZeroMemory(t, sizeof t);
    path_data("", path, sizeof path); CreateDirectoryA(path, NULL);
    path_data("Identity", path, sizeof path); CreateDirectoryA(path, NULL);
    strcpy_s(line, sizeof line, "XboxHDKey: ");
    for (i = 0; i < 16; i++) sprintf(line + 11 + 2 * i, "%02X", k[i]);
    strcat_s(line, sizeof line, "\r\n");
    SecureZeroMemory(k, sizeof k);
    path_data("Identity\\eeprom.bin", path, sizeof path);
    ok = write_file(path, s_eeprom, 256);
    path_data("Identity\\hddinfo.txt", path, sizeof path);
    ok = ok && write_file(path, line, strlen(line));
    SecureZeroMemory(line, sizeof line);
    path_data("Identity\\eepromkey.bin", path, sizeof path);
    ok = ok && write_file(path, s_eekey, 16);
    path_data("Identity\\titlekeys.bin", path, sizeof path);
    ok = ok && write_file(path, s_titlekeys, sizeof s_titlekeys);
    if (!ok) { tell("Your identity could not be saved to the data folder (Identity).", MB_ICONERROR); return 0; }
    return 1;
}

static void save(void)
{
    uint8_t k[16];
    char t[128];
    if (!save_identity()) return;
    if (s_acc_ok) {
        int m;
        GetWindowTextA(s_key, t, sizeof t);
        hex_key(t, k);
        SecureZeroMemory(t, sizeof t);
        m = key_matches_accounts(k);
        SecureZeroMemory(k, sizeof k);
        if (m != 1) {
            tell(m == 0 ? "Your identity is saved, but the accounts on that drive were made on another console "
                          "(this hard-drive key did not sign them), so they were not added."
                        : "Your identity is saved, but the accounts on that drive could not be checked "
                          "(game\\default.xbe missing), so they were not added.", MB_ICONWARNING);
            say(s_status, "Identity saved. Accounts not added.");
            return;
        }
        if (!write_accounts()) { tell("The accounts could not be written to DiskConfig.bin in the data folder.", MB_ICONERROR); return; }
    }
    say(s_status, s_acc_ok ? "Saved. Start Halo 2 and sign in to Xbox Live."
                           : "Saved. Now add your account: Recover from Insignia...");
    tell(s_acc_ok ? "Your console identity and Xbox Live account are set up.\n\nStart Halo 2 and sign in to Xbox Live."
                  : "Your console identity is set up.\n\nTo add your Xbox Live account, use Recover from Insignia... "
                    "with your gamertag and the 16-digit number from your insignia.live Web Dashboard.",
         MB_ICONINFORMATION);
}

/* ---- Recover from Insignia ---------------------------------------------------------------------- */
enum { ID_REC_TAG = 300, ID_REC_NUM, ID_REC_OK, ID_REC_CANCEL };
static HWND s_rec, s_rec_tag, s_rec_num;
static int s_rec_done;

static void rec_digits_only(HWND e)
{
    static int busy;
    char t[64], d[17];
    int i, n = 0;
    if (busy) return;
    GetWindowTextA(e, t, sizeof t);
    for (i = 0; t[i] && n < 16; i++) if (t[i] >= '0' && t[i] <= '9') d[n++] = t[i];
    d[n] = 0;
    if (strcmp(t, d)) {
        busy = 1;
        SetWindowTextA(e, d);
        SendMessageA(e, EM_SETSEL, (WPARAM)n, (LPARAM)n);
        busy = 0;
    }
    SecureZeroMemory(t, sizeof t);
    SecureZeroMemory(d, sizeof d);
}

static LRESULT CALLBACK rec_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND && LOWORD(w) == ID_REC_NUM && HIWORD(w) == EN_CHANGE) { rec_digits_only((HWND)l); return 0; }
    if (m == WM_COMMAND && (LOWORD(w) == ID_REC_OK || LOWORD(w) == ID_REC_CANCEL)) { s_rec_done = LOWORD(w) == ID_REC_OK ? 1 : -1; return 0; }
    if (m == WM_CLOSE) { s_rec_done = -1; return 0; }
    {
        LRESULT r;
        if (skin_msg(h, m, w, l, &s_skin_rec, &r)) return r;
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND rec_mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    return skin_mk(s_rec, cls, text, style, x, y + 64, w, h, id);
}

static int ask_recover(char *tag, unsigned tagn, char *num, unsigned numn)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r = { 0, 0, 460, 262 + 64 };
    skin_init();
    s_skin_rec.n = 0;
    s_skin_rec.title = "RECOVER ACCOUNT";
    s_skin_rec.band_h = 48;
    skin_panel(&s_skin_rec, 6, 64, 448, 256, NULL);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = rec_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "Halo2Recover";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    s_rec = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName, "Recover your account from Insignia",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, s_main, NULL, wc.hInstance, NULL);
    rec_mk("STATIC", "Sign in to your Web Dashboard at insignia.live and press Reveal to see your account's "
           "16-digit number. Don't share it: it lets anyone recover your account.", SS_LEFT, 14, 10, 432, 44, 0);
    rec_mk("STATIC", "Gamertag:", SS_LEFT, 14, 66, 120, 20, 0);
    s_rec_tag = rec_mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, 140, 62, 306, 23, ID_REC_TAG);
    rec_mk("STATIC", "16-digit number:", SS_LEFT, 14, 102, 120, 20, 0);
    s_rec_num = rec_mk("EDIT", "", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 140, 98, 306, 23, ID_REC_NUM);
    rec_mk("STATIC", "Paste it as shown (0000-0000-0000-0000): the dashes are dropped.", SS_LEFT, 140, 126, 306, 32, 0);
    rec_mk("STATIC", "Recover starts Halo 2 in its window. About a minute later it signs this console in to "
           "Insignia, fetches your account and closes by itself.", SS_LEFT, 14, 164, 432, 44, 0);
    SendMessageA(s_rec_tag, EM_LIMITTEXT, 15, 0);
    SendMessageA(s_rec_num, EM_LIMITTEXT, 32, 0);
    rec_mk("BUTTON", "Recover", BS_DEFPUSHBUTTON | WS_TABSTOP, 252, 222, 94, 28, ID_REC_OK);
    rec_mk("BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 352, 222, 94, 28, ID_REC_CANCEL);
    EnableWindow(s_main, FALSE);
    ShowWindow(s_rec, SW_SHOW);
    SetFocus(s_rec_tag);
    s_rec_done = 0;
    while (!s_rec_done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { s_rec_done = 1; continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { s_rec_done = -1; continue; }
        if (IsDialogMessageA(s_rec, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    GetWindowTextA(s_rec_tag, tag, (int)tagn);
    GetWindowTextA(s_rec_num, num, (int)numn);
    SetWindowTextA(s_rec_num, "");
    EnableWindow(s_main, TRUE);
    DestroyWindow(s_rec);
    SetForegroundWindow(s_main);
    return s_rec_done == 1;
}

static const char *recover_result(DWORD code)
{
    switch (code) {
    case 0:  return NULL;
    case 10: return "The game did not receive the gamertag and number.";
    case 11: return "This console could not sign in to Insignia. Check that the console is registered with "
                    "Insignia and your internet connection.";
    case 12: return "Insignia did not answer in time. Check your internet connection and try again.";
    case 13: return "Insignia did not accept that gamertag and number. Check both on your insignia.live "
                    "Web Dashboard (the number changes if you reveal a new one).";
    case 14: return "The account could not be saved to DiskConfig.bin in the data folder.";
    case 15: return "All eight account slots on this console are in use.";
    default: return "The recovery did not finish. recovery_*.log in the data folder's logs says why.";
    }
}

/* Runs the game to recover the account. Only reached with a verified identity
 * that is saved: the game re-checks it with eepromkey.bin before its router
 * lets anything out. */
static void recover(void)
{
    char tag[32], num[40], exe[MAX_PATH], cmd[3 * MAX_PATH], line[96], v[MAX_PATH + 64], logs[MAX_PATH];
    char hex[33], t[128];
    unsigned i, digits = 0;
    uint8_t k[16];
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 19, n;
    SYSTEMTIME st;
    const char *why;
    static const char *const vars[] = { "H2_ACCT", "H2_ACCT_INPUT", "RECOMP_XBOX_EEPROM", "RECOMP_XBOX_HDKEY",
        "RECOMP_XBOX_EEPROM_KEY_FILE", "RECOMP_XBOX_TITLE_KEYS", "RECOMP_NET", "RECOMP_FB_WINDOW",
        "RECOMP_KEYBOARD", "RECOMP_GPU" };

    if (!verified()) { tell("Recover is locked until your identity is verified (steps 1-3).", MB_ICONWARNING); return; }
    if (!save_identity()) return;
    h2_bin_path(s_root, "halo2_recomp.exe", exe, sizeof exe);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) { tell("halo2_recomp.exe was not found beside Insignia Setup.", MB_ICONERROR); return; }
    if (!ask_recover(tag, sizeof tag, num, sizeof num)) { SecureZeroMemory(num, sizeof num); return; }
    for (i = 0; num[i]; i++) if (num[i] >= '0' && num[i] <= '9') digits++;
    if (!tag[0] || digits != 16) {
        SecureZeroMemory(num, sizeof num);
        tell("Enter your gamertag and the full 16-digit number from your insignia.live Web Dashboard.", MB_ICONWARNING);
        return;
    }
    {   /* a copy of the drive's account area before the game writes it */
        char cfg[MAX_PATH], bak[MAX_PATH];
        GetLocalTime(&st);
        path_data("DiskConfig.bin", cfg, sizeof cfg);
        snprintf(bak, sizeof bak, "%s\\%s\\DiskConfig.before-recovery-%04u%02u%02u_%02u%02u%02u.bin", s_root, h2_data_name(),
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        CopyFileA(cfg, bak, TRUE);
    }
    path_data("logs", logs, sizeof logs);
    CreateDirectoryA(logs, NULL);
    if (!CreatePipe(&rd, &wr, &sa, 0)) { SecureZeroMemory(num, sizeof num); tell("Could not start the recovery.", MB_ICONERROR); return; }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

    /* The identity, as Launch-Halo2-Insignia.cmd gives it. */
    GetWindowTextA(s_key, t, sizeof t);
    hex_key(t, k);
    SecureZeroMemory(t, sizeof t);
    for (i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02X", k[i]);
    SecureZeroMemory(k, sizeof k);
    SetEnvironmentVariableA("H2_ACCT", "recover");
    snprintf(line, sizeof line, "%llu", (unsigned long long)(uintptr_t)rd);
    SetEnvironmentVariableA("H2_ACCT_INPUT", line);
    path_data("Identity\\eeprom.bin", v, sizeof v);      SetEnvironmentVariableA("RECOMP_XBOX_EEPROM", v);
    SetEnvironmentVariableA("RECOMP_XBOX_HDKEY", hex);
    SecureZeroMemory(hex, sizeof hex);
    path_data("Identity\\eepromkey.bin", v, sizeof v);   SetEnvironmentVariableA("RECOMP_XBOX_EEPROM_KEY_FILE", v);
    path_data("Identity\\titlekeys.bin", v, sizeof v);   SetEnvironmentVariableA("RECOMP_XBOX_TITLE_KEYS", v);
    SetEnvironmentVariableA("RECOMP_NET", "1");
    SetEnvironmentVariableA("RECOMP_FB_WINDOW", "1");
    SetEnvironmentVariableA("RECOMP_KEYBOARD", "1");         /* a controller, or the game waits on a screen */
    SetEnvironmentVariableA("RECOMP_GPU", "d3d11");

    /* Started like the launcher (cmd with file redirection), inheriting the pipe. */
    GetLocalTime(&st);
    snprintf(cmd, sizeof cmd, "cmd.exe /c \"\"%s\" 1>\"%s\\recovery_%04u%02u%02u_%02u%02u%02u_stdout.log\" "
             "2>\"%s\\recovery_%04u%02u%02u_%02u%02u%02u.log\"\"", exe,
             logs, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             logs, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;                                  /* the console window; the game opens its own */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, s_root, &si, &pi)) {
        for (i = 0; i < sizeof vars / sizeof vars[0]; i++) SetEnvironmentVariableA(vars[i], NULL);
        CloseHandle(rd); CloseHandle(wr); SecureZeroMemory(num, sizeof num);
        tell("Could not start Halo 2.", MB_ICONERROR);
        return;
    }
    for (i = 0; i < sizeof vars / sizeof vars[0]; i++) SetEnvironmentVariableA(vars[i], NULL);
    CloseHandle(rd);
    snprintf(line, sizeof line, "%s\n%s\n", tag, num);
    WriteFile(wr, line, (DWORD)strlen(line), &n, NULL);
    CloseHandle(wr);
    SecureZeroMemory(line, sizeof line);
    SecureZeroMemory(num, sizeof num);
    CloseHandle(pi.hThread);
    say(s_status, "Halo 2 is fetching your account (it closes by itself)...");
    EnableWindow(s_main, FALSE);
    {
        DWORD start = GetTickCount();
        MSG msg;
        while (MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 500, QS_ALLINPUT) != WAIT_OBJECT_0) {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
            if (GetTickCount() - start > 300000) { TerminateProcess(pi.hProcess, 19); break; }
        }
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    EnableWindow(s_main, TRUE);
    SetForegroundWindow(s_main);
    why = recover_result(code);
    if (why) { say(s_status, "The account was not recovered."); tell(why, MB_ICONERROR); return; }
    snprintf(line, sizeof line, "OK: %s recovered from Insignia", tag);
    say(s_acc_st, line);
    say(s_status, "Account Recovery Successful");
    tell("Account Recovery Successful", MB_ICONINFORMATION);
}

/* ---- startup ---------------------------------------------------------------------------------- */
static void load_current(void)
{
    char path[MAX_PATH];
    path_data("Identity\\eeprom.bin", path, sizeof path);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { update_state(); return; }
    SetWindowTextA(s_ee, path);
    check_eeprom(path);
    unlock_with_saved_key();
    if (s_unlocked)
        say(s_bios_st, s_have_titlekeys ? "OK: verified earlier against your console's BIOS (eepromkey.bin)."
                                        : "Verified earlier, but titlekeys.bin is missing: add your BIOS again.");
    path_data("Identity\\hddinfo.txt", path, sizeof path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) key_from_hddinfo(path);
    update_state();
}

/* The install folder: the one holding build-ninja\ (this program lives there). */
static void find_root(void)
{
    char *sl;
    const char *e = getenv("H2_ROOT");
    if (e && *e) { strcpy_s(s_root, sizeof s_root, e); return; }
    GetModuleFileNameA(NULL, s_root, sizeof s_root);
    if ((sl = strrchr(s_root, '\\')) != NULL) *sl = 0;
    if ((sl = strrchr(s_root, '\\')) != NULL && _stricmp(sl + 1, "build-ninja") == 0) *sl = 0;
}

/* ---- window ----------------------------------------------------------------------------------- */
static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    if ((style & 0xF) == BS_GROUPBOX && cls[0] == 'B') {    /* a panel the window paints */
        skin_panel(&s_skin_main, x, y + s_yoff, w, h, text);
        return NULL;
    }
    return skin_mk(s_main, cls, text, style, x, y + s_yoff, w, h, id);
}

static void make_ui(void)
{
    int y = 12;
    skin_init();
    s_yoff = 76;
    s_skin_main.n = 0;
    s_skin_main.title = "INSIGNIA SETUP";
    s_skin_main.band_h = 60;
    skin_panel(&s_skin_main, 10, s_yoff + 4, 572, 56, NULL);
    mk("STATIC", "Play Halo 2 on Xbox Live through Insignia with your own Xbox console's identity and account. "
       "Your console must already be registered with Insignia. Nothing is sent anywhere until your EEPROM and "
       "hard-drive key are verified against your console's own BIOS.", SS_LEFT, 22, y - 2, 552, 44, 0);
    y += 54;
    mk("BUTTON", "1. Your console's EEPROM (eeprom.bin)", BS_GROUPBOX, 10, y, 572, 86, 0);
    s_ee = mk("EDIT", "", ES_AUTOHSCROLL | ES_READONLY, 24, y + 24, 440, 23, ID_EE_EDIT);
    mk("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 474, y + 23, 96, 25, ID_EE_BROWSE);
    s_ee_st = mk("STATIC", "Needed: the EEPROM backed up from your console.", SS_LEFT, 24, y + 56, 546, 20, ID_EE_STATUS);
    y += 96;
    mk("BUTTON", "2. Your console's BIOS and MCPX boot ROM (proof it is real hardware)", BS_GROUPBOX, 10, y, 572, 86, 0);
    mk("BUTTON", "Choose BIOS + MCPX ROM...", BS_PUSHBUTTON | WS_TABSTOP, 24, y + 22, 180, 25, ID_BIOS);
    mk("STATIC", "Read only to check the EEPROM and make Halo 2's keys; the BIOS itself is not kept.",
       SS_LEFT, 214, y + 20, 356, 30, 0);
    s_bios_st = mk("STATIC", "Needed: your console's BIOS and MCPX boot ROM, to prove the EEPROM is a real console's.",
                   SS_LEFT, 24, y + 54, 546, 28, ID_BIOS_STATUS);
    y += 96;
    mk("BUTTON", "3. Your console's hard-drive key", BS_GROUPBOX, 10, y, 572, 86, 0);
    s_key = mk("EDIT", "", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 24, y + 24, 260, 23, ID_KEY_EDIT);
    mk("BUTTON", "Show", BS_AUTOCHECKBOX | WS_TABSTOP, 292, y + 25, 60, 22, ID_KEY_SHOW);
    mk("BUTTON", "From hddinfo.txt...", BS_PUSHBUTTON | WS_TABSTOP, 430, y + 23, 140, 25, ID_KEY_FILE);
    s_key_st = mk("STATIC", "", SS_LEFT, 24, y + 56, 546, 20, ID_KEY_STATUS);
    y += 96;
    mk("BUTTON", "4. Your Xbox Live account", BS_GROUPBOX, 10, y, 572, 116, 0);
    mk("STATIC", "Recover it from Insignia (gamertag + the 16-digit number on your insignia.live Web Dashboard), "
       "or load it from your console's drive dump, a raw drive image or an xemu drive (.qcow2):",
       SS_LEFT, 24, y + 20, 546, 30, 0);
    s_acc = mk("EDIT", "", ES_AUTOHSCROLL | ES_READONLY, 24, y + 54, 440, 23, ID_ACC_EDIT);
    mk("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 474, y + 53, 96, 25, ID_ACC_BROWSE);
    s_acc_st = mk("STATIC", "Optional: an account from a drive, or recover one from Insignia.", SS_LEFT, 24, y + 86, 360, 20, ID_ACC_STATUS);
    s_btn_recover = mk("BUTTON", "Recover from Insignia...", BS_PUSHBUTTON | WS_TABSTOP, 394, y + 82, 176, 25, ID_RECOVER);
    y += 128;
    skin_panel(&s_skin_main, 10, y + s_yoff - 6, 572, 40, NULL);
    s_status = mk("STATIC", "", SS_LEFT, 22, y + 1, 354, 26, ID_STATUS);
    s_btn_save = mk("BUTTON", "Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 386, y, 92, 28, ID_SAVE);
    mk("BUTTON", "Close", BS_PUSHBUTTON | WS_TABSTOP, 488, y, 92, 28, ID_CLOSE);
}

static int browse(const char *filter, char *path, DWORD n)
{
    OPENFILENAMEA o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = s_main;
    o.lpstrFilter = filter;
    o.lpstrFile = path;
    o.nMaxFile = n;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    path[0] = 0;
    return GetOpenFileNameA(&o);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    char path[MAX_PATH];
    switch (m) {
    case WM_CREATE:
        s_main = h;
        make_ui();
        load_current();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_EE_BROWSE:
            if (browse("EEPROM (*.bin)\0*.bin\0All files\0*.*\0", path, sizeof path)) { SetWindowTextA(s_ee, path); check_eeprom(path); }
            return 0;
        case ID_BIOS: {
            char mcpx[MAX_PATH];
            MessageBoxA(s_main, "Choose your console's BIOS image first, then its MCPX boot ROM:\n\n"
                        "MCPX 1.0 for an Xbox 1.0 (kernels 3944, 4034, 4036, 4627)\n"
                        "MCPX 1.1 for an Xbox 1.1-1.6 (kernels 4817 to 5838)\n\n"
                        "They prove the EEPROM came from a real console and make Halo 2's keys. "
                        "The BIOS and ROM are not copied or kept.", APP_TITLE, MB_ICONINFORMATION);
            if (browse("BIOS image (*.bin)\0*.bin\0All files\0*.*\0", path, sizeof path) &&
                browse("MCPX boot ROM (*.bin)\0*.bin\0All files\0*.*\0", mcpx, sizeof mcpx))
                bios_step(path, mcpx);
            return 0;
        }
        case ID_KEY_FILE:
            if (browse("hddinfo.txt\0*.txt\0All files\0*.*\0", path, sizeof path)) key_from_hddinfo(path);
            return 0;
        case ID_KEY_EDIT:
            if (HIWORD(w) == EN_CHANGE) update_state();
            return 0;
        case ID_KEY_SHOW:
            s_show_key = !s_show_key;
            SendMessageA(s_key, EM_SETPASSWORDCHAR, s_show_key ? 0 : (WPARAM)'*', 0);
            InvalidateRect(s_key, NULL, TRUE);
            InvalidateRect((HWND)l, NULL, TRUE);
            return 0;
        case ID_ACC_BROWSE:
            if (browse("Drive dump or image (*.bin;*.img;*.qcow2;*.raw)\0*.bin;*.img;*.qcow2;*.raw\0All files\0*.*\0", path, sizeof path)) {
                SetWindowTextA(s_acc, path);
                check_accounts(path);
            }
            return 0;
        case ID_RECOVER: recover(); return 0;
        case ID_SAVE: save(); return 0;
        case ID_CLOSE: DestroyWindow(h); return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    {
        LRESULT r;
        if (skin_msg(h, m, w, l, &s_skin_main, &r)) return r;
    }
    return DefWindowProcA(h, m, w, l);
}

/* "Insignia Setup.exe" --eeprom FILE --bios FILE --mcpx FILE [--hdd-key HEX | --hddinfo FILE]
 *                      [--drive FILE] [--check]
 * does what the window does, without it. --check verifies and saves nothing.
 * Exit 0 verified (and saved unless --check); 3 EEPROM; 4 BIOS/MCPX; 5 key;
 * 6 save failed; 2 usage. Never touches the network. */
static int run_args(void)
{
    const char *ee = NULL, *key = NULL, *info = NULL, *drive = NULL, *bios = NULL, *mcpx = NULL;
    char t[256];
    int i, check = 0;
    for (i = 1; i < __argc; i++) {
        const char *a = __argv[i], *v = i + 1 < __argc ? __argv[i + 1] : NULL;
        if (!strcmp(a, "--check")) { check = 1; continue; }
        if (!v) return 2;
        if (!strcmp(a, "--eeprom")) ee = v;
        else if (!strcmp(a, "--hdd-key")) key = v;
        else if (!strcmp(a, "--hddinfo")) info = v;
        else if (!strcmp(a, "--drive")) drive = v;
        else if (!strcmp(a, "--bios")) bios = v;
        else if (!strcmp(a, "--mcpx")) mcpx = v;
        else if (!strcmp(a, "--root")) strcpy_s(s_root, sizeof s_root, v);
        else return 2;
        i++;
    }
    s_cli = 1;
    {   /* stderr redirected to a file or pipe: keep it; else the calling console */
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if ((h == NULL || h == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
            freopen("CONOUT$", "w", stderr);
    }
    if (!ee || !bios || !mcpx) {
        fprintf(stderr, "usage: \"Insignia Setup.exe\" --eeprom FILE --bios FILE --mcpx FILE "
                        "[--hdd-key HEX | --hddinfo FILE] [--drive FILE] [--root DIR] [--check]\n");
        return 2;
    }
    check_eeprom(ee);
    GetWindowTextA(s_ee_st, t, sizeof t); fprintf(stderr, "EEPROM: %s\n", t);
    if (!s_ee_ok) return 3;
    if (key) SetWindowTextA(s_key, key);
    else if (info) key_from_hddinfo(info);
    bios_step(bios, mcpx);
    GetWindowTextA(s_bios_st, t, sizeof t); fprintf(stderr, "BIOS: %s\n", t);
    if (!s_unlocked || !s_have_titlekeys) return 4;
    update_state();
    GetWindowTextA(s_key_st, t, sizeof t); fprintf(stderr, "Hard-drive key: %s\n", t);
    if (!verified()) return 5;
    if (drive) { check_accounts(drive); GetWindowTextA(s_acc_st, t, sizeof t); fprintf(stderr, "Drive: %s\n", t); }
    if (check) { fprintf(stderr, "Verified. Nothing saved (--check).\n"); return 0; }
    save();
    GetWindowTextA(s_status, t, sizeof t);
    fprintf(stderr, "%s\n", t);
    return strncmp(t, "Saved", 5) == 0 || strncmp(t, "Identity saved", 14) == 0 ? 0 : 6;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r = { 0, 0, 592, 530 + 76 };
    (void)prev; (void)cmd;
    find_root();
    InitCommonControls();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hbrBackground = NULL;
    wc.lpszClassName = "Halo2InsigniaSetup";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    if (__argc > 1) {                          /* the controls exist (hidden) so the checks are the same code */
        int i;
        for (i = 1; i + 1 < __argc; i++) if (!strcmp(__argv[i], "--root")) strcpy_s(s_root, sizeof s_root, __argv[i + 1]);
    }
    CreateWindowExA(0, wc.lpszClassName, APP_TITLE,
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | (__argc > 1 ? 0 : WS_VISIBLE),
                    CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (__argc > 1)
        return run_args();
    ShowWindow(s_main, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(s_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
