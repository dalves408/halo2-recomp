/* extras.c: config.exe's EXTRAS page (extras.h).
 *
 * The Multiplayer Map Pack: which packs the game has (<data>\TDATA\4d530064\$c\4d53006400000001..4), and ADD MAP PACK
 * DISC, which runs halo2_mappack.exe (installer/halo2_mappack.c) on the player's disc image or folder.
 *
 * The bonus videos, in <install>\extras as the disc has them (MPEG-1 + CRI ADX/AIX, media.c) or as the .mp4 that
 * RE-ENCODE TO H.264 makes of each (Windows' own H.264 and AAC encoders; the .sfd goes once its .mp4 is complete).
 * PLAY plays it in the page's own window (config.exe's, or the game's F1 menu): the picture at its own shape, the sound
 * through waveOut (5.1 where the video has it; Windows mixes it down for stereo), Killtacular's subtitles in the Windows display language where the disc has one.
 *
 * The game's F1 menu shows the same page (extras_init_game): the map packs installed and the videos to play, chosen
 * with the keyboard, the mouse or a pad. Adding the disc and re-encoding stay in config.exe (a file dialog, and
 * minutes of CPU), so the game page has neither. Its player covers the game's picture and closes back to the menu.
 */
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extras.h"
#include "media.h"
#include "h2_layout.h"

/* ---- the videos -------------------------------------------------------------------------------------------------- */
typedef struct {
    const char *title, *desc, *preview;   /* preview: videos\<name>.bmp */
    const char *dir;                      /* under extras\movies */
    int parts;                            /* .sfd parts (Killtacular: 3) */
    int per_language;                     /* movie1: one copy per language, in <dir>\<lang> */
    int subtitles;
} Video;

static const Video VIDEOS[] = {
    { "Another Day at the Beach", "A Halo 2 cinematic made for this disc: what became of the lost ODST squad in Old Mombasa.",
      "beach", "movie1", 1, 1, 0 },
    { "Halo 2: Killtacular", "Behind-the-scenes interviews with the people who made Halo 2 and its new maps.",
      "interPrev", "movie2", 3, 0, 1 },
    { "Audio Test", "Check your speakers (5.1 where you have it) with a helpful and very silly presentation.",
      "sstest", "soundtest", 1, 0, 0 },
    { "Halo 2 Announcement Trailer", "The first Halo 2 trailer, shown in New York in August 2002.",
      "announce", "movie3", 1, 0, 0 },
    { "Halo E3 2000", "Halo as it was first shown at E3 2000, five years before this disc.",
      "e3_2000", "movie4", 1, 0, 0 },
};
#define NVID ((int)(sizeof VIDEOS / sizeof VIDEOS[0]))

static const char *const LANGS[] = { "en", "de", "fr", "it", "sp", "jp", "ko", "ch" };

static char s_root[MAX_PATH], s_extras[MAX_PATH], s_data[MAX_PATH];
static HWND s_owner;
static HBITMAP s_prev[NVID];
static RECT s_play_r[NVID], s_add_r, s_enc_r;
static int s_hover = -1;      /* 0..NVID-1 play, 100 add, 101 encode */
static int s_sel = -1;        /* the game page: the video selected with the keys or a pad */
static int s_ingame;          /* extras_init_game: the F1 menu's page */
static HWND s_game;           /* the game page: the window the player covers */

/* What is installed, read when the page opens and after a job (the game repaints its menu 60 times a second). */
static int s_vstate[NVID];    /* video_state */
static int s_any_sfd;
static char s_pack_names[96];
static int s_npacks;

/* the job: copying from the disc or re-encoding */
enum { JOB_NONE, JOB_COPY, JOB_ENCODE };
static volatile LONG s_job, s_cancel;
static HANDLE s_job_thread;
static CRITICAL_SECTION s_lock;
static char s_status[200];
static volatile double s_job_frac;

static const char *ui_lang(void)
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_GERMAN: return "de";
    case LANG_FRENCH: return "fr";
    case LANG_ITALIAN: return "it";
    case LANG_SPANISH: return "sp";
    case LANG_JAPANESE: return "jp";
    case LANG_KOREAN: return "ko";
    case LANG_CHINESE: return "ch";
    default: return "en";
    }
}

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

/* The files of video i (language lang for movie1): its .mp4 when re-encoded, else its .sfd part(s). n = 0: none. */
static int video_files(int i, const char *lang, char out[3][MAX_PATH], int *mp4)
{
    const Video *v = &VIDEOS[i];
    char dir[MAX_PATH];
    int k;
    if (v->per_language) snprintf(dir, sizeof dir, "%s\\movies\\%s\\%s", s_extras, v->dir, lang);
    else snprintf(dir, sizeof dir, "%s\\movies\\%s", s_extras, v->dir);
    snprintf(out[0], MAX_PATH, "%s\\%s", dir, v->parts > 1 ? "killtacular.mp4" : "ddmovie.mp4");
    if (exists(out[0])) { *mp4 = 1; return 1; }
    *mp4 = 0;
    for (k = 0; k < v->parts; k++) {
        if (v->parts > 1) snprintf(out[k], MAX_PATH, "%s\\kill_part%d.sfd", dir, k + 1);
        else snprintf(out[k], MAX_PATH, "%s\\ddmovie.sfd", dir);
        if (!exists(out[k])) return 0;
    }
    return v->parts;
}

/* The language to play movie1 in: the Windows display language when installed, else English, else any. */
static const char *beach_lang(void)
{
    char f[3][MAX_PATH];
    int mp4;
    size_t k;
    if (video_files(0, ui_lang(), f, &mp4)) return ui_lang();
    for (k = 0; k < sizeof LANGS / sizeof *LANGS; k++)
        if (video_files(0, LANGS[k], f, &mp4)) return LANGS[k];
    return "en";
}

static int video_state(int i)   /* 0 not installed, 1 MPEG-1, 2 H.264 */
{
    char f[3][MAX_PATH];
    int mp4, n = video_files(i, i == 0 ? beach_lang() : "", f, &mp4);
    return n ? (mp4 ? 2 : 1) : 0;
}

