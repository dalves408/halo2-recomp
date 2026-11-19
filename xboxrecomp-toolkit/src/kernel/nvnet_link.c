/*
 * nvnet_link.c -- the wire under the NIC model (nvnet.c): Ethernet frames
 * carried in UDP datagrams between hosts.
 *
 * System link is Xbox-to-Xbox Ethernet: the consoles find each other with
 * broadcast frames and then talk by MAC. Carrying each frame whole in one UDP
 * datagram makes every running instance on the LAN one Ethernet segment, with
 * no driver to install (raw Ethernet on Windows needs Npcap). Frames go to the
 * directed broadcast address of every IPv4 interface that is up, and to any
 * peers named in RECOMP_NET_PEERS (host[:port],...) -- broadcast does not
 * cross a VPN such as Tailscale, a peer list does.
 *
 * Tailscale is found by itself: only while this PC's Tailscale adapter is
 * connected (seen in the same adapter walk as the LAN broadcast), a
 * background thread asks Tailscale for the tailnet's peers every 15 s
 * (`tailscale status --json`) and every online peer's IPv4 address becomes a
 * destination too, so friends in one tailnet need no peer list. Without
 * Tailscale nothing is started and only the LAN is searched.
 * RECOMP_NET_TAILSCALE=0 turns it off. Addresses are never logged, only counts.
 *
 * Datagram: "XBL1" then the frame (14-byte Ethernet header first, no FCS).
 * RECOMP_NET_PORT sets the UDP port (default 30740). The model filters by
 * MAC; this layer only drops what is not a frame and what this instance sent.
 *
 * xemu: its "udp" network backend carries the same frames bare, one per
 * datagram, between its bind address and one remote address. Its default
 * "nat" backend keeps system link inside the emulator, and "pcap" puts raw
 * Ethernet on a network card, which this layer does not read. With
 * RECOMP_NET_XEMU=<host>:<port> (xemu's bind address; xemu's remote address
 * is this PC's, port RECOMP_NET_PORT) frames also go there without the tag,
 * and untagged datagrams from exactly that address are taken as frames.
 */
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvnet_link.h"

#define LINK_MAGIC "XBL1"
#define LINK_MAX_DEST 64
#define TS_MAX_PEERS 48

static SOCKET s_sock = INVALID_SOCKET;
static struct sockaddr_in s_dest[LINK_MAX_DEST];
static int s_ndest;
static uint8_t s_mac[6];
static unsigned short s_port = 30740;
static DWORD s_last_scan;
static struct sockaddr_in s_xemu;      /* RECOMP_NET_XEMU; sin_port 0: none */
static int s_last_xemu;                /* the last frame returned came from it */

/* Tailscale peers (network byte order), written by ts_thread, read by scan_dests. */
static CRITICAL_SECTION s_ts_lock;
static uint32_t s_ts_ip[TS_MAX_PEERS];
static int s_ts_n;
/* ts_thread exists only once a connected Tailscale adapter has been seen, and
 * asks Tailscale for peers only while one is up (s_ts_up, set by scan_dests,
 * which already walks the adapters every 10 s for the LAN broadcast). */
static volatile LONG s_ts_up;
static HANDLE s_ts_wake;
static int s_ts_allowed;

/* Runs `"<exe>" status --json` with no window; returns its stdout (malloc'd) or NULL. */
static char *ts_run(const char *exe)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 32];
    char *out = NULL;
    size_t len = 0, cap = 0;
    DWORD start = GetTickCount();

    if (!CreatePipe(&rd, &wr, &sa, 0))
        return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = NULL;
    si.hStdInput = NULL;
    snprintf(cmd, sizeof cmd, "\"%s\" status --json", exe);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return NULL;
    }
    CloseHandle(wr);
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL))
            break;                                   /* the tool exited and the pipe is drained */
        if (avail == 0) {
            if (GetTickCount() - start > 5000) {     /* hung: stop the tool this thread started */
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            Sleep(20);
            continue;
        }
        if (len + avail + 1 > cap) {
            char *n;
            cap = (len + avail + 1) * 2;
            if (cap > 8u * 1024 * 1024) break;
            n = (char *)realloc(out, cap);
            if (!n) break;
            out = n;
        }
        if (!ReadFile(rd, out + len, avail, &got, NULL) || got == 0)
            break;
        len += got;
    }
    if (out) out[len] = 0;
    WaitForSingleObject(pi.hProcess, 1000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CloseHandle(rd);
    return out;
}

/* The end of the JSON value starting at p (an object or array), honouring strings. */
static const char *json_skip(const char *p)
{
    int depth = 0, in_str = 0;
    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_str = 0;
        } else if (*p == '"') in_str = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
    }
    return p;
}

