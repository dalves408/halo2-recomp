/*
 * h2_account.c -- recover a player's Insignia account through Halo 2's own XOnline.
 *
 * Ported from CounterStrikeXB's cs_account.c (same author, MIT). The title's
 * library signs the console in (no players, account service only) and posts
 * the dashboard's restoreaccount request; the host builds the account record
 * from the reply, signs it with the console's hard-drive key and the library's
 * K1/K2 (checked offline against a real console's drive), and writes it into a
 * slot of the drive's config area with the library's own sector functions.
 *
 * H2_ACCT=recover runs it: the gamertag and the 16-digit recovery number come
 * on the pipe named by H2_ACCT_INPUT (two lines), never a file, argument or
 * variable. The number is never logged and is wiped once sent. The process
 * ends with a result code (h2_account.h). H2_ACCT=reserve + H2_ACCT_NAME asks
 * whether a gamertag is free: a harmless test of the whole path.
 *
 * Halo 2 1.5 (cert 0x803) addresses:
 *   0x0049102C  the library's object (CXo); 0 until XOnlineStartup
 *   0x003AA721  XOnlineLogon(users[4], services, count, event, &task)
 *   0x003AA700  XOnlineTaskContinue(task): 0 while running, then the result
 *   0x003AA70B  XOnlineTaskClose(task)
 *   0x003C44EF  CXo XRL post (thiscall, 12 arguments; the service-address
 *               override is the 2nd here, the last in CS's library)
 *   0x003D9AB6  config sector read (handle, index, buffer, 0x1EC): nonzero = ok
 *   0x003D9B6D  config sector write (same arguments)
 *   0x0057AB4C, 0x0057AB38  K1, K2 (XON_RD)
 *   Config-sector indexes: 3 = the user account directory (8 entries of 12
 *   bytes: in-use dword, 8-byte confounder), 4..11 = the account slots.
 */
#define _CRT_RAND_S
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen/recomp_types.h"
#include "h2_account.h"

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
int xbox_ConsoleHdKey(uint8_t out[16]);
VOID __stdcall xbox_XcHMAC(const UCHAR *Key, ULONG KeyLength, const UCHAR *Data1, ULONG Data1Length,
                           const UCHAR *Data2, ULONG Data2Length, UCHAR *Digest);
VOID __stdcall xbox_XcDESKeyParity(PUCHAR Key, ULONG KeyLength);
VOID __stdcall xbox_XcKeyTable(ULONG CipherSelect, PVOID KeyTable, const UCHAR *Key);
VOID __stdcall xbox_XcBlockCryptCBC(ULONG CipherSelect, ULONG OutputLength, PVOID Output, PVOID Input,
                                    PVOID KeyTable, ULONG Operation, PVOID FeedbackVector);

#define VA_CXO           0x0049102Cu
#define VA_LOGON         0x003AA721u
#define VA_TASK_CONTINUE 0x003AA700u
#define VA_TASK_CLOSE    0x003AA70Bu
#define VA_XRL_POST      0x003C44EFu
#define VA_SECTOR_READ   0x003D9AB6u
#define VA_SECTOR_WRITE  0x003D9B6Du
#define VA_K1            0x0057AB4Cu
#define VA_K2            0x0057AB38u
#define THUNK_KEDELAY    0x0041B740u
#define THUNK_PSCREATE   0x0041B6A4u
#define THUNK_KEQST      0x0041B668u   /* KeQuerySystemTime: the step tick */
#define CXO_CONFIG       0x94u         /* CS: 0x88 */
#define SERVICE_ACCOUNT  5u
#define COUNTRY_US       103u
#define FAKE_RETURN      0xFFFF0000u
#define SECTOR_DATA      0x1ECu
#define RECORD_SIZE      0x6Cu
#define REPLY_SIZE       0x400u

enum { MODE_NONE, MODE_RESERVE, MODE_RECOVER };
enum { ST_OFF, ST_WAIT, ST_LOGON, ST_SEND, ST_REPLY, ST_SAVE, ST_DONE };
static int s_mode, s_state = ST_OFF, s_result = H2_ACCT_ERR_OTHER;
static ULONGLONG s_t0;
static volatile LONG s_busy, s_logon_called;
static uint32_t s_users, s_services, s_task_slot, s_xrl, s_body, s_resp, s_body_len;
static uint32_t s_machine_off = 0x740; /* CXo machine account id (CS: 0x1C); H2_CXO_MACHINE_ID */
static char s_name[32], s_card[32];
static uint8_t s_machine[8];        /* the machine account id, from the drive */
#define SNAP_DW 4096
static uint32_t s_snap[SNAP_DW];
static unsigned s_delay_ms = 20000;   /* H2_ACCT_DELAY_MS */