static int packs_installed(char *names, size_t n)
{
    static const char *const PACK[4] = { "Bonus", "Killtacular", "Maptacular", "Blastacular" };
    int k, count = 0;
    names[0] = 0;
    for (k = 0; k < 4; k++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\TDATA\\4d530064\\$c\\4d5300640000000%d\\ContentMeta.xbx", s_data, k + 1);
        if (exists(p)) {
            if (count++) strncat(names, ", ", n - strlen(names) - 1);
            strncat(names, PACK[k], n - strlen(names) - 1);
        }
    }
    return count;
}

void extras_init(HWND owner, const char *root)
{
    int i;
    static int once;
    if (!once) { InitializeCriticalSection(&s_lock); once = 1; }
    s_owner = owner;
    snprintf(s_root, sizeof s_root, "%s", root);
    snprintf(s_extras, sizeof s_extras, "%s\\extras", root);
    h2_data_path(root, "", s_data, sizeof s_data);
    for (i = 0; i < NVID; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\videos\\%s.bmp", s_extras, VIDEOS[i].preview);
        if (s_prev[i]) DeleteObject(s_prev[i]);
        s_prev[i] = (HBITMAP)LoadImageA(NULL, p, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    }
    extras_refresh();
}

void extras_init_game(HWND game, const char *root)
{
    s_ingame = 1;
    s_game = game;
    extras_init(NULL, root);
    if (s_sel < 0 || !s_vstate[s_sel]) s_sel = extras_next_video(-1, 1);
}

const char *extras_status(void) { return s_status; }
int extras_busy(void) { return s_job != JOB_NONE; }

static void set_status(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&s_lock);
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&s_lock);
    if (s_owner) PostMessageA(s_owner, WM_APP_EXTRAS, 0, 0);
}

/* ---- drawing ----------------------------------------------------------------------------------------------------- */
static int any_sfd(void)
{
    int i;
    size_t k;
    for (i = 0; i < NVID; i++) {
        char f[3][MAX_PATH];
        int mp4;
        if (VIDEOS[i].per_language) {
            for (k = 0; k < sizeof LANGS / sizeof *LANGS; k++)
                if (video_files(i, LANGS[k], f, &mp4) && !mp4) return 1;
        } else if (video_files(i, "", f, &mp4) && !mp4) return 1;
    }
    return 0;
}

void extras_refresh(void)
{
    char names[96];
    int i, n = packs_installed(names, sizeof names), st[NVID], sfd = any_sfd();
    for (i = 0; i < NVID; i++) st[i] = video_state(i);
    EnterCriticalSection(&s_lock);
    memcpy(s_vstate, st, sizeof st);
    s_any_sfd = sfd;
    s_npacks = n;
    memcpy(s_pack_names, names, sizeof names);
    LeaveCriticalSection(&s_lock);
}

int extras_next_video(int cur, int dir)
{
    int i;
    for (i = cur + dir; i >= 0 && i < NVID; i += dir)
        if (s_vstate[i]) return i;
    if (cur >= 0 && cur < NVID && s_vstate[cur]) return cur;
    for (i = 0; i < NVID; i++) if (s_vstate[i]) return i;
    return -1;
}

int  extras_selected(void) { return s_sel; }
void extras_select(int i) { s_sel = i; }

