/*
 * update_check.c -- see update_check.h.
 *
 * GET https://api.github.com/repos/<H2N_UPDATE_REPO>/releases/latest, read
 * "tag_name" and the first .zip asset's "browser_download_url". Nothing about
 * the player is sent: a fixed User-Agent and the request. With no repository
 * set (H2N_UPDATE_REPO "", until releases exist) nothing is contacted.
 *
 * Testing: RECOMP_UPDATE_REPO=owner/name checks another repository;
 * RECOMP_UPDATE_FAKE=1.2.3 shows that version as available (no download).
 */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "h2_layout.h"
#include "update_check.h"
#include "version.h"

#pragma comment(lib, "winhttp.lib")

#define CHECK_EVERY (2 * 60 * 60)          /* seconds */

static CRITICAL_SECTION s_lock;
static volatile LONG s_init, s_started;
static UpdInfo s_info;
static char s_ini[MAX_PATH];

static void lock_init(void)
{
    if (InterlockedCompareExchange(&s_init, 1, 0) == 0)
        InitializeCriticalSection(&s_lock);
    else
        while (s_init != 1) Sleep(0);
}

/* "v1.2.3" or "1.2" -> comparable parts. */
static void parse_ver(const char *s, int v[3])
{
    int i;
    v[0] = v[1] = v[2] = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    for (i = 0; i < 3 && *s; i++) {
        v[i] = atoi(s);
        while (*s >= '0' && *s <= '9') s++;
        if (*s != '.') break;
        s++;
    }
}

static int newer(const char *tag)
{
    int a[3], b[3], i;
    parse_ver(tag, a);
    parse_ver(H2N_VERSION, b);
    for (i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

/* The string value after "key": in a JSON text, from p on. */
static const char *json_str(const char *p, const char *key, char *out, size_t n)
{
    char pat[64];
    const char *q;
    size_t i = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    if (!(q = strstr(p, pat))) return NULL;
    q += strlen(pat);
    while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (*q != '"') return NULL;
    q++;
    while (*q && *q != '"' && i + 1 < n) {
        if (*q == '\\' && q[1]) q++;
        out[i++] = *q++;
    }
    out[i] = 0;
    return q;
}

static int fetch(const char *repo, char *buf, size_t cap)
{
    wchar_t path[256];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD got = 0, n, status = 0, sl = sizeof status;
    size_t len = 0;
    int ok = 0;
    swprintf(path, 256, L"/repos/%hs/releases/latest", repo);
    ses = WinHttpOpen(L"Halo2Native-UpdateCheck/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto done;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 8000);
    if (!(con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0))) goto done;
    if (!(req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))) goto done;
    if (!WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) goto done;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { ok = status == 404 ? 2 : 0; goto done; }   /* 404: no release yet */
    while (len + 1 < cap && WinHttpQueryDataAvailable(req, &n) && n) {
        if (n > cap - 1 - len) n = (DWORD)(cap - 1 - len);
        if (!WinHttpReadData(req, buf + len, n, &got) || !got) break;
        len += got;
    }
    buf[len] = 0;
    ok = len > 0;
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

static void store(int state, const char *latest, const char *url)
{
    EnterCriticalSection(&s_lock);
    s_info.state = state;
    snprintf(s_info.latest, sizeof s_info.latest, "%s", latest ? latest : "");
    snprintf(s_info.url, sizeof s_info.url, "%s", url ? url : "");
    LeaveCriticalSection(&s_lock);
}

static DWORD WINAPI check_thread(LPVOID arg)
{
    const char *repo = (const char *)arg;
    static char json[256 * 1024];
    char tag[32] = "", url[512] = "", now_s[32];
    const char *p;
    int r;

    r = fetch(repo, json, sizeof json);
    if (r == 1 && json_str(json, "tag_name", tag, sizeof tag)) {
        /* the first .zip asset; one named "...-update.zip" wins */
        char u[512];
        for (p = json; (p = json_str(p, "browser_download_url", u, sizeof u)) != NULL; ) {
            size_t l = strlen(u);
            if (l > 4 && !_stricmp(u + l - 4, ".zip") &&
                (!url[0] || strstr(u, "-update.zip"))) strcpy_s(url, sizeof url, u);
        }
    }
    snprintf(now_s, sizeof now_s, "%lld", (long long)time(NULL));
    if (r == 0) {
        store(UPD_FAILED, NULL, NULL);         /* offline: try again at the next start */
        return 0;
    }
    WritePrivateProfileStringA("update", "last_check", now_s, s_ini);
    WritePrivateProfileStringA("update", "latest", tag, s_ini);
    WritePrivateProfileStringA("update", "url", url, s_ini);
    store(tag[0] && newer(tag) && url[0] ? UPD_AVAILABLE : UPD_CURRENT, tag, url);
    return 0;
}

void upd_start(const char *root)
{
    static char repo[128];
    const char *e;
    char last[32], tag[32], url[512];
    long long then;

    lock_init();
    if (InterlockedExchange(&s_started, 1)) return;
    h2_data_path(root, "update.ini", s_ini, sizeof s_ini);
    if ((e = getenv("RECOMP_UPDATE_FAKE")) != NULL && *e) {
        store(UPD_AVAILABLE, e, "");
        return;
    }
    e = getenv("RECOMP_UPDATE_REPO");
    snprintf(repo, sizeof repo, "%s", e && *e ? e : H2N_UPDATE_REPO);
    if (!repo[0] || !strchr(repo, '/')) { store(UPD_OFF, NULL, NULL); return; }

    /* Checked less than 2 hours ago (by the game or Config.exe): use that answer. */
    GetPrivateProfileStringA("update", "last_check", "0", last, sizeof last, s_ini);
    then = _atoi64(last);
    if (then > 0 && time(NULL) - then < CHECK_EVERY && time(NULL) >= then) {
        GetPrivateProfileStringA("update", "latest", "", tag, sizeof tag, s_ini);
        GetPrivateProfileStringA("update", "url", "", url, sizeof url, s_ini);
        store(tag[0] && newer(tag) && url[0] ? UPD_AVAILABLE : UPD_CURRENT, tag, url);
        return;
    }
    store(UPD_CHECKING, NULL, NULL);
    {
        HANDLE t = CreateThread(NULL, 0, check_thread, repo, 0, NULL);
        if (t) CloseHandle(t); else store(UPD_FAILED, NULL, NULL);
    }
}

void upd_get(UpdInfo *out)
{
    lock_init();
    EnterCriticalSection(&s_lock);
    *out = s_info;
    LeaveCriticalSection(&s_lock);
}

int upd_launch(const char *root, const char *relaunch)
{
    char src[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], cmd[2048];
    UpdInfo u;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    upd_get(&u);
    if (u.state != UPD_AVAILABLE || !u.url[0]) return 0;
    /* The updater runs from SaveData\Update so build-ninja\ can be replaced. */
    h2_bin_path(root, "Halo2 Updater.exe", src, sizeof src);
    h2_data_path(root, "", dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    h2_data_path(root, "Update", dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    snprintf(exe, sizeof exe, "%s\\Halo2 Updater.exe", dir);
    if (!CopyFileA(src, exe, FALSE)) return 0;
    snprintf(cmd, sizeof cmd, "\"%s\" --pid %lu --root \"%s\" --version \"%s\" --url \"%s\" --relaunch \"%s\"",
             exe, (unsigned long)GetCurrentProcessId(), root, u.latest, u.url, relaunch ? relaunch : "");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}