static void finish(void);

/* Debug: with a RECOMP_CALL_PROFILE build, the direct calls a step makes are
 * recorded (repeats of the same target collapsed) and written next to the
 * session log, to diff a run that works against one that does not. */
static __declspec(thread) int t_trace;
static uint32_t s_trace[60000];
static unsigned s_ntrace;
/* Every thread's XNet-range and kernel calls while s_trace_any is set, each
 * run of one thread's calls headed by EEEE0000|thread id. */
static volatile LONG s_trace_any, s_nany;
static int s_any_from;
static uint32_t s_any[200000];
static volatile DWORD s_any_last_tid;
void recomp_call_profile_hit(uint32_t va)
{
    if (t_trace && s_ntrace < sizeof s_trace / sizeof s_trace[0] && (!s_ntrace || s_trace[s_ntrace - 1] != va))
        s_trace[s_ntrace++] = va;
    if (s_trace_any && ((va >= 0x003D7000u && va < 0x003EA000u) || va >= 0xFE000000u)) {
        DWORD tid = GetCurrentThreadId();
        LONG i;
        if (tid != s_any_last_tid) {
            s_any_last_tid = tid;
            i = InterlockedIncrement(&s_nany) - 1;
            if (i < (LONG)(sizeof s_any / sizeof s_any[0])) s_any[i] = 0xEEEE0000u | (tid & 0xFFFF);
        }
        i = InterlockedIncrement(&s_nany) - 1;
        if (i < (LONG)(sizeof s_any / sizeof s_any[0])) s_any[i] = va;
    }
}
static void trace_any_dump(void)
{
    FILE *f = fopen("build-ninja\\screenshots\\acct_trace_anythread.txt", "w");
    LONG i, n = s_nany < (LONG)(sizeof s_any / sizeof s_any[0]) ? s_nany : (LONG)(sizeof s_any / sizeof s_any[0]);
    if (f) { for (i = 0; i < n; i++) fprintf(f, "%08X\n", s_any[i]); fclose(f); }
    fprintf(stderr, "[ACCT] %ld any-thread calls traced (acct_trace_anythread.txt), account thread %lu\n",
            (long)n, (unsigned long)GetCurrentThreadId());
}
static void trace_dump(const char *what)
{
    char path[MAX_PATH];
    FILE *f;
    unsigned i;
    if (!s_ntrace) return;
    snprintf(path, sizeof path, "build-ninja\\screenshots\\acct_trace_%s_%llu.txt", what, (unsigned long long)GetTickCount64());
    f = fopen(path, "w");
    if (f) { for (i = 0; i < s_ntrace; i++) fprintf(f, "%08X\n", s_trace[i]); fclose(f); }
    fprintf(stderr, "[ACCT] %u calls traced to %s\n", s_ntrace, path);
    s_ntrace = 0;
}

/* ---- input ------------------------------------------------------------------ */
static int read_line(HANDLE h, char *out, unsigned cap)
{
    unsigned n = 0;
    char c;
    DWORD got;
    while (ReadFile(h, &c, 1, &got, NULL) && got == 1) {
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

static int read_recover_input(void)
{
    const char *h = getenv("H2_ACCT_INPUT");
    char raw[64];
    HANDLE pipe;
    unsigned i, n = 0;
    if (!h || !*h) return 0;
    pipe = (HANDLE)(uintptr_t)_strtoui64(h, NULL, 10);
    if (!read_line(pipe, s_name, sizeof s_name)) { CloseHandle(pipe); return 0; }
    read_line(pipe, raw, sizeof raw);
    CloseHandle(pipe);
    for (i = 0; raw[i] && n + 1 < sizeof s_card; i++)
        if (raw[i] >= '0' && raw[i] <= '9') s_card[n++] = raw[i];
    s_card[n] = 0;
    SecureZeroMemory(raw, sizeof raw);
    return n >= 12;
}

int h2_account_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *m = getenv("H2_ACCT"), *mo = getenv("H2_CXO_MACHINE_ID");
        on = 0;
        if (mo && *mo) s_machine_off = (uint32_t)strtoul(mo, NULL, 0);
        if (getenv("H2_ACCT_DELAY_MS")) s_delay_ms = (unsigned)strtoul(getenv("H2_ACCT_DELAY_MS"), NULL, 0);
        if (m && !_stricmp(m, "reserve")) {
            const char *n = getenv("H2_ACCT_NAME");
            if (n && *n && strlen(n) < sizeof s_name) {
                strcpy(s_name, n);
                s_mode = MODE_RESERVE;
                fprintf(stderr, "[ACCT] gamertag check\n");
            }
        } else if (m && !_stricmp(m, "recover")) {
            s_mode = MODE_RECOVER;
            if (!read_recover_input()) {
                fprintf(stderr, "[ACCT] account recovery: no gamertag and number on the input pipe\n");
                s_result = H2_ACCT_ERR_INPUT;
                s_state = ST_DONE;
                finish();
            }
            fprintf(stderr, "[ACCT] account recovery for a gamertag of %u characters\n", (unsigned)strlen(s_name));
        }
        if (s_mode != MODE_NONE) { s_state = ST_WAIT; on = 1; }
    }
    return on;
}