void extras_draw(HDC dc, const UiFonts *f, const RECT *area)
{
    RECT r, sec;
    char names[96], t[200];
    int i, y, n, any = 0, busy = extras_busy();
    int x0 = area->left, x1 = area->right;

    /* the map packs */
    SetRect(&sec, x0, area->top, x1, area->top + ui_px(f, 30));
    ui_section(dc, f, &sec, "Multiplayer Map Pack");
    y = sec.bottom + ui_px(f, 6);
    EnterCriticalSection(&s_lock);
    n = s_npacks;
    memcpy(names, s_pack_names, sizeof names);
    LeaveCriticalSection(&s_lock);
    if (n) snprintf(t, sizeof t, "Installed: %s.%s", names,
                    strstr(names, "Blastacular") ? "" : " Blastacular was never on a disc: it comes from the in-game "
                                                       "content download (Xbox Live).");
    else if (s_ingame) snprintf(t, sizeof t, "No map packs installed. Add them from your Halo 2 Multiplayer Map Pack "
                                             "disc in Config.exe (EXTRAS), or download them here through Xbox Live.");
    else snprintf(t, sizeof t, "No map packs installed. Add them from your Halo 2 Multiplayer Map Pack disc, "
                                "or download them in the game through Xbox Live.");
    SetRect(&r, x0 + ui_px(f, 12), y, x1 - (s_ingame ? 0 : ui_px(f, 250)), y + ui_px(f, 44));
    ui_text(dc, f->fine, RGB(190, 214, 240), &r, t, DT_LEFT | DT_WORDBREAK);
    SetRect(&s_add_r, 0, 0, 0, 0);
    if (!s_ingame) {
        SetRect(&s_add_r, x1 - ui_px(f, 236), y + ui_px(f, 2), x1, y + ui_px(f, 38));
        ui_button(dc, f, &s_add_r, busy && s_job == JOB_COPY ? "CANCEL" : "ADD MAP PACK DISC...", 0, s_hover == 100);
    }
    y += ui_px(f, 52);

    /* the videos */
    SetRect(&sec, x0, y, x1, y + ui_px(f, 30));
    ui_section(dc, f, &sec, "Bonus videos");
    y = sec.bottom + ui_px(f, 6);
    for (i = 0; i < NVID; i++) {
        int st = s_vstate[i], rh = ui_px(f, 58);
        RECT pr, tr, dr;
        any |= st != 0;
        SetRect(&pr, x0 + ui_px(f, 12), y, x0 + ui_px(f, 12) + ui_px(f, 96), y + ui_px(f, 54));
        if (s_prev[i]) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, s_prev[i]);
            BITMAP bm;
            GetObject(s_prev[i], sizeof bm, &bm);
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, mem, 0, 0, bm.bmWidth,
                       bm.bmHeight, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        } else {
            ui_chamfer(dc, &pr, ui_px(f, 6), RGB(10, 24, 48), RGB(40, 70, 110));
        }
        SetRect(&tr, pr.right + ui_px(f, 14), y, x1 - ui_px(f, 130), y + ui_px(f, 22));
        ui_text(dc, f->body, st ? RGB(255, 255, 255) : RGB(120, 140, 165), &tr, VIDEOS[i].title,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetRect(&dr, tr.left, tr.bottom, tr.right, y + rh - ui_px(f, 4));
        snprintf(t, sizeof t, "%s%s", VIDEOS[i].desc,
                 st == 2 ? "  (H.264)" : st == 0 ? "  Not installed." : "");
        ui_text(dc, f->fine, RGB(150, 178, 210), &dr, t, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        SetRect(&s_play_r[i], x1 - ui_px(f, 116), y + ui_px(f, 10), x1, y + ui_px(f, 46));
        if (st) ui_button(dc, f, &s_play_r[i], "PLAY", s_ingame && s_sel == i, s_hover == i);
        else SetRect(&s_play_r[i], 0, 0, 0, 0);
        y += rh;
    }
    /* re-encode */
    SetRect(&s_enc_r, 0, 0, 0, 0);
    if (s_ingame) {
        if (!any) {
            SetRect(&r, x0 + ui_px(f, 12), y + ui_px(f, 4), x1, y + ui_px(f, 44));
            ui_text(dc, f->fine, RGB(150, 178, 210), &r,
                    "To watch these, add your Map Pack disc in Config.exe (EXTRAS) and choose to copy its videos.",
                    DT_LEFT | DT_WORDBREAK);
        }
    } else if (busy && s_job == JOB_ENCODE) {
        SetRect(&s_enc_r, x1 - ui_px(f, 236), y + ui_px(f, 4), x1, y + ui_px(f, 40));
        ui_button(dc, f, &s_enc_r, "STOP RE-ENCODING", 0, s_hover == 101);
    } else if (any && s_any_sfd) {
        SetRect(&s_enc_r, x1 - ui_px(f, 236), y + ui_px(f, 4), x1, y + ui_px(f, 40));
        ui_button(dc, f, &s_enc_r, "RE-ENCODE TO H.264", 0, s_hover == 101);
        SetRect(&r, x0 + ui_px(f, 12), y + ui_px(f, 4), s_enc_r.left - ui_px(f, 12), y + ui_px(f, 44));
        ui_text(dc, f->fine, RGB(150, 178, 210), &r,
                "The videos are kept exactly as on the disc (MPEG-1, about 2.8 GB). Re-encoding makes them about a "
                "quarter of the size with Windows' own H.264 encoder; the originals are then removed.",
                DT_LEFT | DT_WORDBREAK);
    } else if (!any) {
        SetRect(&r, x0 + ui_px(f, 12), y + ui_px(f, 4), x1, y + ui_px(f, 44));
        ui_text(dc, f->fine, RGB(150, 178, 210), &r,
                "To watch these, add your Map Pack disc and choose to copy its videos.", DT_LEFT | DT_WORDBREAK);
    }
}

int extras_hover(int x, int y)
{
    POINT p;
    int h = -1, i;
    p.x = x; p.y = y;
    for (i = 0; i < NVID; i++) if (PtInRect(&s_play_r[i], p)) h = i;
    if (PtInRect(&s_add_r, p)) h = 100;
    if (PtInRect(&s_enc_r, p)) h = 101;
    if (h == s_hover) return 0;
    s_hover = h;
    return 1;
}

/* ---- ADD MAP PACK DISC ------------------------------------------------------------------------------------------- */
typedef struct { char src[MAX_PATH]; int videos; } CopyJob;

static DWORD WINAPI copy_thread(LPVOID arg)
{
    CopyJob *j = (CopyJob *)arg;
    char exe[MAX_PATH], cmd[3 * MAX_PATH + 64], line[512];
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 99, n;
    size_t ln = 0;
    char err[200] = "";

    h2_bin_path(s_root, "halo2_mappack.exe", exe, sizeof exe);
    if (j->videos)
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" \"%s\" --videos \"%s\"", exe, j->src, s_data, s_extras);
    else
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" \"%s\"", exe, j->src, s_data);
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    set_status("Copying from the Map Pack disc...");
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        set_status("halo2_mappack.exe could not be started (error %lu).", GetLastError());
        CloseHandle(rd); CloseHandle(wr);
        free(j);
        InterlockedExchange(&s_job, JOB_NONE);
        return 0;
    }
    CloseHandle(wr);
    for (;;) {
        char c;
        if (s_cancel) TerminateProcess(pi.hProcess, 98);
        if (!ReadFile(rd, &c, 1, &n, NULL) || !n) break;
        if (c == '\r') continue;
        if (c != '\n') { if (ln < sizeof line - 1) line[ln++] = c; continue; }
        line[ln] = 0;
        ln = 0;
        if (!strncmp(line, "PROGRESS ", 9)) {
            unsigned long long done = 0, total = 0;
            if (sscanf(line + 9, "%llu %llu", &done, &total) == 2 && total)
                set_status("Copying from the Map Pack disc: %d%% (%llu of %llu MB)", (int)(done * 100 / total),
                           done >> 20, total >> 20);
        } else if (!strncmp(line, "ERROR ", 6)) {
            snprintf(err, sizeof err, "%s", line + 6);
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (s_cancel) set_status("Copying stopped. Files already checked are kept; add the disc again to finish.");
    else if (code == 0) set_status(j->videos ? "The map packs and videos are installed." : "The map packs are installed.");
    else set_status("%s", err[0] ? err : "The Map Pack disc could not be read.");
    free(j);
    extras_init(s_owner, s_root);    /* the previews, now there */
    InterlockedExchange(&s_job, JOB_NONE);
    PostMessageA(s_owner, WM_APP_EXTRAS, 0, 0);
    return 0;
}

static void add_disc(HWND owner)
{
    OPENFILENAMEA o;
    char file[MAX_PATH] = "";
    CopyJob *j;
    int r;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = owner;
    o.lpstrFilter = "Map Pack disc image or its default.xbe (*.iso;*.xiso;default.xbe)\0*.iso;*.xiso;default.xbe\0"
                    "All files (*.*)\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = sizeof file;
    o.lpstrTitle = "Select your Halo 2 Multiplayer Map Pack disc (an image, or default.xbe in a folder of its files)";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&o)) return;
    j = (CopyJob *)calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->src, sizeof j->src, "%s", file);
    {   /* default.xbe picked: the folder it is in */
        char *sl = strrchr(j->src, '\\');
        if (sl && !_stricmp(sl + 1, "default.xbe")) *sl = 0;
    }
    r = MessageBoxA(owner, "Also copy the bonus videos (about 2.8 GB)?\n\n"
                           "Another Day at the Beach, Halo 2: Killtacular, the audio test, the Halo 2 announcement "
                           "trailer and Halo E3 2000. You can re-encode them to H.264 here afterwards to save space.",
                    "Multiplayer Map Pack", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) { free(j); return; }
    j->videos = r == IDYES;
    s_cancel = 0;
    if (s_job_thread) { CloseHandle(s_job_thread); s_job_thread = NULL; }
    InterlockedExchange(&s_job, JOB_COPY);
    s_job_thread = CreateThread(NULL, 0, copy_thread, j, 0, NULL);
}