/* Finds "key" inside [p, end) and returns the first non-space character after its colon. */
static const char *json_field(const char *p, const char *end, const char *key)
{
    size_t kl = strlen(key);
    for (; p + kl + 2 < end; p++) {
        if (*p == '"' && memcmp(p + 1, key, kl) == 0 && p[kl + 1] == '"') {
            const char *q = p + kl + 2;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
            if (q < end && *q == ':') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
                return q;
            }
        }
    }
    return NULL;
}

/* The online peers' IPv4 addresses from `tailscale status --json`. */
static int ts_parse(const char *json, uint32_t *ip, int max)
{
    const char *end = json + strlen(json), *p = json_field(json, end, "Peer"), *peers_end;
    int n = 0;
    if (!p || *p != '{')
        return 0;                                    /* "Peer": null -- nobody else in the tailnet */
    peers_end = json_skip(p);
    p++;
    while (p < peers_end && n < max) {
        const char *obj, *obj_end, *v;
        while (p < peers_end && *p != '{') {         /* skip the node key and its colon */
            if (*p == '"') { p++; while (p < peers_end && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        if (p >= peers_end) break;
        obj = p;
        obj_end = json_skip(obj);
        v = json_field(obj, obj_end, "Online");
        if (v && strncmp(v, "true", 4) == 0) {
            v = json_field(obj, obj_end, "TailscaleIPs");
            if (v && *v == '[') {
                const char *arr_end = json_skip(v), *s = v;
                while ((s = memchr(s, '"', (size_t)(arr_end - s))) != NULL && s < arr_end) {
                    char addr[64];
                    const char *e = memchr(s + 1, '"', (size_t)(arr_end - s - 1));
                    struct in_addr a;
                    if (!e) break;
                    if ((size_t)(e - s - 1) < sizeof addr) {
                        memcpy(addr, s + 1, (size_t)(e - s - 1));
                        addr[e - s - 1] = 0;
                        if (inet_pton(AF_INET, addr, &a) == 1) { ip[n++] = a.s_addr; break; }
                    }
                    s = e + 1;
                }
            }
        }
        p = obj_end;
    }
    return n;
}

static DWORD WINAPI ts_thread(LPVOID unused)
{
    int last = -1;
    (void)unused;
    for (;;) {
        char exe[MAX_PATH], pf[MAX_PATH];
        uint32_t ip[TS_MAX_PEERS];
        int n = 0, found = 0;
        if (!s_ts_up) {                         /* Tailscale disconnected: forget its peers and wait */
            EnterCriticalSection(&s_ts_lock);
            s_ts_n = 0;
            LeaveCriticalSection(&s_ts_lock);
            if (last != 0 && last != -1)
                fprintf(stderr, "  [NVNET] Tailscale: disconnected\n");
            last = -1;
            WaitForSingleObject(s_ts_wake, INFINITE);
            continue;
        }
        DWORD got = GetEnvironmentVariableA("ProgramFiles", pf, sizeof pf);
        if (got && got < sizeof pf - 32) {
            snprintf(exe, sizeof exe, "%s\\Tailscale\\tailscale.exe", pf);
            found = GetFileAttributesA(exe) != INVALID_FILE_ATTRIBUTES;
        }
        if (!found)
            found = SearchPathA(NULL, "tailscale.exe", NULL, sizeof exe, exe, NULL) != 0;
        if (found) {
            char *json = ts_run(exe);
            if (json) { n = ts_parse(json, ip, TS_MAX_PEERS); free(json); }
        }
        EnterCriticalSection(&s_ts_lock);
        memcpy(s_ts_ip, ip, sizeof ip[0] * (size_t)n);
        s_ts_n = n;
        LeaveCriticalSection(&s_ts_lock);
        if (n != last) {
            if (found)
                fprintf(stderr, "  [NVNET] Tailscale: %d online peer(s)\n", n);
            last = n;
        }
        if (!found) {                           /* adapter without the command-line tool: LAN only */
            fprintf(stderr, "  [NVNET] Tailscale: adapter is up but tailscale.exe was not found, "
                            "LAN only (add the other players to [network] peers=)\n");
            return 0;
        }
        WaitForSingleObject(s_ts_wake, 15000);
    }
}

/* Tailscale's adapter (named "Tailscale", or holding a single 100.64.0.0/10
 * address) is connected: start or wake the peer lookup. */
static void ts_seen(int up)
{
    LONG was = InterlockedExchange(&s_ts_up, up);
    if (!up || !s_ts_allowed)
        return;
    if (!s_ts_wake) {
        HANDLE t;
        s_ts_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!s_ts_wake) return;
        t = CreateThread(NULL, 0, ts_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (!was)
        SetEvent(s_ts_wake);
}

static void add_dest(uint32_t addr_be, unsigned short port)
{
    int i;
    for (i = 0; i < s_ndest; i++)
        if (s_dest[i].sin_addr.s_addr == addr_be && s_dest[i].sin_port == htons(port))
            return;
    if (s_ndest == LINK_MAX_DEST)
        return;
    memset(&s_dest[s_ndest], 0, sizeof s_dest[0]);
    s_dest[s_ndest].sin_family = AF_INET;
    s_dest[s_ndest].sin_addr.s_addr = addr_be;
    s_dest[s_ndest].sin_port = htons(port);
    s_ndest++;
}

/* Every up IPv4 interface's directed broadcast, then the configured peers.
 * Rescanned now and then, so a laptop that joins Wi-Fi after the game started
 * is reached too. */
static void scan_dests(void)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = NULL, *a;
    const char *peers = getenv("RECOMP_NET_PEERS");
    int tries, ts_up = 0;

    s_ndest = 0;
    for (tries = 0; tries < 3; tries++) {
        ULONG r;
        aa = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (!aa) break;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, aa, &size);
        if (r == NO_ERROR) break;
        free(aa); aa = NULL;
        if (r != ERROR_BUFFER_OVERFLOW) break;
    }
    for (a = aa; a; a = a->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if ((a->FriendlyName && wcsstr(a->FriendlyName, L"Tailscale")) ||
            (a->Description && wcsstr(a->Description, L"Tailscale")))
            ts_up = 1;
        for (u = a->FirstUnicastAddress; u; u = u->Next) {
            const struct sockaddr_in *sa = (const struct sockaddr_in *)u->Address.lpSockaddr;
            uint32_t ip, mask;
            if (sa->sin_family == AF_INET && u->OnLinkPrefixLength == 32 &&
                (ntohl(sa->sin_addr.s_addr) & 0xFFC00000u) == 0x64400000u)
                ts_up = 1;                      /* 100.64.0.0/10, one address: Tailscale's */
            if (sa->sin_family != AF_INET || u->OnLinkPrefixLength == 0 || u->OnLinkPrefixLength >= 31)
                continue;
            ip = ntohl(sa->sin_addr.s_addr);
            mask = 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength);
            add_dest(htonl(ip | ~mask), s_port);
        }
    }
    free(aa);
    ts_seen(ts_up);
    if (s_ndest == 0)
        add_dest(htonl(INADDR_BROADCAST), s_port);
    if (peers && *peers) {
        char buf[1024], *tok, *ctx = NULL;
        strncpy(buf, peers, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (tok = strtok_s(buf, ",; ", &ctx); tok; tok = strtok_s(NULL, ",; ", &ctx)) {
            char *colon = strrchr(tok, ':');
            unsigned short port = s_port;
            struct addrinfo hints, *res = NULL;
            if (colon) { *colon = 0; port = (unsigned short)atoi(colon + 1); }
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(tok, NULL, &hints, &res) == 0 && res) {
                add_dest(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr, port);
                freeaddrinfo(res);
            }
        }
    }
    {
        int i;
        EnterCriticalSection(&s_ts_lock);
        for (i = 0; i < s_ts_n; i++)
            add_dest(s_ts_ip[i], s_port);
        LeaveCriticalSection(&s_ts_lock);
    }
    s_last_scan = GetTickCount();
}

int nvlink_open(const uint8_t mac[6])
{
    WSADATA wd;
    struct sockaddr_in me;
    BOOL on = TRUE;
    const char *p = getenv("RECOMP_NET_PORT");
    int i;

    memcpy(s_mac, mac, 6);
    if (p && atoi(p) > 0 && atoi(p) < 65536)
        s_port = (unsigned short)atoi(p);
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
        return -1;
    /* Not inheritable: a title that relaunches itself (XLaunchNewImage as a new
     * process) would hand this socket to its successor, keep the port bound
     * after it exits, and the frames sent to it would go to a socket nobody
     * reads. */
    s_sock = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s_sock == INVALID_SOCKET)
        return -1;
    SetHandleInformation((HANDLE)s_sock, HANDLE_FLAG_INHERIT, 0);
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    {   /* A datagram to a closed port (a peer not started yet, xemu between
         * restarts) makes Windows fail the socket's next receive with
         * WSAECONNRESET. Turn that report off: UDP has no connection. */
        BOOL off = FALSE;
        DWORD got = 0;
        WSAIoctl(s_sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    }
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = htons(s_port);
    if (bind(s_sock, (struct sockaddr *)&me, sizeof me) != 0) {
        fprintf(stderr, "  [NVNET] cannot bind UDP port %u (error %d)\n", s_port, WSAGetLastError());
        closesocket(s_sock); s_sock = INVALID_SOCKET;
        return -1;
    }
    static int ts_init;
    if (!ts_init) {
        const char *ts = getenv("RECOMP_NET_TAILSCALE");
        InitializeCriticalSection(&s_ts_lock);
        s_ts_allowed = !ts || strcmp(ts, "0") != 0;
        ts_init = 1;
    }
    scan_dests();
    {
        const char *x = getenv("RECOMP_NET_XEMU");
        memset(&s_xemu, 0, sizeof s_xemu);
        if (x && *x) {
            char host[256], *colon;
            struct addrinfo hints, *res = NULL;
            strncpy(host, x, sizeof host - 1); host[sizeof host - 1] = 0;
            colon = strrchr(host, ':');
            if (colon && atoi(colon + 1) > 0 && atoi(colon + 1) < 65536) {
                *colon = 0;
                memset(&hints, 0, sizeof hints);
                hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
                if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
                    s_xemu.sin_family = AF_INET;
                    s_xemu.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
                    s_xemu.sin_port = htons((unsigned short)atoi(colon + 1));
                    freeaddrinfo(res);
                }
            }
            fprintf(stderr, "  [NVNET] xemu peer: %s\n", s_xemu.sin_port ? "on (untagged frames, its udp backend)"
                                                                        : "RECOMP_NET_XEMU is not host:port, ignored");
        }
    }
    fprintf(stderr, "  [NVNET] link: UDP port %u, %d destination(s):", s_port, s_ndest);
    for (i = 0; i < s_ndest; i++) {
        uint32_t a = ntohl(s_dest[i].sin_addr.s_addr);
        fprintf(stderr, " %u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    }
    fprintf(stderr, "\n");
    if (s_ts_allowed)
        fprintf(stderr, "  [NVNET] Tailscale: %s\n", s_ts_up ? "connected, looking for peers" : "not connected, LAN only");
    return 0;
}

void nvlink_send(const uint8_t *frame, int len)
{
    char buf[4 + NVLINK_MAX_FRAME];
    int i;
    if (s_sock == INVALID_SOCKET || len < 14 || len > NVLINK_MAX_FRAME)
        return;
    if (GetTickCount() - s_last_scan > 10000)
        scan_dests();
    memcpy(buf, LINK_MAGIC, 4);
    memcpy(buf + 4, frame, (size_t)len);
    for (i = 0; i < s_ndest; i++)
        sendto(s_sock, buf, 4 + len, 0, (const struct sockaddr *)&s_dest[i], sizeof s_dest[i]);
    if (s_xemu.sin_port)
        sendto(s_sock, buf + 4, len, 0, (const struct sockaddr *)&s_xemu, sizeof s_xemu);
}

int nvlink_last_from_xemu(void) { return s_last_xemu; }

void nvlink_send_xemu(const uint8_t *frame, int len)
{
    if (s_sock != INVALID_SOCKET && s_xemu.sin_port && len >= 14 && len <= NVLINK_MAX_FRAME)
        sendto(s_sock, (const char *)frame, len, 0, (const struct sockaddr *)&s_xemu, sizeof s_xemu);
}

int nvlink_recv(uint8_t *frame, int max, int timeout_ms)
{
    char buf[4 + NVLINK_MAX_FRAME];
    fd_set rs;
    struct timeval tv;
    int n;

    if (s_sock == INVALID_SOCKET) { Sleep((DWORD)timeout_ms); return 0; }
    FD_ZERO(&rs); FD_SET(s_sock, &rs);
    tv.tv_sec = 0; tv.tv_usec = timeout_ms * 1000;
    if (select(0, &rs, NULL, NULL, &tv) <= 0)
        return 0;
    int off = 4;
    s_last_xemu = 0;
    {
        struct sockaddr_in from;
        int fl = sizeof from;
        n = recvfrom(s_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n >= 14 && s_xemu.sin_port && from.sin_port == s_xemu.sin_port &&
            from.sin_addr.s_addr == s_xemu.sin_addr.s_addr && memcmp(buf, LINK_MAGIC, 4) != 0)
            off = 0;                            /* xemu's udp backend: a bare frame */
        else if (n < 4 + 14 || memcmp(buf, LINK_MAGIC, 4) != 0)
            return 0;
    }
    n -= off;
    s_last_xemu = off == 0;
    if (memcmp(buf + off + 6, s_mac, 6) == 0)   /* our own, looped back */
        return 0;
    if (n > max) n = max;
    memcpy(frame, buf + off, (size_t)n);
    return n;
}

#else   /* POSIX: not yet */
#include <stdint.h>
#include "nvnet_link.h"
int  nvlink_open(const uint8_t mac[6]) { (void)mac; return -1; }
void nvlink_send(const uint8_t *frame, int len) { (void)frame; (void)len; }
int  nvlink_recv(uint8_t *frame, int max, int timeout_ms) { (void)frame; (void)max; (void)timeout_ms; return 0; }
int  nvlink_last_from_xemu(void) { return 0; }
void nvlink_send_xemu(const uint8_t *frame, int len) { (void)frame; (void)len; }
#endif