/* ---- calling guest code ------------------------------------------------------ */
static uint32_t gcall(uint32_t va, uint32_t ecx, int n, const uint32_t *args)
{
    recomp_func_t f = recomp_lookup(va);
    uint32_t esp = g_esp;
    int i;
    if (!f) f = recomp_lookup_kernel(va);
    if (!f) { fprintf(stderr, "[ACCT] no translated function at %08X\n", va); return 0x80004005u; }
    for (i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = args[i]; }
    g_esp -= 4; MEM32(g_esp) = FAKE_RETURN;
    g_ecx = ecx;
    f();
    g_esp = esp;
    return g_eax;
}

static uint32_t galloc(uint32_t size)
{
    uint32_t va = xbox_HeapAlloc(size, 16), i;
    for (i = 0; va && i < size; i++) MEM8(va + i) = 0;
    return va;
}

static uint32_t gstring(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, va = galloc(n), i;
    for (i = 0; va && i < n; i++) MEM8(va + i) = (uint8_t)s[i];
    return va;
}

static void log_hr(const char *what, uint32_t hr) { fprintf(stderr, "[ACCT] %s: 0x%08X\n", what, hr); }

/* ---- request bodies ------------------------------------------------------------ */
static uint32_t put_string(uint32_t p, const char *s)
{
    uint32_t n = (uint32_t)strlen(s), i;
    MEM8(p) = (uint8_t)n; MEM8(p + 1) = (uint8_t)(n >> 8);
    for (i = 0; i < n; i++) MEM8(p + 2 + i) = (uint8_t)s[i];
    return p + 2 + n;
}

static uint32_t put_machine_id(uint32_t p, uint32_t cxo)
{
    int k;
    (void)cxo;
    for (k = 0; k < 8; k++) MEM8(p + k) = s_machine[k];
    return p + 8;
}

static void build_reserve_body(uint32_t cxo)
{
    uint32_t p = put_machine_id(s_body, cxo);
    MEM8(p) = (uint8_t)COUNTRY_US; p += 1;
    MEM8(p) = 5; MEM8(p + 1) = 0; p += 2;
    p = put_string(p, s_name);
    s_body_len = p - s_body;
}

/* The dashboard's recovery form: gamertag, phone area code, number, extension,
 * postal code, name, card number. Insignia checks the gamertag and its number. */
static void build_recover_body(uint32_t cxo)
{
    uint32_t p = put_machine_id(s_body, cxo);
    p = put_string(p, s_name);
    p = put_string(p, "555");
    p = put_string(p, "5555555");
    p = put_string(p, "");
    p = put_string(p, "98052");
    p = put_string(p, "Insignia Player");
    p = put_string(p, s_card);
    s_body_len = p - s_body;
    SecureZeroMemory(s_card, sizeof s_card);
}

static uint32_t reply_error(void)
{
    char t[REPLY_SIZE + 1];
    uint32_t i;
    char *e;
    for (i = 0; i < REPLY_SIZE; i++) { uint8_t c = MEM8(s_resp + i); t[i] = (char)(c ? c : '\n'); }
    t[REPLY_SIZE] = 0;
    e = strstr(t, "X-Err: ");
    return e ? (uint32_t)strtoul(e + 7, NULL, 16) : 0;
}

static void log_reply_headers(void)
{
    uint32_t i;
    fprintf(stderr, "[ACCT] reply: ");
    for (i = 0; i < 200; i++) { uint8_t c = MEM8(s_resp + i); fputc(c >= 0x20 && c < 0x7F ? c : ' ', stderr); }
    fputc('\n', stderr);
}

/* ---- the record ------------------------------------------------------------------ */
/* As the dashboard builds it from the restoreaccount reply: id (reply +0x00),
 * gamertag (+0x0C), Kerberos names (reply +0x18 -> +0x24, 20 bytes; reply +0x2C
 * -> +0x38, 24 bytes), account key (reply +0x08 -> +0x50), time saved (+0x60,
 * two-second units since 2000-01-01). */