/* ---- RE-ENCODE TO H.264 ------------------------------------------------------------------------------------------ */
typedef struct { char src[3][MAX_PATH]; int n; char dst[MAX_PATH]; unsigned long long bytes; char title[64]; } EncItem;

static unsigned long long fsize(const char *p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExA(p, GetFileExInfoStandard, &a)
           ? ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow : 0;
}

static struct { unsigned long long done, total; int index, count; const char *title; } s_enc;

static void enc_progress(double p, void *user)
{
    EncItem *it = (EncItem *)user;
    double all = s_enc.total ? ((double)s_enc.done + p * (double)it->bytes) / (double)s_enc.total : 0;
    set_status("Re-encoding %d of %d (%s): %d%%, all %d%%", s_enc.index + 1, s_enc.count, it->title, (int)(p * 100),
               (int)(all * 100));
}

static DWORD WINAPI encode_thread(LPVOID arg)
{
    EncItem items[16];
    int n = 0, i, ok = 0, failed = 0;
    unsigned long long before = 0, after = 0;
    char err[200] = "";
    size_t k;
    (void)arg;
    for (i = 0; i < NVID; i++) {
        size_t nl = VIDEOS[i].per_language ? sizeof LANGS / sizeof *LANGS : 1;
        for (k = 0; k < nl && n < 16; k++) {
            char f[3][MAX_PATH];
            int mp4, c = video_files(i, VIDEOS[i].per_language ? LANGS[k] : "", f, &mp4), m;
            if (!c || mp4) continue;
            memset(&items[n], 0, sizeof items[n]);
            for (m = 0; m < c; m++) {
                snprintf(items[n].src[m], MAX_PATH, "%s", f[m]);
                items[n].bytes += fsize(f[m]);
            }
            items[n].n = c;
            {   /* <dir>\ddmovie.mp4, or <dir>\killtacular.mp4 */
                char *sl;
                snprintf(items[n].dst, MAX_PATH, "%s", f[0]);
                sl = strrchr(items[n].dst, '\\');
                snprintf(sl + 1, MAX_PATH - (sl + 1 - items[n].dst), "%s",
                         c > 1 ? "killtacular.mp4" : "ddmovie.mp4");
            }
            snprintf(items[n].title, sizeof items[n].title, "%s%s%s", VIDEOS[i].title,
                     VIDEOS[i].per_language ? ", " : "", VIDEOS[i].per_language ? LANGS[k] : "");
            s_enc.total += items[n].bytes;
            n++;
        }
    }
    s_enc.count = n;
    for (i = 0; i < n && !s_cancel; i++) {
        WCHAR w[3][MAX_PATH], wd[MAX_PATH];
        const WCHAR *wp[3];
        int m, r;
        s_enc.index = i;
        for (m = 0; m < items[i].n; m++) {
            MultiByteToWideChar(CP_ACP, 0, items[i].src[m], -1, w[m], MAX_PATH);
            wp[m] = w[m];
        }
        MultiByteToWideChar(CP_ACP, 0, items[i].dst, -1, wd, MAX_PATH);
        enc_progress(0, &items[i]);
        r = media_encode_mp4(wp, items[i].n, wd, enc_progress, &items[i], &s_cancel, err, sizeof err);
        if (r == 1) {
            before += items[i].bytes;
            after += fsize(items[i].dst);
            for (m = 0; m < items[i].n; m++) DeleteFileA(items[i].src[m]);
            ok++;
        } else if (r == 0) {
            failed++;
            break;
        }
        s_enc.done += items[i].bytes;
    }
    if (failed) set_status("Re-encoding failed: %s. The original videos are kept.", err);
    else if (s_cancel) set_status("Re-encoding stopped. %d video%s re-encoded; the rest are kept as they were.", ok,
                                  ok == 1 ? "" : "s");
    else set_status("%d video%s re-encoded: %llu MB now, was %llu MB.", ok, ok == 1 ? "" : "s", after >> 20, before >> 20);
    memset(&s_enc, 0, sizeof s_enc);
    extras_refresh();
    InterlockedExchange(&s_job, JOB_NONE);
    PostMessageA(s_owner, WM_APP_EXTRAS, 0, 0);
    return 0;
}

void extras_cancel(void)
{
    if (!extras_busy()) return;
    InterlockedExchange(&s_cancel, 1);
    if (s_job_thread) {
        WaitForSingleObject(s_job_thread, 60000);
        CloseHandle(s_job_thread);
        s_job_thread = NULL;
    }
}

/* ---- the player ---------------------------------------------------------------------------------------------------- */
/* KSDATAFORMAT_SUBTYPE_PCM, here so no GUID library is needed */
static const GUID SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
#define NBUF 6
#define BUF_FRAMES 4800                      /* 100 ms at 48 kHz */