static void build_record(uint8_t *rec)
{
    uint32_t i;
    FILETIME ft;
    unsigned long long t;
    memset(rec, 0, RECORD_SIZE);
    for (i = 0; i < 8; i++) rec[i] = MEM8(s_resp + i);
    for (i = 0; i < 16 && s_name[i]; i++) rec[0x0C + i] = (uint8_t)s_name[i];
    for (i = 0; i < 20; i++) rec[0x24 + i] = MEM8(s_resp + 0x18 + i);
    for (i = 0; i < 24; i++) rec[0x38 + i] = MEM8(s_resp + 0x2C + i);
    for (i = 0; i < 16; i++) rec[0x50 + i] = MEM8(s_resp + 0x08 + i);
    GetSystemTimeAsFileTime(&ft);
    t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + 0xFE40A38D010495A0ull;
    t /= 20000000ull;
    rec[0x60] = (uint8_t)t; rec[0x61] = (uint8_t)(t >> 8); rec[0x62] = (uint8_t)(t >> 16); rec[0x63] = (uint8_t)(t >> 24);
}

/* Sign as the library does (verified on a real console's drive): +0x64 = the
 * first 8 bytes of HMAC-SHA1(hd, record[0..0x63]) with the account key in the
 * clear; then the key at +0x50 is encrypted with 3DES-CBC, IV = the slot's
 * confounder, key = HMAC-SHA1(K1, hd)[0..3] | HMAC-SHA1(K2, hd). */
static int sign_record(uint8_t *rec, const uint8_t confounder[8])
{
    uint8_t hd[16], d[20], k1[16], k2[16], key24[24], iv[8], out[16], table[1024];
    int i;
    if (!xbox_ConsoleHdKey(hd)) return 0;
    for (i = 0; i < 16; i++) { k1[i] = MEM8(VA_K1 + i); k2[i] = MEM8(VA_K2 + i); }
    rec[0x1F] &= 0x0F;
    memset(rec + 8, 0, 4);
    xbox_XcHMAC(hd, 16, rec, 0x64, NULL, 0, d);
    memcpy(rec + 0x64, d, 8);
    xbox_XcHMAC(k1, 16, hd, 16, NULL, 0, d); memcpy(key24, d, 4);
    xbox_XcHMAC(k2, 16, hd, 16, NULL, 0, d); memcpy(key24 + 4, d, 20);
    xbox_XcDESKeyParity(key24, 24);
    xbox_XcKeyTable(1, table, key24);
    memcpy(iv, confounder, 8);
    xbox_XcBlockCryptCBC(1, 16, out, rec + 0x50, table, 1, iv);
    memcpy(rec + 0x50, out, 16);
    SecureZeroMemory(hd, sizeof hd); SecureZeroMemory(key24, sizeof key24);
    SecureZeroMemory(table, sizeof table); SecureZeroMemory(d, sizeof d); SecureZeroMemory(out, sizeof out);
    return 1;
}

static int sector_io(uint32_t va, uint32_t cxo, uint32_t index, uint32_t buf)
{
    uint32_t a[4] = { MEM32(cxo + CXO_CONFIG), index, buf, SECTOR_DATA };
    return gcall(va, 0, 4, a) != 0;
}

/* The dashboard's save: the slot already holding this account, else the first
 * free one; a fresh confounder, the record signed with it, slot and directory
 * written back. */
static int save_record(uint32_t cxo, uint8_t *rec)
{
    uint32_t dir = galloc(SECTOR_DATA), users = galloc(8 * SECTOR_DATA), slot = 8, i;
    uint8_t conf[8];
    unsigned int r;
    if (!dir || !users) return H2_ACCT_ERR_SAVE;
    if (!sector_io(VA_SECTOR_READ, cxo, 3, dir)) fprintf(stderr, "[ACCT] no account directory yet: starting one\n");
    for (i = 0; i < 8; i++)
        if (!sector_io(VA_SECTOR_READ, cxo, 4 + i, users + i * SECTOR_DATA)) {
            uint32_t k;
            for (k = 0; k < SECTOR_DATA; k++) MEM8(users + i * SECTOR_DATA + k) = 0;
        }
    for (i = 0; i < 8 && slot == 8; i++)
        if (MEM32(dir + 12 * i) && !memcmp((const void *)XBOX_PTR(users + i * SECTOR_DATA), rec, 8))
            slot = i;
    if (slot < 8) fprintf(stderr, "[ACCT] the account is already in slot %u: replacing it\n", slot);
    for (i = 0; i < 8 && slot == 8; i++)
        if (!MEM32(dir + 12 * i)) slot = i;
    if (slot == 8) return H2_ACCT_ERR_FULL;
    for (i = 0; i < 8; i += 4) { rand_s(&r); memcpy(conf + i, &r, 4); }
    if (!sign_record(rec, conf)) { fprintf(stderr, "[ACCT] no hard-drive key: cannot sign the account\n"); return H2_ACCT_ERR_SAVE; }
    {
        uint32_t ub = users + slot * SECTOR_DATA, de = dir + 12 * slot;
        for (i = 0; i < SECTOR_DATA; i++) MEM8(ub + i) = 0;
        for (i = 0; i < RECORD_SIZE; i++) MEM8(ub + i) = rec[i];
        for (i = 0; i < 8; i++) MEM8(de + 4 + i) = conf[i];
        MEM32(de) = 1;
        if (!sector_io(VA_SECTOR_WRITE, cxo, 4 + slot, ub) || !sector_io(VA_SECTOR_WRITE, cxo, 3, dir))
            return H2_ACCT_ERR_SAVE;
        for (i = 0; i < SECTOR_DATA; i++) MEM8(ub + i) = 0;
    }
    fprintf(stderr, "[ACCT] account saved in slot %u\n", slot);
    return H2_ACCT_OK;
}

/* ---- the steps ------------------------------------------------------------------- */
static void step(void)
{
    uint32_t cxo = MEM32(VA_CXO), hr;
    switch (s_state) {
    case ST_WAIT:
        s_users = galloc(4 * 0x70); s_services = galloc(16); s_task_slot = galloc(16);
        s_xrl = galloc(0x400); s_body = galloc(0x200); s_resp = galloc(REPLY_SIZE + 16);
        if (!s_users || !s_services || !s_task_slot || !s_xrl || !s_body || !s_resp) {
            fprintf(stderr, "[ACCT] out of guest memory\n"); s_state = ST_DONE; return;
        }
        MEM32(s_services) = SERVICE_ACCOUNT;
        fprintf(stderr, "[ACCT] buffers: users %08X services %08X task %08X xrl %08X esp %08X; CXo+C8 %08X\n",
                s_users, s_services, s_task_slot, s_xrl, g_esp, MEM32(cxo + 0xC8));
        {
            uint32_t a[5] = { s_users, s_services, 1, 0, s_task_slot };
            t_trace = 1;
            {   /* snapshot of CXo before sign-in, diffed after (machine id hunt) */
                uint32_t k;
                for (k = 0; k < SNAP_DW; k++) s_snap[k] = MEM32(cxo + 4 * k);
            }
            hr = gcall(VA_LOGON, 0, 5, a);
            t_trace = 0;
            trace_dump("logon");
            InterlockedExchange(&s_logon_called, 1);
        }
        log_hr("signing in to Xbox Live (console only, account service)", hr);
        fprintf(stderr, "[ACCT] logon task %08X, CXo %08X\n", MEM32(s_task_slot), cxo);
        {
            uint32_t lo = MEM32(cxo + 0xC8), t = MEM32(s_task_slot);
            if (lo) fprintf(stderr, "[ACCT] logon object %08X: +0C %08X +10 %08X +14 %08X +A4 %08X\n",
                            lo, MEM32(lo + 0xC), MEM32(lo + 0x10), MEM32(lo + 0x14), MEM32(lo + 0xA4));
            if (t) {
                uint32_t sub = MEM32(t + 0xB4);
                fprintf(stderr, "[ACCT] task %08X: %08X %08X %08X %08X %08X %08X; +08 %08X +4C %08X +B4 %08X\n", t,
                        MEM32(t), MEM32(t + 4), MEM32(t + 8), MEM32(t + 0xC), MEM32(t + 0x10), MEM32(t + 0x14),
                        MEM32(t + 8), MEM32(t + 0x4C), sub);
                if (sub) fprintf(stderr, "[ACCT] subtask %08X: %08X %08X %08X %08X %08X %08X\n", sub,
                                 MEM32(sub), MEM32(sub + 4), MEM32(sub + 8), MEM32(sub + 0xC), MEM32(sub + 0x10), MEM32(sub + 0x14));
            }
        }
        if ((int32_t)hr < 0) { s_result = H2_ACCT_ERR_LOGON; s_state = ST_DONE; return; }
        s_state = ST_LOGON;
        return;
    case ST_LOGON: {
        uint32_t a[1] = { MEM32(s_task_slot) }, o;
        static int polls;
        if (polls < 3) {
            uint32_t t = MEM32(s_task_slot), sub = t ? MEM32(t + 0xB4) : 0, k;
            char row[200]; int n = 0;
            for (k = 0; sub && k < 0x40 && n < 180; k += 4) n += snprintf(row + n, sizeof row - n, " %08X", MEM32(sub + k));
            fprintf(stderr, "[ACCT] poll %d subtask %08X:%s\n", polls, sub, row);
        }
        t_trace = 1;
        hr = gcall(VA_TASK_CONTINUE, 0, 1, a);
        t_trace = 0;
        if (hr != 0) {
            char nm[32];
            uint32_t xn = MEM32(0x0049103Cu), tabs[2], k, e;
            snprintf(nm, sizeof nm, "final_poll%d", polls); trace_dump(nm);
            tabs[0] = MEM32(VA_CXO); tabs[1] = xn ? MEM32(xn + 0xAEC) : 0;
            fprintf(stderr, "[ACCT] final: CXo %08X, XNet %08X, XNet+AEC %08X\n", tabs[0], xn, tabs[1]);
            for (k = 0; k < 2; k++)
                for (e = 0; tabs[k] && e < 16; e++) {
                    uint32_t p = tabs[k] + 0xD4 + e * 0x14;
                    if (MEM32(p) || MEM32(p + 4) || MEM32(p + 0x10))
                        fprintf(stderr, "[ACCT] %s service %2u: id %08X addr %08X +08 %08X flag %08X ctx %08X\n",
                                k ? "AEC" : "CXo", e, MEM32(p), MEM32(p + 4), MEM32(p + 8), MEM32(p + 0xC), MEM32(p + 0x10));
                }
        }
        {
            static uint32_t last_state = 0xFFFFFFFFu;
            static int dump_next;
            uint32_t t = MEM32(s_task_slot), st = t ? MEM32(t + 0x14) : 0;
            if (st != last_state) {
                fprintf(stderr, "[ACCT] logon state %X at poll %d (+0C %08X +10 %08X +50 %08X +54 %08X +2C %08X +B4 %08X)\n",
                        st, polls, MEM32(t + 0xC), MEM32(t + 0x10), MEM32(t + 0x50), MEM32(t + 0x54), MEM32(t + 0x2C), MEM32(t + 0xB4));
                last_state = st;
                if (st == 0xF) {
                    char nm[32]; snprintf(nm, sizeof nm, "toF_poll%d", polls); trace_dump(nm); dump_next = 1;
                    s_any_from = polls; InterlockedExchange(&s_trace_any, 1);
                    {   /* CXo service table: 16 entries of 0x14 at +0xD4 */
                        uint32_t cx = MEM32(VA_CXO), e;
                        for (e = 0; e < 16; e++) {
                            uint32_t p = cx + 0xD4 + e * 0x14;
                            if (MEM32(p) || MEM32(p + 4) || MEM32(p + 0x10))
                                fprintf(stderr, "[ACCT] CXo service %2u: id %08X addr %08X +08 %08X flag %08X ctx %08X\n",
                                        e, MEM32(p), MEM32(p + 4), MEM32(p + 8), MEM32(p + 0xC), MEM32(p + 0x10));
                        }
                    }
                }
                else s_ntrace = 0;
            } else if (dump_next) {
                char nm[32]; snprintf(nm, sizeof nm, "afterF_poll%d", polls); trace_dump(nm); dump_next = 0;
            } else s_ntrace = 0;
        }
        if (s_trace_any && polls - s_any_from >= 40) { InterlockedExchange(&s_trace_any, 0); trace_any_dump(); }
        if (polls < 5 || polls % 20 == 0) fprintf(stderr, "[ACCT] logon poll %d: %08X (t=%llu ms)\n", polls, hr, GetTickCount64() % 1000000);
        {   /* the request object the logon waits on in state F (logon +0x54) */
            uint32_t t = MEM32(s_task_slot), rq = t ? MEM32(t + 0x54) : 0;
            static uint32_t last_rs = 0xFFFFFFFFu, last_sub = 0xFFFFFFFFu;
            if (rq) {
                uint32_t rs = MEM32(rq + 0x10), sub = MEM32(rq + 0x98);
                if (rs != last_rs || sub != last_sub || polls % 20 == 0) {
                    fprintf(stderr, "[ACCT] request %08X: state %u +14 %08X +8F0 %08X task +98: %08X %08X %08X %08X %08X %08X\n",
                            rq, rs, MEM32(rq + 0x14), MEM32(rq + 0x8F0), MEM32(rq + 0x98), MEM32(rq + 0x9C),
                            MEM32(rq + 0xA0), MEM32(rq + 0xA4), MEM32(rq + 0xA8), MEM32(rq + 0xAC));
                    last_rs = rs; last_sub = sub;
                }
            }
        }
        polls++;
        if (hr == 0) return;
        log_hr("signed in", hr);
        if ((int32_t)hr < 0) {
            uint32_t t = MEM32(s_task_slot);
            fprintf(stderr, "[ACCT] logon object: +0C %08X +10 %08X +14 %08X +A4 %08X +3CC %08X\n",
                    MEM32(t + 0xC), MEM32(t + 0x10), MEM32(t + 0x14), MEM32(t + 0xA4), MEM32(t + 0x3CC));
            s_result = H2_ACCT_ERR_LOGON; s_state = ST_DONE; return;
        }
        {   /* The machine account id: the first 8 bytes of the drive's machine account
             * record (config index 1, sector 9) -- what the library signs in with.
             * A full sign-in also copies it to CXo+0x740 (CS: +0x1C), but one that
             * reuses cached tickets does not, so the record is the source; the copy is
             * only a cross-check. Logged: which source, never the id. */
            uint32_t buf = galloc(SECTOR_DATA), k;
            int have = buf && sector_io(VA_SECTOR_READ, cxo, 1, buf);
            for (k = 0; have && k < 8; k++) s_machine[k] = MEM8(buf + k);
            if (have && !memcmp(s_machine, "\0\0\0\0\0\0\0\0", 8)) have = 0;
            if (!have && (MEM32(cxo + s_machine_off) || MEM32(cxo + s_machine_off + 4))) {
                for (k = 0; k < 8; k++) s_machine[k] = MEM8(cxo + s_machine_off + k);
                have = 2;
            }
            if (buf) for (k = 0; k < SECTOR_DATA; k++) MEM8(buf + k) = 0;
            if (!have) {
                fprintf(stderr, "[ACCT] no machine account on the drive (config index 1) or at CXo+%X\n", s_machine_off);
                s_result = H2_ACCT_ERR_LOGON; s_state = ST_DONE; return;
            }
            {
                int copy = 1;
                for (k = 0; k < 8; k++) if (MEM8(cxo + s_machine_off + k) != s_machine[k]) copy = 0;
                fprintf(stderr, "[ACCT] machine account from %s (CXo+%X %s)\n",
                        have == 1 ? "the drive's record" : "the library",
                        s_machine_off, copy ? "matches" : "not loaded or different");
            }
        }
        if (s_mode == MODE_RECOVER) build_recover_body(cxo); else build_reserve_body(cxo);
        s_state = ST_SEND;
        return;
    }
    case ST_SEND: {
        uint32_t path = gstring(s_mode == MODE_RECOVER ? "/xuacs/restoreaccount.ashx" : "/xuacs/reservename.ashx");
        /* Halo 2's order: service, address override, path, reply, reply size,
         * 0, 0, body, body length, timeout (ms), event, task block */
        uint32_t a[12] = { SERVICE_ACCOUNT, 0, path, s_resp, REPLY_SIZE, 0, 0, s_body, s_body_len,
                           60000, 0, s_xrl };
        hr = gcall(VA_XRL_POST, cxo, 12, a);
        log_hr("request sent", hr);
        if ((int32_t)hr < 0) { s_result = H2_ACCT_ERR_NETWORK; s_state = ST_DONE; return; }
        s_state = ST_REPLY;
        return;
    }
    case ST_REPLY: {
        uint32_t a[1] = { s_xrl }, i;
        hr = gcall(VA_TASK_CONTINUE, 0, 1, a);
        if (hr == 0) return;
        for (i = 0; i < s_body_len; i++) MEM8(s_body + i) = 0;
        log_hr("reply", hr);
        if ((int32_t)hr < 0) {
            uint32_t err = reply_error();
            fprintf(stderr, "[ACCT] the server refused the request (X-Err %08X)\n", err);
            if (s_mode == MODE_RESERVE) log_reply_headers();
            s_result = err ? H2_ACCT_ERR_REFUSED : H2_ACCT_ERR_NETWORK;
            s_state = ST_DONE;
            return;
        }
        if (s_mode == MODE_RESERVE) { log_reply_headers(); s_result = H2_ACCT_OK; s_state = ST_DONE; return; }
        s_state = ST_SAVE;
        return;
    }
    case ST_SAVE: {
        uint8_t rec[RECORD_SIZE];
        uint32_t i;
        build_record(rec);
        for (i = 0; i < REPLY_SIZE; i++) MEM8(s_resp + i) = 0;
        s_result = save_record(cxo, rec);
        SecureZeroMemory(rec, sizeof rec);
        s_state = ST_DONE;
        return;
    }
    default:
        return;
    }
}