typedef struct { int start, end; WCHAR text[200]; } Sub;

static struct {
    int active, video;
    HWND host;                                /* config.exe's window, repainted for each picture; NULL in the game */
    Media *m;
    HANDLE thread;
    volatile LONG stop, paused, done;
    CRITICAL_SECTION lock;
    uint8_t *px;                              /* the picture shown, BGRX */
    int vw, vh, have_pic;
    double clock;
    int fullscreen;
    LONG style;
    WINDOWPLACEMENT place;
    DWORD hint_until;
    Sub *subs;
    int nsubs, sub_on, sub_lang;
    WCHAR title[64];
} P;

static const char *const SUB_FILES[] = { "German_01", "Chinese_02", "Japanese_03", "Korean_04", "French_05",
                                         "Italian_06", "Spanish_07" };
static const char *const SUB_LANG[] = { "de", "ch", "jp", "ko", "fr", "it", "sp" };
static const char *const SUB_NAME[] = { "German", "Chinese", "Japanese", "Korean", "French", "Italian", "Spanish" };

static void subs_load(int lang)
{
    char p[MAX_PATH];
    FILE *f;
    long n;
    WCHAR *t, *s, *e;
    free(P.subs);
    P.subs = NULL;
    P.nsubs = 0;
    if (lang < 0) return;
    snprintf(p, sizeof p, "%s\\movies\\movie2\\subtitle\\%s.txt", s_extras, SUB_FILES[lang]);
    if (!(f = fopen(p, "rb"))) return;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    t = (WCHAR *)calloc((size_t)n / 2 + 2, sizeof(WCHAR));
    if (t) fread(t, 1, (size_t)n, f);
    fclose(f);
    if (!t) return;
    P.subs = (Sub *)calloc(1024, sizeof(Sub));
    /* UTF-16: a first line with the time unit (1000: milliseconds), then "start,end,text" */
    for (s = t + (t[0] == 0xFEFF); s && *s && P.subs && P.nsubs < 1024; s = e) {
        WCHAR *c1, *c2, *q;
        int a, b;
        e = wcschr(s, L'\n');
        if (e) *e++ = 0;
        if ((q = wcschr(s, L'\r')) != NULL) *q = 0;
        c1 = wcschr(s, L',');
        c2 = c1 ? wcschr(c1 + 1, L',') : NULL;
        if (!c2) continue;
        a = _wtoi(s); b = _wtoi(c1 + 1);
        q = c2 + 1;
        while (*q == L' ') q++;
        if (*q == L'"') { WCHAR *z; q++; if ((z = wcsrchr(q, L'"')) != NULL) *z = 0; }
        if (!*q) continue;
        if (P.nsubs && P.subs[P.nsubs - 1].start == a && P.subs[P.nsubs - 1].end == b) {   /* a second line */
            wcsncat(P.subs[P.nsubs - 1].text, L"\n", 199 - wcslen(P.subs[P.nsubs - 1].text));
            wcsncat(P.subs[P.nsubs - 1].text, q, 199 - wcslen(P.subs[P.nsubs - 1].text));
            continue;
        }
        P.subs[P.nsubs].start = a;
        P.subs[P.nsubs].end = b;
        wcsncpy(P.subs[P.nsubs].text, q, 199);
        P.nsubs++;
    }
    free(t);
}