static void finish(void)
{
    if (s_mode != MODE_RECOVER) return;
    fprintf(stderr, "[ACCT] account recovery finished: result %d\n", s_result);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), (UINT)s_result);
}

/* The account thread (a guest thread, started with PsCreateSystemThreadEx and
 * H2_ACCOUNT_THREAD_VA). Between steps it waits with the guest's
 * KeDelayExecutionThread so the network stack keeps running. */
static void account_thread(void)
{
    uint32_t interval = galloc(8);
    ULONGLONG start = GetTickCount64();
    fprintf(stderr, "[ACCT] account thread running\n");
    MEM32(interval) = (uint32_t)-500000; MEM32(interval + 4) = 0xFFFFFFFFu;   /* 50 ms, relative */
    while (s_state != ST_DONE) {
        uint32_t a[3] = { 0, 0, interval };
        fprintf(stderr, "%s", "");
        step();
        if (s_state != ST_DONE && GetTickCount64() - start > 120000) {
            fprintf(stderr, "[ACCT] no answer in 2 minutes\n");
            s_result = H2_ACCT_ERR_NETWORK;
            s_state = ST_DONE;
        }
        gcall(MEM32(THUNK_KEDELAY), 0, 3, a);
    }
    fprintf(stderr, "[ACCT] account thread done (result %d)\n", s_result);
    finish();
    g_eax = 0;
}