static DWORD WINAPI player_thread(LPVOID unused)
{
    Media *m = P.m;
    HWAVEOUT wo = NULL;
    WAVEHDR hdr[NBUF];
    int16_t *buf[NBUF];
    int ch, rate, i, have_audio = 1, audio_end = 0, video_end = 0, pending = 0, out_ch, primed = 0;
    double ft = 0, fps = media_fps(m);
    long long played_base = 0;
    LARGE_INTEGER qf, q0, qn;
    int16_t *first = (int16_t *)malloc(sizeof(int16_t) * 6 * BUF_FRAMES);
    (void)unused;

    QueryPerformanceFrequency(&qf);
    /* the sound's format is known once some has been decoded */
    primed = media_audio(m, first, BUF_FRAMES);
    ch = media_channels(m);
    rate = media_rate(m);
    out_ch = ch;
    if (primed > 0) {
        WAVEFORMATEXTENSIBLE x;
        memset(&x, 0, sizeof x);
        x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        x.Format.nChannels = (WORD)ch;
        x.Format.nSamplesPerSec = (DWORD)rate;
        x.Format.wBitsPerSample = 16;
        x.Format.nBlockAlign = (WORD)(2 * ch);
        x.Format.nAvgBytesPerSec = (DWORD)(2 * ch * rate);
        x.Format.cbSize = 22;
        x.Samples.wValidBitsPerSample = 16;
        x.dwChannelMask = ch == 6 ? 0x3F : 0x3;   /* FL FR FC LFE BL BR */
        x.SubFormat = SUBTYPE_PCM;
        if (waveOutOpen(&wo, WAVE_MAPPER, &x.Format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            /* no 5.1 output: mix to stereo here */
            x.Format.nChannels = 2; x.Format.nBlockAlign = 4; x.Format.nAvgBytesPerSec = (DWORD)(4 * rate);
            x.dwChannelMask = 0x3;
            out_ch = 2;
            if (waveOutOpen(&wo, WAVE_MAPPER, &x.Format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) wo = NULL;
        }
    }
    if (!wo) have_audio = 0;
    for (i = 0; i < NBUF; i++) {
        buf[i] = (int16_t *)malloc(sizeof(int16_t) * 6 * BUF_FRAMES);
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)buf[i];
        hdr[i].dwFlags = WHDR_DONE;          /* free to fill */
    }
    QueryPerformanceCounter(&q0);
    while (!P.stop) {
        double clock;
        /* sound: refill the buffers that came back */
        if (have_audio) {
            for (i = 0; i < NBUF && !audio_end; i++) {
                int got;
                if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
                if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
                if (primed > 0) {
                    memcpy(buf[i], first, (size_t)primed * ch * 2);
                    got = primed;
                    primed = 0;
                } else {
                    got = media_audio(m, buf[i], BUF_FRAMES);
                }
                if (got <= 0) { audio_end = 1; break; }
                if (out_ch == 2 && ch == 6) {   /* FL FR FC LFE BL BR -> L R */
                    int k;
                    for (k = 0; k < got; k++) {
                        const int16_t *s = buf[i] + k * 6;
                        int l = (int)(0.5 * s[0] + 0.354 * s[2] + 0.354 * s[4] + 0.25 * s[3]);
                        int r = (int)(0.5 * s[1] + 0.354 * s[2] + 0.354 * s[5] + 0.25 * s[3]);
                        buf[i][k * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                        buf[i][k * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
                    }
                }
                hdr[i].dwBufferLength = (DWORD)(got * out_ch * 2);
                hdr[i].dwFlags = 0;
                waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
                waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
            }
        }
        /* the clock: the sound played so far, or the wall clock without sound */
        if (have_audio) {
            MMTIME t;
            t.wType = TIME_SAMPLES;
            waveOutGetPosition(wo, &t, sizeof t);
            clock = (double)(played_base + t.u.sample) / rate;
            if (audio_end) {   /* past the sound's end the picture runs on the wall clock */
                int all_done = 1;
                for (i = 0; i < NBUF; i++) if (!(hdr[i].dwFlags & WHDR_DONE)) all_done = 0;
                if (all_done) {
                    QueryPerformanceCounter(&qn);
                    if (!P.paused) clock = (double)(qn.QuadPart - q0.QuadPart) / qf.QuadPart;
                    have_audio = 0;
                    q0.QuadPart = qn.QuadPart - (LONGLONG)(clock * qf.QuadPart);
                }
            }
        } else {
            QueryPerformanceCounter(&qn);
            if (P.paused) { q0.QuadPart = qn.QuadPart - (LONGLONG)(P.clock * qf.QuadPart); clock = P.clock; }
            else clock = (double)(qn.QuadPart - q0.QuadPart) / qf.QuadPart;
        }
        P.clock = clock;
        /* the picture */
        if (!pending && !video_end) {
            if (media_next_frame(m, &ft)) pending = 1;
            else video_end = 1;
        }
        if (pending && ft <= clock) {
            if (clock - ft < 2.0 / fps || !P.have_pic) {   /* late frames are skipped, not shown */
                EnterCriticalSection(&P.lock);
                media_frame_bgrx(m, P.px, P.vw * 4);
                P.have_pic = 1;
                LeaveCriticalSection(&P.lock);
                if (P.host) InvalidateRect(P.host, NULL, FALSE);
            }
            pending = 0;
            continue;
        }
        if (video_end && (audio_end || !have_audio)) {
            int all_done = 1;
            if (have_audio) for (i = 0; i < NBUF; i++) if (!(hdr[i].dwFlags & WHDR_DONE)) all_done = 0;
            if (all_done) break;
        }
        if (P.paused) {
            if (wo) waveOutPause(wo);
            while (P.paused && !P.stop) Sleep(20);
            if (wo) waveOutRestart(wo);
        }
        Sleep(2);
    }
    if (wo) {
        waveOutReset(wo);
        for (i = 0; i < NBUF; i++) if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
        waveOutClose(wo);
    }
    for (i = 0; i < NBUF; i++) free(buf[i]);
    free(first);
    InterlockedExchange(&P.done, 1);
    if (!P.stop && P.host) PostMessageA(P.host, WM_APP_EXTRAS, 0, 0);   /* the end: back to the page */
    return 0;
}

/* config.exe: its window full screen on its monitor while a video plays (F11, Enter or a double-click), and back */
static void set_fullscreen(HWND w, int on)
{
    if (!w || on == P.fullscreen) return;
    if (on) {
        MONITORINFO mi;
        P.place.length = sizeof P.place;
        GetWindowPlacement(w, &P.place);
        P.style = GetWindowLongA(w, GWL_STYLE);
        mi.cbSize = sizeof mi;
        GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(w, GWL_STYLE, P.style);
        SetWindowPlacement(w, &P.place);
        SetWindowPos(w, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    P.fullscreen = on;
}

static void player_stop(void)
{
    if (!P.active) return;
    InterlockedExchange(&P.stop, 1);
    if (P.thread) { WaitForSingleObject(P.thread, 5000); CloseHandle(P.thread); P.thread = NULL; }
    if (P.fullscreen) set_fullscreen(P.host, 0);
    media_close(P.m);
    P.m = NULL;
    EnterCriticalSection(&P.lock);
    free(P.px);
    P.px = NULL;
    P.have_pic = 0;
    LeaveCriticalSection(&P.lock);
    free(P.subs);
    P.subs = NULL;
    P.nsubs = 0;
    P.active = 0;
    if (P.host) InvalidateRect(P.host, NULL, FALSE);
}

/* 1 while a video plays; a video that ended is closed here, back to the page. */
int extras_playing(void)
{
    if (P.active && P.done && !P.stop) player_stop();
    return P.active;
}

void extras_stop(void) { player_stop(); }

static void player_hud(HDC dc, const RECT *area);

void extras_draw_player(HDC dc, const RECT *area)
{
    RECT vr;
    int cw = area->right - area->left, chh = area->bottom - area->top, vw, vh;
    FillRect(dc, area, (HBRUSH)GetStockObject(BLACK_BRUSH));
    /* the video's own shape: the disc's 640x480 is 4:3 with square pixels, and a re-encoded video keeps only the
     * picture's area at the same square pixels (media_encode_mp4) */
    {
        int sw = P.vw > 0 ? P.vw : 4, sh = P.vh > 0 ? P.vh : 3;
        vw = cw; vh = (int)((long long)cw * sh / sw);
        if (vh > chh) { vh = chh; vw = (int)((long long)chh * sw / sh); }
    }
    SetRect(&vr, area->left + (cw - vw) / 2, area->top + (chh - vh) / 2, area->left + (cw - vw) / 2 + vw,
            area->top + (chh - vh) / 2 + vh);
    EnterCriticalSection(&P.lock);
    if (P.have_pic && P.px) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = P.vw;
        bi.bmiHeader.biHeight = -P.vh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, vr.left, vr.top, vw, vh, 0, 0, P.vw, P.vh, P.px, &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    LeaveCriticalSection(&P.lock);
    SetBkMode(dc, TRANSPARENT);
    if (P.sub_on && P.nsubs) {   /* the subtitle on screen now */
        int ms = (int)(P.clock * 1000), k;
        for (k = 0; k < P.nsubs; k++)
            if (ms >= P.subs[k].start && ms < P.subs[k].end) {
                HFONT f = CreateFontA(-(vh / 18), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, 0, "Segoe UI");
                HGDIOBJ of = SelectObject(dc, f);
                RECT tr, sh;
                SetRect(&tr, vr.left + vw / 12, vr.top + vh * 3 / 4, vr.right - vw / 12, vr.bottom - vh / 30);
                sh = tr;
                OffsetRect(&sh, 2, 2);
                SetTextColor(dc, RGB(0, 0, 0));
                DrawTextW(dc, P.subs[k].text, -1, &sh, DT_CENTER | DT_BOTTOM | DT_WORDBREAK);
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(dc, P.subs[k].text, -1, &tr, DT_CENTER | DT_BOTTOM | DT_WORDBREAK);
                SelectObject(dc, of);
                DeleteObject(f);
                break;
            }
    }
    if (GetTickCount() < P.hint_until || P.paused)
        player_hud(dc, area);
}

/* One control in the player's bar: the key in a small panel, as on the CONTROLS page, then what it does.
 * Returns the x after it. */
static int hud_key(HDC dc, const UiFonts *f, int x, int y, int h, const char *key, const char *what)
{
    RECT k, t;
    SIZE sz;
    HGDIOBJ of = SelectObject(dc, f->key);
    GetTextExtentPoint32A(dc, key, (int)strlen(key), &sz);
    SelectObject(dc, of);
    SetRect(&k, x, y, x + sz.cx + ui_px(f, 18), y + h);
    ui_chamfer(dc, &k, ui_px(f, 5), RGB(20, 74, 140), RGB(130, 206, 255));
    ui_text(dc, f->key, RGB(242, 246, 251), &k, key, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    of = SelectObject(dc, f->body);
    GetTextExtentPoint32A(dc, what, (int)strlen(what), &sz);
    SelectObject(dc, of);
    SetRect(&t, k.right + ui_px(f, 8), y, k.right + ui_px(f, 8) + sz.cx + 2, y + h);
    ui_text(dc, f->body, RGB(210, 224, 240), &t, what, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return t.right + ui_px(f, 22);
}

/* The player's controls and notices in the menus' look (halo_ui.c): a panel along the bottom with the video's title,
 * how far it is and the keys, and a PAUSED panel in the middle while paused. Shown for a moment after a key or a
 * move of the pointer, and all the time while paused. */
static void player_hud(HDC dc, const RECT *area)
{
    UiFonts f;
    RECT bar, r, track, fill;
    int cw = area->right - area->left, chh = area->bottom - area->top, x, ky, kh, secs;
    float sc = (float)chh / 640.0f;
    char t[160];
    HBRUSH b;
    double prog = P.m ? media_progress(P.m) : 0;
    int subs = P.nsubs || P.sub_lang >= 0;

    if ((float)cw / 960.0f < sc) sc = (float)cw / 960.0f;
    ui_fonts_make(&f, sc);
    SetBkMode(dc, TRANSPARENT);

    /* PAUSED, in the middle */
    if (P.paused) {
        RECT p;
        int pw = ui_px(&f, 220), ph = ui_px(&f, 64);
        SetRect(&p, area->left + (cw - pw) / 2, area->top + (chh - ph) / 2, area->left + (cw + pw) / 2,
                area->top + (chh + ph) / 2);
        ui_chamfer(dc, &p, ui_px(&f, 14), RGB(6, 24, 54), RGB(70, 140, 214));
        ui_text(dc, f.title, RGB(242, 246, 251), &p, "PAUSED", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* the bar: title and time on the left, the keys under them, the progress along the bottom */
    SetRect(&bar, area->left + ui_px(&f, 16), area->bottom - ui_px(&f, 16) - ui_px(&f, 92), area->right - ui_px(&f, 16),
            area->bottom - ui_px(&f, 16));
    ui_chamfer(dc, &bar, ui_px(&f, 16), RGB(6, 24, 54), RGB(70, 140, 214));
    secs = (int)P.clock;
    snprintf(t, sizeof t, "%s", VIDEOS[P.video].title);
    SetRect(&r, bar.left + ui_px(&f, 20), bar.top + ui_px(&f, 8), bar.right - ui_px(&f, 160), bar.top + ui_px(&f, 34));
    ui_text(dc, f.head, RGB(242, 246, 251), &r, t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (P.sub_lang >= 0 && P.sub_on) snprintf(t, sizeof t, "Subtitles: %s   %d:%02d", SUB_NAME[P.sub_lang], secs / 60, secs % 60);
    else if (P.sub_lang == -1) snprintf(t, sizeof t, "Subtitles off   %d:%02d", secs / 60, secs % 60);
    else snprintf(t, sizeof t, "%d:%02d", secs / 60, secs % 60);
    SetRect(&r, bar.right - ui_px(&f, 380), bar.top + ui_px(&f, 8), bar.right - ui_px(&f, 20), bar.top + ui_px(&f, 34));
    ui_text(dc, f.body, RGB(130, 206, 255), &r, t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    ky = bar.top + ui_px(&f, 40);
    kh = ui_px(&f, 26);
    x = bar.left + ui_px(&f, 20);
    if (s_ingame) {
        x = hud_key(dc, &f, x, ky, kh, "SPACE / A", P.paused ? "Play" : "Pause");
        if (subs) x = hud_key(dc, &f, x, ky, kh, "S / X", "Subtitles");
        x = hud_key(dc, &f, x, ky, kh, "ESC / B", "Back to the menu");
    } else {
        x = hud_key(dc, &f, x, ky, kh, "SPACE", P.paused ? "Play" : "Pause");
        if (subs) x = hud_key(dc, &f, x, ky, kh, "S", "Subtitles");
        x = hud_key(dc, &f, x, ky, kh, "F11", P.fullscreen ? "Window" : "Full screen");
        x = hud_key(dc, &f, x, ky, kh, "ESC", P.fullscreen ? "Leave full screen" : "Close");
    }

    /* how far through: a thin bar along the panel's bottom edge */
    SetRect(&track, bar.left + ui_px(&f, 20), bar.bottom - ui_px(&f, 14), bar.right - ui_px(&f, 20),
            bar.bottom - ui_px(&f, 10));
    b = CreateSolidBrush(RGB(16, 44, 86));
    FillRect(dc, &track, b);
    DeleteObject(b);
    fill = track;
    if (prog < 0) prog = 0;
    if (prog > 1) prog = 1;
    fill.right = track.left + (int)((track.right - track.left) * prog);
    if (fill.right > fill.left) {
        b = CreateSolidBrush(RGB(130, 206, 255));
        FillRect(dc, &fill, b);
        DeleteObject(b);
    }
    ui_fonts_free(&f);
}

/* A key while a video plays: 1 when the player took it. */
int extras_key(int vk)
{
    if (!extras_playing()) return 0;
    P.hint_until = GetTickCount() + 2500;
    if (vk == VK_ESCAPE) {
        if (P.fullscreen) set_fullscreen(P.host, 0);
        else player_stop();
    } else if ((vk == VK_F11 || vk == VK_RETURN) && !s_ingame) {
        set_fullscreen(P.host, !P.fullscreen);
    } else if (vk == VK_SPACE) {
        InterlockedExchange(&P.paused, !P.paused);
    } else if (vk == 'S' && P.sub_lang >= -1) {   /* off, then each language in turn */
        int n = (int)(sizeof SUB_FILES / sizeof *SUB_FILES);
        P.sub_lang = P.sub_on ? (P.sub_lang + 1 < n ? P.sub_lang + 1 : -1) : 0;
        P.sub_on = P.sub_lang >= 0;
        subs_load(P.sub_lang);
    }
    if (P.host) InvalidateRect(P.host, NULL, FALSE);
    return 1;
}

/* The pointer while a video plays: a move shows the hint, a double-click (config.exe) goes full screen. */
void extras_player_mouse(int dblclick)
{
    if (!P.active) return;
    P.hint_until = GetTickCount() + 2500;
    if (dblclick && !s_ingame) set_fullscreen(P.host, !P.fullscreen);
}

static void play(HWND owner, int i)
{
    char f[3][MAX_PATH];
    WCHAR wf[3][MAX_PATH];
    const WCHAR *wp[3];
    int mp4, n, k;
    static int init;
    if (P.active) return;
    n = video_files(i, i == 0 ? beach_lang() : "", f, &mp4);
    if (!n) return;
    for (k = 0; k < n; k++) {
        MultiByteToWideChar(CP_ACP, 0, f[k], -1, wf[k], MAX_PATH);
        wp[k] = wf[k];
    }
    if (!init) { InitializeCriticalSection(&P.lock); init = 1; }
    P.m = media_open(wp, n);
    if (!P.m) {
        if (!s_ingame) MessageBoxA(owner, "This video could not be opened.", "Halo 2 Recomp", MB_ICONERROR);
        return;
    }
    P.host = s_ingame ? NULL : owner;   /* the game's menu repaints itself 60 times a second */
    P.video = i;
    P.vw = media_width(P.m);
    P.vh = media_height(P.m);
    P.px = (uint8_t *)calloc((size_t)P.vw * P.vh, 4);
    P.have_pic = 0; P.stop = 0; P.paused = 0; P.done = 0; P.fullscreen = 0; P.clock = 0;
    P.hint_until = GetTickCount() + 4000;
    P.sub_lang = -1; P.sub_on = 0; P.nsubs = 0;
    if (VIDEOS[i].subtitles) {   /* Killtacular is spoken in English: subtitles in the display language, if any */
        const char *ul = ui_lang();
        for (k = 0; k < (int)(sizeof SUB_LANG / sizeof *SUB_LANG); k++)
            if (!strcmp(ul, SUB_LANG[k])) { P.sub_lang = k; P.sub_on = 1; subs_load(k); }
        if (!P.sub_on) P.sub_lang = -1;
    } else {
        P.sub_lang = -2;   /* no subtitles for this video */
    }
    P.active = 1;
    P.thread = CreateThread(NULL, 0, player_thread, NULL, 0, NULL);
    if (P.host) InvalidateRect(P.host, NULL, FALSE);
}

void extras_play(int i)
{
    if (i >= 0 && i < NVID && s_vstate[i]) play(s_owner, i);
}

/* ---- clicks ------------------------------------------------------------------------------------------------------ */
int extras_click(HWND owner, int x, int y)
{
    POINT p;
    int i;
    p.x = x; p.y = y;
    for (i = 0; i < NVID; i++)
        if (PtInRect(&s_play_r[i], p)) { s_sel = i; play(owner ? owner : s_owner, i); return 1; }
    if (PtInRect(&s_add_r, p)) {
        if (s_job == JOB_COPY) extras_cancel();
        else if (!extras_busy()) add_disc(owner);
        return 1;
    }
    if (PtInRect(&s_enc_r, p)) {
        if (s_job == JOB_ENCODE) extras_cancel();
        else if (!extras_busy() &&
                 MessageBoxA(owner, "Re-encode the bonus videos to H.264?\n\n"
                                    "This takes several minutes. Each video's original file is removed once its "
                                    "H.264 copy is complete; you can copy the originals again from the disc later.",
                             "Bonus videos", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            s_cancel = 0;
            memset(&s_enc, 0, sizeof s_enc);
            if (s_job_thread) { CloseHandle(s_job_thread); s_job_thread = NULL; }
            InterlockedExchange(&s_job, JOB_ENCODE);
            s_job_thread = CreateThread(NULL, 0, encode_thread, NULL, 0, NULL);
        }
        return 1;
    }
    return 0;
}