/* From the game's KeQuerySystemTime calls: once the library is up (and the
 * game has run 20 s), start the account thread from a thread at PASSIVE level. */
static int s_thread_started;
static void tick(void)
{
    if (s_state == ST_OFF || s_state == ST_DONE) return;
    static ULONGLONG last;
    int why = 0;
    if (!s_t0) { s_t0 = GetTickCount64(); fprintf(stderr, "[ACCT] step tick hooked\n"); }
    if (GetTickCount64() - s_t0 < (ULONGLONG)s_delay_ms) return;
    if (!MEM32(VA_CXO)) why = 1;
    else if (!g_fs_base || MEM8(g_fs_base + 0x24) != 0) why = 2;   /* IRQL above PASSIVE */
    if (why) {
        if (GetTickCount64() - last > 15000) {
            last = GetTickCount64();
            fprintf(stderr, "[ACCT] waiting: %s\n", why == 1 ? "the Xbox Live library has not started" : "raised IRQL");
        }
        return;
    }
    /* Not on the main thread itself: the logon raises IRQL while it works,
     * and from the main thread's tick that held off the NIC's interrupts. */
    if (s_thread_started || InterlockedCompareExchange(&s_busy, 1, 0)) return;
    s_thread_started = 1;
    {
        uint32_t r[9], handle = galloc(8), tid = galloc(8), hr;
        r[0] = g_eax; r[1] = g_ecx; r[2] = g_edx; r[3] = g_ebx; r[4] = g_esp;
        r[5] = g_ebp; r[6] = g_esi; r[7] = g_edi; r[8] = g_eflags;
        g_esp -= 64;
        {
            uint32_t a[10] = { handle, 0, 0, 0, tid, 0, 0, 0, 0, H2_ACCOUNT_THREAD_VA };
            hr = gcall(MEM32(THUNK_PSCREATE), 0, 10, a);
        }
        g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3]; g_esp = r[4];
        g_ebp = r[5]; g_esi = r[6]; g_edi = r[7]; g_eflags = r[8];
        log_hr("account thread created", hr);
        /* Hold the game's main thread here until the logon call has been made:
         * started while the main loop runs on, the logon never got to DNS. */
        {
            ULONGLONG t = GetTickCount64();
            while (!s_logon_called && GetTickCount64() - t < 15000) Sleep(1);
            fprintf(stderr, "[ACCT] main thread held %llu ms for the logon call\n", GetTickCount64() - t);
        }
    }
    InterlockedExchange(&s_busy, 0);
}

/* Halo 2 hardly calls KeQuerySystemTime, so until the thread starts every
 * kernel call ticks: the lookup records the VA on its own thread, and the
 * hook runs the step check before the real bridge (RECOMP_ICALL looks up
 * and calls back to back on the same thread). */
static __declspec(thread) uint32_t t_kernel_va;
static void kernel_tick_hook(void)
{
    /* tick first: recomp_lookup_kernel records the slot in a global that the
     * tick's own kernel calls would overwrite */
    uint32_t va = t_kernel_va;
    recomp_func_t bridge;
    tick();
    bridge = recomp_lookup_kernel(va);
    if (bridge) bridge();
}

void (*h2_account_kernel_override(uint32_t xbox_va))(void)
{
    if (!h2_account_enabled()) return NULL;
    if (xbox_va == H2_ACCOUNT_THREAD_VA) return account_thread;
    /* only KeQuerySystemTime: a tick on any kernel call can land on XNet's own
     * threads, and a logon started from there goes wrong */
    if (s_state != ST_OFF && s_state != ST_DONE && xbox_va == MEM32(THUNK_KEQST)) {
        t_kernel_va = xbox_va;
        return kernel_tick_hook;
    }
    return NULL;
}
