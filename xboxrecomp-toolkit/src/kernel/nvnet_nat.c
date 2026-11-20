/*
 * nvnet_nat.c -- a small router behind the NIC model (nvnet.c): the
 * console's way to the internet, for Xbox Live on Insignia.
 *
 * The NIC's DHCP answer puts the console at 10.x.y.z/8 with this router at
 * 10.0.0.1 and Insignia's DNS server first. Frames the console addresses to
 * the router's MAC come here; everything else still goes out on the system
 * link (nvnet_link.c), so system link and Xbox Live work side by side.
 *
 *   ARP for 10.0.0.1          answered here
 *   ICMP echo to 10.0.0.1     answered here
 *   UDP to anywhere           one host socket per console port, bound to the
 *                             same port when it is free (3074 leaves as 3074);
 *                             replies from any host come back (endpoint-
 *                             independent mapping, which XNet's NAT test likes)
 *   IP fragments              reassembled on the way out, made on the way in
 *   TCP                       refused with a reset for now (logged), so a
 *                             connect fails at once instead of timing out
 *
 * Xbox Live's sign-in (Kerberos, UDP 88) and its security gateway (UDP 3074)
 * are UDP. RECOMP_NET_NAT=0 turns the router off; RECOMP_NET_DNS sets the DNS
 * servers (comma-separated; default 46.101.64.175, Insignia's, then 1.1.1.1).
 * The log names the first DNS lookups and the first host each way, never
 * payloads.
 *
 * Nothing leaves for the internet without the player's console identity (a
 * valid eeprom.bin and a hard-drive key, console_id.c): without one the
 * router still answers locally but drops every packet bound beyond it, DNS
 * included, so Insignia is never contacted with a blank or made-up console.
 */
#include "nvnet_nat.h"
#include "console_id.h"

#if defined(_WIN32)
#include <stddef.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t g_nat_router_mac[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x01 };

#define MAX_UDP     64
#define UDP_IDLE_MS 300000u
#define MTU         1500

typedef struct {
    SOCKET s;
    uint16_t cport;         /* the console's source port */
    DWORD used;
} UdpMap;

typedef struct {            /* one datagram being reassembled */
    uint32_t src, dst;
    uint16_t id;
    uint8_t proto;
    int have, total;        /* bytes held, total once the last fragment came */
    DWORD started;
    uint8_t got[8192 / 8];  /* which 8-byte units arrived */
    uint8_t data[8192];
} Frag;

static int s_on = -1;
static void (*s_inject)(const uint8_t *frame, int len);
static CRITICAL_SECTION s_lock;
static UdpMap s_udp[MAX_UDP];
static Frag s_frag;
static uint8_t s_console_mac[6];
static uint32_t s_console_ip;           /* host order, learned from its frames */
static uint32_t s_dns[2];               /* network order */
static int s_ndns;
static uint16_t s_ip_id = 1;
static unsigned s_log_dns, s_log_hosts, s_log_tcp;
static unsigned long s_out, s_in;
static int s_identity;                  /* 1: the player's EEPROM and hard-drive key, verified */
static int (*volatile s_gate)(void);     /* nat_set_internet_gate */
static unsigned s_log_blocked;
/* Xbox Live traffic (UDP 3074, either way). Signed in, the console trades a
 * keep-alive with the security gateway every few seconds; in a match it
 * trades game traffic with the other consoles, dozens of packets a second.
 * A second with at least NAT_LIVE_MATCH_PPS packets marks a match in progress
 * (nat_live_match_recent); any packet marks being signed in (nat_live_recent). */
#define NAT_LIVE_MATCH_PPS 8
static volatile DWORD s_last_live, s_last_live_match;
static volatile LONG s_live_second, s_live_count;

static void live_note(void)
{
    DWORD now = GetTickCount();
    LONG sec = (LONG)(now / 1000u);
    s_last_live = now;
    if (InterlockedExchange(&s_live_second, sec) != sec)
        InterlockedExchange(&s_live_count, 0);
    if (InterlockedIncrement(&s_live_count) >= NAT_LIVE_MATCH_PPS)
        s_last_live_match = now;
}

static uint16_t csum(const uint8_t *p, int n, uint32_t s)
{
    int i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)~s;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void ipstr(uint32_t ip, char *out) { sprintf(out, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }

/* An IPv4 packet to the console, split into fragments when it does not fit. */
static void send_ip(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload, int plen)
{
    uint8_t f[14 + MTU];
    int off = 0;
    uint16_t id = s_ip_id++;
    do {
        int room = MTU - 20, n = plen - off, more = 0;
        if (n > room) { n = room & ~7; more = 1; }
        memcpy(f, s_console_mac, 6);
        memcpy(f + 6, g_nat_router_mac, 6);
        f[12] = 0x08; f[13] = 0x00;
        memset(f + 14, 0, 20);
        f[14] = 0x45;
        put16(f + 16, (uint32_t)(20 + n));
        put16(f + 18, id);
        put16(f + 20, (uint32_t)((more ? 0x2000 : 0) | (off / 8)));
        f[22] = 64; f[23] = proto;
        put32(f + 26, src); put32(f + 30, dst);
        put16(f + 24, csum(f + 14, 20, 0));
        memcpy(f + 34, payload + off, (size_t)n);
        s_inject(f, 14 + 20 + n);
        off += n;
    } while (off < plen);
}

static void arp_reply(const uint8_t *req)
{
    uint8_t f[42];
    memcpy(f, req + 6, 6);
    memcpy(f + 6, g_nat_router_mac, 6);
    f[12] = 0x08; f[13] = 0x06;
    put16(f + 14, 1); put16(f + 16, 0x0800); f[18] = 6; f[19] = 4; put16(f + 20, 2);
    memcpy(f + 22, g_nat_router_mac, 6);
    put32(f + 28, NAT_ROUTER_IP);
    memcpy(f + 32, req + 22, 10);                       /* the asker's MAC and IP */
    s_inject(f, 42);
}

static UdpMap *udp_for(uint16_t cport)
{
    int i, free_i = -1;
    DWORD now = GetTickCount();
    for (i = 0; i < MAX_UDP; i++) {
        if (s_udp[i].s != INVALID_SOCKET && s_udp[i].cport == cport) { s_udp[i].used = now; return &s_udp[i]; }
        if (s_udp[i].s != INVALID_SOCKET && now - s_udp[i].used > UDP_IDLE_MS) {
            closesocket(s_udp[i].s); s_udp[i].s = INVALID_SOCKET;
        }
        if (s_udp[i].s == INVALID_SOCKET && free_i < 0) free_i = i;
    }
    if (free_i < 0) return NULL;
    {
        SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        struct sockaddr_in a;
        u_long nb = 1;
        if (s == INVALID_SOCKET) return NULL;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(cport);
        if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {       /* the same port, else any */
            a.sin_port = 0;
            bind(s, (struct sockaddr *)&a, sizeof a);
        }
        ioctlsocket(s, FIONBIO, &nb);
        s_udp[free_i].s = s;
        s_udp[free_i].cport = cport;
        s_udp[free_i].used = now;
        {
            int len = sizeof a;
            getsockname(s, (struct sockaddr *)&a, &len);
            fprintf(stderr, "  [NAT] console UDP port %u -> host port %u\n", cport, ntohs(a.sin_port));
        }
        return &s_udp[free_i];
    }
}

static void log_dns(const uint8_t *q, int n)
{
    char name[128];
    int i = 12, k = 0;
    if (n < 13 || s_log_dns >= 48) return;
    while (i < n && q[i] && k < (int)sizeof name - 2) {
        int l = q[i++];
        if (k) name[k++] = '.';
        while (l-- > 0 && i < n && k < (int)sizeof name - 1) name[k++] = (char)q[i++];
    }
    name[k] = 0;
    s_log_dns++;
    fprintf(stderr, "  [NAT] DNS lookup: %s\n", name);
}

/* Xbox Live names always to the first DNS server (Insignia's).
 *
 * The console asks its DHCP DNS servers in turn: Insignia's first, then the
 * public one Insignia's guide says to set as secondary. When Insignia's does
 * not answer in time (one lost packet is enough), the console asks the public
 * one, which answers with Microsoft's old Xbox Live servers -- long gone -- and
 * sign-in then waits on a server that never replies (seen: TGS.XBOXLIVE.COM ->
 * 40.90.8.164, Kerberos resent until it timed out). So a lookup of an
 * *.xboxlive.com name is sent to the first server whichever one the console
 * addressed, and the reply is given back as if from the one it asked (the
 * query id tells the replies apart). */
#define DNS_REDIRECTS 16
static struct { uint16_t cport, id; uint32_t asked; DWORD at; } s_dns_redirect[DNS_REDIRECTS];
static unsigned s_dns_redirect_next, s_log_redirect;

static int dns_is_xboxlive(const uint8_t *q, int n)
{
    char name[128];
    int i = 12, k = 0, len;
    static const char tail[] = "xboxlive.com";
    if (n < 13) return 0;
    while (i < n && q[i] && k < (int)sizeof name - 2) {
        int l = q[i++];
        if (k) name[k++] = '.';
        while (l-- > 0 && i < n && k < (int)sizeof name - 1) name[k++] = (char)tolower(q[i++]);
    }
    name[k] = 0;
    len = (int)strlen(name);
    return len >= (int)sizeof tail - 1 && !strcmp(name + len - (int)(sizeof tail - 1), tail)
        && (len == (int)sizeof tail - 1 || name[len - (int)sizeof tail] == '.');
}

/* The server a redirected reply should look like it came from, or 0. */
static uint32_t dns_redirected_from(uint16_t cport, const uint8_t *reply, int n)
{
    unsigned i;
    if (n < 2) return 0;
    for (i = 0; i < DNS_REDIRECTS; i++)
        if (s_dns_redirect[i].asked && s_dns_redirect[i].cport == cport &&
            s_dns_redirect[i].id == get16(reply) && GetTickCount() - s_dns_redirect[i].at < 10000) {
            uint32_t asked = s_dns_redirect[i].asked;
            s_dns_redirect[i].asked = 0;
            return asked;
        }
    return 0;
}

/* One whole IPv4 packet from the console, for the router or beyond it. */
/* ---- TCP relay ------------------------------------------------------------
 *
 * The console runs its own TCP stack, so the router ends each connection
 * itself (as slirp does) and carries the bytes over a host socket to the same
 * server. Halo 2's content downloader needs it: the packs come over plain
 * HTTP from Insignia's content server (188.240.13.12:8081), and with TCP
 * refused every download ended in "Installation of content failed".
 *
 * The console side runs from the NIC's transmit path (tcp_from_console). A
 * thread of its own (tcp_thread) completes connects, reads the server, sends
 * within the console's window, retransmits what the console has not
 * acknowledged (the virtual link can drop a frame when the receive ring is
 * full), and closes. The thread builds its frames under s_tcp_lock and sends
 * them after letting go of it: injecting takes the NIC's lock, which the
 * transmit path already holds when it calls in here. */
#define MAX_TCP   32
#define TCP_BUF   65536u
#define TCP_MSS   1460u
#define TCP_FIN   0x01
#define TCP_SYN   0x02
#define TCP_RST   0x04
#define TCP_PSH   0x08
#define TCP_ACK   0x10
enum { TC_FREE, TC_CONNECTING, TC_SYNACK, TC_OPEN };

typedef struct {
    int state;
    SOCKET s;
    uint32_t cip, rip;          /* host order */
    uint16_t cport, rport;
    uint32_t c_next;            /* next sequence number expected from the console */
    uint32_t r_una, r_nxt;      /* ours: oldest unacknowledged, next to send */
    uint32_t c_win;             /* the console's receive window */
    uint32_t mss;
    uint32_t buflen;
    int host_eof, fin_sent, console_fin;
    DWORD last_tx, last_use, opened;
    uint64_t up, down;          /* bytes console->server, server->console */
    char req[72], resp[48];     /* first request line (no query string), first status line */
    /* Last, so a new connection clears every field above with one memset up
     * to here. With the buffer in the middle, a reused slot kept the previous
     * connection's buflen and end-of-stream flags: the next download's
     * connection was closed 15 ms after it opened, which is why only the
     * first pack of a downloader session installed. */
    uint8_t buf[TCP_BUF];       /* server bytes from r_una on, sent or not */
} TcpConn;

/* Copy a protocol's first line for the log: printable bytes up to the line
 * end; a request stops at '?' so no query string (which can carry a token)
 * is ever written. */
static void tcp_first_line(char *dst, size_t cap, const uint8_t *p, int n, int stop_at_query)
{
    size_t k = 0;
    int i;
    if (dst[0]) return;
    for (i = 0; i < n && k + 1 < cap; i++) {
        uint8_t ch = p[i];
        if (ch == '\r' || ch == '\n' || (stop_at_query && ch == '?')) break;
        dst[k++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    dst[k] = 0;
    if (!k) { dst[0] = '-'; dst[1] = 0; }
}

static TcpConn s_tcp[MAX_TCP];
static CRITICAL_SECTION s_tcp_lock;
static int s_tcp_ready;
static unsigned s_log_tcp_open;

/* Frames the thread built, sent after the lock is released. */
#define TCP_OUTQ 96
static struct { uint32_t src, dst; int n; uint8_t d[20 + 4 + TCP_MSS]; } s_tcp_q[TCP_OUTQ];
static int s_tcp_nq, s_tcp_queueing;

static void tcp_seg(TcpConn *c, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t n, int mss_opt)
{
    uint8_t seg[20 + 4 + TCP_MSS];
    uint8_t ph[12];
    uint32_t s = 0, hl = mss_opt ? 24u : 20u;
    int i;
    if (n > TCP_MSS) n = TCP_MSS;
    memset(seg, 0, hl);
    put16(seg, c->rport); put16(seg + 2, c->cport);
    put32(seg + 4, seq);
    put32(seg + 8, (flags & TCP_ACK) ? c->c_next : 0);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    put16(seg + 14, 65535);                     /* what we take from the console */
    if (mss_opt) { seg[20] = 2; seg[21] = 4; put16(seg + 22, TCP_MSS); }
    if (n) memcpy(seg + hl, data, n);
    put32(ph, c->rip); put32(ph + 4, c->cip); ph[8] = 0; ph[9] = 6; put16(ph + 10, hl + n);
    for (i = 0; i < 12; i += 2) s += (uint32_t)(ph[i] << 8 | ph[i + 1]);
    put16(seg + 16, csum(seg, (int)(hl + n), s));
    c->last_tx = GetTickCount();
    if (s_tcp_queueing) {
        if (s_tcp_nq < TCP_OUTQ) {
            s_tcp_q[s_tcp_nq].src = c->rip; s_tcp_q[s_tcp_nq].dst = c->cip;
            s_tcp_q[s_tcp_nq].n = (int)(hl + n);
            memcpy(s_tcp_q[s_tcp_nq].d, seg, hl + n);
            s_tcp_nq++;
        }
        return;
    }
    send_ip(c->rip, c->cip, 6, seg, (int)(hl + n));
}

static unsigned s_log_tcp_close;
static void tcp_free(TcpConn *c, const char *why)
{
    if (s_log_tcp_close < 64) {
        char a[20];
        s_log_tcp_close++;
        ipstr(c->rip, a);
        fprintf(stderr, "  [NAT] TCP %u -> %s:%u closed (%s) after %lu ms: %llu bytes up, %llu down;"
                        " request '%s', reply '%s'\n", c->cport, a, c->rport, why,
                (unsigned long)(GetTickCount() - c->opened), (unsigned long long)c->up,
                (unsigned long long)c->down, c->req[0] ? c->req : "-", c->resp[0] ? c->resp : "-");
    }
    if (c->s != INVALID_SOCKET) closesocket(c->s);
    c->s = INVALID_SOCKET;
    c->state = TC_FREE;
}

/* A reset to a segment with no connection behind it. */
static void tcp_reset_reply(uint32_t src, uint32_t dst, const uint8_t *p, int n)
{
    TcpConn t;
    uint32_t seq = get32(p + 4), ack = get32(p + 8);
    uint8_t fl = p[13];
    int hl = (p[12] >> 4) * 4, payload = n - hl;
    if (fl & TCP_RST) return;
    memset(&t, 0, sizeof t);
    t.rip = dst; t.cip = src; t.rport = get16(p + 2); t.cport = get16(p);
    if (fl & TCP_ACK) { t.c_next = 0; tcp_seg(&t, ack, TCP_RST, NULL, 0, 0); }
    else {
        t.c_next = seq + (payload > 0 ? (uint32_t)payload : 0) + ((fl & TCP_SYN) ? 1u : 0u) + ((fl & TCP_FIN) ? 1u : 0u);
        tcp_seg(&t, 0, TCP_RST | TCP_ACK, NULL, 0, 0);
    }
}

/* Drop what the console has acknowledged from the send buffer. */
static void tcp_acked(TcpConn *c, uint32_t ack)
{
    uint32_t inflight = c->r_nxt - c->r_una, adv = ack - c->r_una;
    if (adv == 0 || adv > inflight) return;
    if (c->state == TC_SYNACK) { adv--; c->r_una++; c->state = TC_OPEN; }
    if (adv > c->buflen) { adv = c->buflen; c->r_una = ack; }   /* the FIN */
    else c->r_una += adv;
    if (adv) { memmove(c->buf, c->buf + adv, c->buflen - adv); c->buflen -= adv; }
}

/* A TCP segment from the console to anywhere but the router. 1: handled. */
static int tcp_from_console(uint32_t src, uint32_t dst, const uint8_t *p, int n)
{
    uint16_t sp = get16(p), dp = get16(p + 2);
    uint32_t seq = get32(p + 4), ack = get32(p + 8);
    uint8_t fl = p[13];
    int hl = (p[12] >> 4) * 4, plen, i;
    TcpConn *c = NULL;
    if (!s_tcp_ready || hl < 20 || hl > n) return 0;
    plen = n - hl;
    EnterCriticalSection(&s_tcp_lock);
    for (i = 0; i < MAX_TCP; i++)
        if (s_tcp[i].state != TC_FREE && s_tcp[i].cport == sp && s_tcp[i].rport == dp && s_tcp[i].rip == dst) {
            c = &s_tcp[i]; break;
        }
    if (!c) {
        struct sockaddr_in to;
        u_long nb = 1;
        char a[20];
        if (!(fl & TCP_SYN) || (fl & TCP_ACK)) { LeaveCriticalSection(&s_tcp_lock); tcp_reset_reply(src, dst, p, n); return 1; }
        if ((s_gate && s_gate()) || !s_identity) { LeaveCriticalSection(&s_tcp_lock); tcp_reset_reply(src, dst, p, n); return 1; }
        for (i = 0; i < MAX_TCP && s_tcp[i].state != TC_FREE; i++) ;
        if (i == MAX_TCP) { LeaveCriticalSection(&s_tcp_lock); tcp_reset_reply(src, dst, p, n); return 1; }
        c = &s_tcp[i];
        memset(c, 0, offsetof(TcpConn, buf));
        c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c->s == INVALID_SOCKET) { c->state = TC_FREE; LeaveCriticalSection(&s_tcp_lock); tcp_reset_reply(src, dst, p, n); return 1; }
        ioctlsocket(c->s, FIONBIO, &nb);
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(dst); to.sin_port = htons(dp);
        connect(c->s, (struct sockaddr *)&to, sizeof to);
        c->cip = src; c->rip = dst; c->cport = sp; c->rport = dp;
        c->c_next = seq + 1;
        c->r_una = c->r_nxt = (GetTickCount() << 10) ^ 0x5A3C0000u;
        c->c_win = get16(p + 14);
        c->mss = 536;
        for (i = 20; i + 1 < hl; ) {                          /* the console's MSS */
            uint8_t k = p[i];
            if (k == 0) break;
            if (k == 1) { i++; continue; }
            if (k == 2 && p[i + 1] == 4 && i + 4 <= hl) c->mss = get16(p + i + 2);
            if (p[i + 1] < 2) break;
            i += p[i + 1];
        }
        if (c->mss > TCP_MSS) c->mss = TCP_MSS;
        c->state = TC_CONNECTING;
        c->opened = GetTickCount();
        c->last_use = GetTickCount();
        if (s_log_tcp_open < 16) { s_log_tcp_open++; ipstr(dst, a); fprintf(stderr, "  [NAT] TCP %u -> %s:%u connecting (relayed)\n", sp, a, dp); }
        LeaveCriticalSection(&s_tcp_lock);
        return 1;
    }
    c->last_use = GetTickCount();
    if (fl & TCP_RST) { tcp_free(c, "console reset"); LeaveCriticalSection(&s_tcp_lock); return 1; }
    if (fl & TCP_SYN) {                                     /* a retransmitted SYN */
        if (c->state == TC_SYNACK) tcp_seg(c, c->r_una, TCP_SYN | TCP_ACK, NULL, 0, 1);
        LeaveCriticalSection(&s_tcp_lock);
        return 1;
    }
    c->c_win = get16(p + 14);
    if (fl & TCP_ACK) tcp_acked(c, ack);
    if (plen > 0 || (fl & TCP_FIN)) {
        if (seq == c->c_next && c->state == TC_OPEN) {
            int sent = 0;
            if (plen > 0) {
                sent = send(c->s, (const char *)p + hl, plen, 0);
                if (sent < 0) sent = 0;                    /* would block: the console resends */
                if (sent > 0) { tcp_first_line(c->req, sizeof c->req, p + hl, sent, 1); c->up += (uint32_t)sent; }
                c->c_next += (uint32_t)sent;
            }
            if ((fl & TCP_FIN) && sent == plen && !c->console_fin) {
                c->c_next++;
                c->console_fin = 1;
                shutdown(c->s, SD_SEND);
            }
        }
        tcp_seg(c, c->r_nxt, TCP_ACK, NULL, 0, 0);
    }
    if (c->console_fin && c->fin_sent && c->r_una == c->r_nxt) tcp_free(c, "closed by both");
    LeaveCriticalSection(&s_tcp_lock);
    return 1;
}

static DWORD WINAPI tcp_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        fd_set rs, ws, es;
        struct timeval tv = { 0, 2000 };
        int i, any = 0;
        FD_ZERO(&rs); FD_ZERO(&ws); FD_ZERO(&es);
        EnterCriticalSection(&s_tcp_lock);
        for (i = 0; i < MAX_TCP; i++) {
            TcpConn *c = &s_tcp[i];
            if (c->state == TC_FREE) continue;
            any = 1;
            if (c->state == TC_CONNECTING) { FD_SET(c->s, &ws); FD_SET(c->s, &es); }
            else if (!c->host_eof && c->buflen < TCP_BUF) FD_SET(c->s, &rs);
        }
        LeaveCriticalSection(&s_tcp_lock);
        if (!any) { Sleep(5); continue; }
        select(0, &rs, &ws, &es, &tv);

        EnterCriticalSection(&s_tcp_lock);
        s_tcp_queueing = 1;
        s_tcp_nq = 0;
        for (i = 0; i < MAX_TCP; i++) {
            TcpConn *c = &s_tcp[i];
            DWORD now = GetTickCount();
            uint32_t inflight, unsent;
            if (c->state == TC_FREE) continue;
            if (c->state == TC_CONNECTING) {
                if (FD_ISSET(c->s, &es) || now - c->last_use > 20000) {
                    tcp_seg(c, 0, TCP_RST | TCP_ACK, NULL, 0, 0);
                    tcp_free(c, FD_ISSET(c->s, &es) ? "connect failed" : "connect timed out");
                } else if (FD_ISSET(c->s, &ws)) {
                    c->state = TC_SYNACK;
                    tcp_seg(c, c->r_nxt, TCP_SYN | TCP_ACK, NULL, 0, 1);
                    c->r_nxt++;
                }
                continue;
            }
            if (FD_ISSET(c->s, &rs)) {
                int got = recv(c->s, (char *)c->buf + c->buflen, (int)(TCP_BUF - c->buflen), 0);
                if (got > 0) {
                    tcp_first_line(c->resp, sizeof c->resp, c->buf + c->buflen, got, 0);
                    c->buflen += (uint32_t)got; c->down += (uint32_t)got; c->last_use = now;
                }
                else if (got == 0) c->host_eof = 1;
                else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    tcp_seg(c, c->r_nxt, TCP_RST | TCP_ACK, NULL, 0, 0);
                    tcp_free(c, "server connection error");
                    continue;
                }
            }
            if (c->state != TC_OPEN) {
                if (c->state == TC_SYNACK && now - c->last_tx > 500)
                    tcp_seg(c, c->r_una, TCP_SYN | TCP_ACK, NULL, 0, 1);
                continue;
            }
            /* Go back to the oldest unacknowledged byte when the console has
             * gone quiet on it: the link dropped a frame. */
            if (c->r_nxt != c->r_una && now - c->last_tx > 400) {
                c->r_nxt = c->r_una;
                c->fin_sent = 0;
            }
            inflight = c->r_nxt - c->r_una;
            if (inflight > c->buflen) inflight = c->buflen;
            unsent = c->buflen - inflight;
            while (unsent && inflight < c->c_win && s_tcp_nq < TCP_OUTQ - 2) {
                uint32_t k = unsent, room = c->c_win - inflight;
                if (k > c->mss) k = c->mss;
                if (k > room) k = room;
                tcp_seg(c, c->r_nxt, TCP_ACK | TCP_PSH, c->buf + inflight, k, 0);
                c->r_nxt += k; inflight += k; unsent -= k;
            }
            if (c->host_eof && !unsent && !c->fin_sent) {
                tcp_seg(c, c->r_nxt, TCP_FIN | TCP_ACK, NULL, 0, 0);
                c->r_nxt++;
                c->fin_sent = 1;
            }
            if (c->console_fin && c->fin_sent && c->r_una == c->r_nxt)
                tcp_free(c, "closed by both");
            else if (now - c->last_use > 300000)
                tcp_free(c, "idle 5 minutes");
        }
        s_tcp_queueing = 0;
        {
            int nq = s_tcp_nq;
            static struct { uint32_t src, dst; int n; uint8_t d[20 + 4 + TCP_MSS]; } out[TCP_OUTQ];
            memcpy(out, s_tcp_q, sizeof out[0] * (size_t)nq);
            s_tcp_nq = 0;
            LeaveCriticalSection(&s_tcp_lock);
            for (i = 0; i < nq; i++) send_ip(out[i].src, out[i].dst, 6, out[i].d, out[i].n);
        }
    }
}

static void tcp_init(void)
{
    int i;
    InitializeCriticalSection(&s_tcp_lock);
    for (i = 0; i < MAX_TCP; i++) { s_tcp[i].s = INVALID_SOCKET; s_tcp[i].state = TC_FREE; }
    s_tcp_ready = 1;
    {
        HANDLE th = CreateThread(NULL, 0, tcp_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
}

static void ip_out(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *p, int n)
{
    char a[20];
    s_console_ip = src;
    if (proto == 1 && dst == NAT_ROUTER_IP && n >= 8 && p[0] == 8) {      /* ping the router */
        uint8_t r[MTU];
        if (n > (int)sizeof r) return;
        memcpy(r, p, (size_t)n);
        r[0] = 0; r[2] = r[3] = 0;
        put16(r + 2, csum(r, n, 0));
        send_ip(NAT_ROUTER_IP, src, 1, r, n);
        return;
    }
    if (proto == 17 && n >= 8) {
        uint16_t sp = get16(p), dp = get16(p + 2);
        UdpMap *m;
        struct sockaddr_in to;
        if (dst == NAT_ROUTER_IP) return;
        if (s_gate && s_gate())                 /* the title's own rule, first: it says why */
            return;
        if (!s_identity) {
            if (!s_log_blocked++)
                fprintf(stderr, "  [NAT] no verified console identity (EEPROM and hard-drive key checked "
                                "against your BIOS): internet traffic blocked, Xbox Live is off. Run "
                                "Insignia Setup to add yours.\n");
            return;
        }
        if (dp == 53) log_dns(p + 8, n - 8);
        if (!(m = udp_for(sp))) return;
        if (dp == 53 && s_ndns && dst != ntohl(s_dns[0]) && n >= 10 && dns_is_xboxlive(p + 8, n - 8)) {
            unsigned k = s_dns_redirect_next++ % DNS_REDIRECTS;
            s_dns_redirect[k].cport = sp;
            s_dns_redirect[k].id = get16(p + 8);
            s_dns_redirect[k].asked = dst;
            s_dns_redirect[k].at = GetTickCount();
            if (s_log_redirect < 8) {
                s_log_redirect++;
                ipstr(dst, a);
                fprintf(stderr, "  [NAT] Xbox Live lookup sent to %s: given to the first DNS server instead\n", a);
            }
            dst = ntohl(s_dns[0]);
        }
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(dst);
        to.sin_port = htons(dp);
        sendto(m->s, (const char *)p + 8, n - 8, 0, (struct sockaddr *)&to, sizeof to);
        if (dp == 3074 || sp == 3074) live_note();          /* Xbox Live (gateway or another console) */
        s_out++;
        if (s_log_hosts < 24 && dp != 53) { s_log_hosts++; ipstr(dst, a); fprintf(stderr, "  [NAT] UDP %u -> %s:%u (%d bytes)\n", sp, a, dp, n - 8); }
        return;
    }
    if (proto == 6 && n >= 20 && dst != NAT_ROUTER_IP && tcp_from_console(src, dst, p, n))
        return;                                                           /* TCP: relayed */
    if (proto == 6 && n >= 20) {                                          /* TCP to the router: refuse */
        uint8_t r[20];
        uint32_t seq = get32(p + 4), ack = get32(p + 8);
        uint8_t fl = p[13];
        uint8_t ph[12];
        uint32_t s = 0;
        int i;
        if (fl & 0x04) return;                                            /* no reset for a reset */
        if (s_log_tcp < 12) { s_log_tcp++; ipstr(dst, a); fprintf(stderr, "  [NAT] TCP %u -> %s:%u refused (no TCP relay yet)\n", get16(p), a, get16(p + 2)); }
        memset(r, 0, sizeof r);
        put16(r, get16(p + 2)); put16(r + 2, get16(p));
        if (fl & 0x10) { put32(r + 4, ack); r[13] = 0x04; }
        else { put32(r + 8, seq + 1 + (uint32_t)((n - (p[12] >> 4) * 4) > 0 ? (n - (p[12] >> 4) * 4) : 0)); r[13] = 0x14; }
        r[12] = 5 << 4;
        put32(ph, dst); put32(ph + 4, src); ph[8] = 0; ph[9] = 6; put16(ph + 10, 20);
        for (i = 0; i < 12; i += 2) s += (uint32_t)(ph[i] << 8 | ph[i + 1]);
        put16(r + 16, csum(r, 20, s));
        send_ip(dst, src, 6, r, 20);
    }
}

/* UPnP discovery (SSDP M-SEARCH to 239.255.255.250:1900). XNet's sign-in
 * looks for a UPnP gateway first and, with no answer at all, keeps retrying
 * until the sign-in times out (0x80151001) without ever reaching DNS or
 * Kerberos. The router answers as a gateway whose description is on TCP
 * port 2869; fetching it is refused at once (TCP gets a reset), so XNet
 * settles on "no UPnP" right away and goes on to sign in. */
static unsigned s_log_ssdp;
static int ssdp_answer(const uint8_t *f, int len)
{
    const uint8_t *ip = f + 14, *udp, *q;
    int ihl, tot, n, i;
    char st[128] = "upnp:rootdevice", reply[512];
    uint8_t pkt[8 + 512];
    uint32_t src;
    if (f[0] != 0x01 || f[1] != 0x00 || f[2] != 0x5E || f[12] != 0x08 || f[13] != 0x00 || len < 42) return 0;
    ihl = (ip[0] & 15) * 4;
    tot = get16(ip + 2);
    if (ip[9] != 17 || get32(ip + 16) != 0xEFFFFFFAu || ihl < 20 || 14 + tot > len) return 0;
    udp = ip + ihl;
    if (get16(udp + 2) != 1900) return 0;
    q = udp + 8;
    n = tot - ihl - 8;
    if (n < 8 || memcmp(q, "M-SEARCH", 8)) return 1;              /* other SSDP: drop */
    for (i = 0; i + 3 < n; i++)
        if ((i == 0 || q[i - 1] == '\n') && (q[i] == 'S' || q[i] == 's') && (q[i + 1] == 'T' || q[i + 1] == 't') && q[i + 2] == ':') {
            int j = i + 3, k = 0;
            while (j < n && q[j] == ' ') j++;
            while (j < n && q[j] != '\r' && q[j] != '\n' && k + 1 < (int)sizeof st) st[k++] = (char)q[j++];
            st[k] = 0;
            break;
        }
    src = get32(ip + 12);
    n = snprintf(reply, sizeof reply,
                 "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                 "LOCATION: http://10.0.0.1:2869/gateway.xml\r\nSERVER: Windows/10 UPnP/1.0 recomp-nat/1\r\n"
                 "ST: %s\r\nUSN: uuid:7265636f-6d70-2d6e-6174-000000000001::%s\r\n\r\n", st, st);
    if (n <= 0 || n >= (int)sizeof reply) return 1;
    EnterCriticalSection(&s_lock);
    memcpy(s_console_mac, f + 6, 6);
    put16(pkt, 1900); put16(pkt + 2, get16(udp)); put16(pkt + 4, (uint32_t)(8 + n)); put16(pkt + 6, 0);
    memcpy(pkt + 8, reply, (size_t)n);
    send_ip(NAT_ROUTER_IP, src, 17, pkt, 8 + n);
    LeaveCriticalSection(&s_lock);
    if (s_log_ssdp < 4) { s_log_ssdp++; fprintf(stderr, "  [NAT] UPnP search answered (%s)\n", st); }
    return 1;
}

int nat_from_console(const uint8_t *f, int len)
{
    const uint8_t *ip;
    int ihl, tot, off;
    uint32_t src, dst;
    if (!nat_enabled() || len < 14) return 0;
    {
        static int ssdp_on = -1;
        if (ssdp_on < 0) { const char *v = getenv("RECOMP_NET_SSDP"); ssdp_on = v && *v == '1'; }
        if (ssdp_on && ssdp_answer(f, len)) return 1;
    }
    if (f[12] == 0x08 && f[13] == 0x06 && len >= 42) {                    /* ARP */
        if (get16(f + 20) == 1 && get32(f + 38) == NAT_ROUTER_IP) {
            EnterCriticalSection(&s_lock);
            memcpy(s_console_mac, f + 6, 6);
            arp_reply(f);
            LeaveCriticalSection(&s_lock);
            return 1;
        }
        return 0;
    }
    if (memcmp(f, g_nat_router_mac, 6) || f[12] != 0x08 || f[13] != 0x00 || len < 34)
        return 0;
    ip = f + 14;
    ihl = (ip[0] & 15) * 4;
    tot = get16(ip + 2);
    if (ihl < 20 || tot < ihl || 14 + tot > len) return 1;
    src = get32(ip + 12); dst = get32(ip + 16);
    off = get16(ip + 6);
    EnterCriticalSection(&s_lock);
    memcpy(s_console_mac, f + 6, 6);
    if ((off & 0x3FFF) == 0) {
        ip_out(src, dst, ip[9], ip + ihl, tot - ihl);
    } else {                                                              /* a fragment */
        Frag *g = &s_frag;
        int fo = (off & 0x1FFF) * 8, n = tot - ihl, k;
        if (g->id != get16(ip + 4) || g->src != src || g->dst != dst || g->proto != ip[9] ||
            GetTickCount() - g->started > 5000) {
            memset(g->got, 0, sizeof g->got);
            g->id = get16(ip + 4); g->src = src; g->dst = dst; g->proto = ip[9];
            g->have = 0; g->total = 0; g->started = GetTickCount();
        }
        if (fo + n <= (int)sizeof g->data) {
            memcpy(g->data + fo, ip + ihl, (size_t)n);
            for (k = fo / 8; k < (fo + n + 7) / 8; k++) if (!g->got[k]) { g->got[k] = 1; g->have += 8; }
            if (!(off & 0x2000)) g->total = fo + n;
            if (g->total && g->have >= ((g->total + 7) & ~7)) {
                ip_out(src, dst, g->proto, g->data, g->total);
                g->id = 0; g->total = 0;
            }
        }
    }
    LeaveCriticalSection(&s_lock);
    return 1;
}

/* Replies from the internet, into frames for the console. */
static DWORD WINAPI nat_thread(LPVOID unused)
{
    static uint8_t buf[65536 + 8];
    (void)unused;
    /* The router's lock is only held to copy the socket list: injecting takes
     * the NIC's lock, and a transmit holds the NIC's while it calls in here,
     * so holding both the other way round would deadlock. */
    for (;;) {
        fd_set rs;
        struct timeval tv = { 0, 5000 };
        SOCKET socks[MAX_UDP];
        uint16_t cports[MAX_UDP];
        int i, n = 0;
        FD_ZERO(&rs);
        EnterCriticalSection(&s_lock);
        for (i = 0; i < MAX_UDP; i++)
            if (s_udp[i].s != INVALID_SOCKET) {
                socks[n] = s_udp[i].s; cports[n] = s_udp[i].cport;
                FD_SET(socks[n], &rs); n++;
            }
        LeaveCriticalSection(&s_lock);
        if (!n) { Sleep(5); continue; }
        if (select(0, &rs, NULL, NULL, &tv) <= 0) continue;
        for (i = 0; i < n; i++) {
            struct sockaddr_in from;
            int fl = sizeof from, got;
            if (!FD_ISSET(socks[i], &rs)) continue;
            while ((got = recvfrom(socks[i], (char *)buf + 8, 65536, 0, (struct sockaddr *)&from, &fl)) > 0) {
                uint32_t rip = ntohl(from.sin_addr.s_addr);
                if (ntohs(from.sin_port) == 53 && s_ndns && rip == ntohl(s_dns[0])) {
                    uint32_t asked;
                    EnterCriticalSection(&s_lock);
                    asked = dns_redirected_from(cports[i], buf + 8, got);
                    LeaveCriticalSection(&s_lock);
                    if (asked) rip = asked;
                }
                {
                    /* The first replies, by size and first byte (for Kerberos
                     * the message tag: 6B AS-REP, 6D TGS-REP, 7E KRB-ERROR). */
                    static unsigned logged_in;
                    if (logged_in < 24 && ntohs(from.sin_port) != 53) {
                        char a[20];
                        logged_in++;
                        ipstr(rip, a);
                        fprintf(stderr, "  [NAT] UDP %s:%u -> console %u (%d bytes, first byte %02X)\n",
                                a, ntohs(from.sin_port), cports[i], got, got > 0 ? buf[8] : 0);
                    }
                }
                if (s_console_ip) {
                    if (ntohs(from.sin_port) == 3074 || cports[i] == 3074) live_note();
                    put16(buf, ntohs(from.sin_port));
                    put16(buf + 2, cports[i]);
                    put16(buf + 4, (uint32_t)(8 + got));
                    buf[6] = buf[7] = 0;                                  /* no UDP checksum */
                    send_ip(rip, s_console_ip, 17, buf, 8 + got);
                    s_in++;
                }
                fl = sizeof from;
            }
        }
    }
}

int nat_live_recent(unsigned ms)
{
    DWORD t = s_last_live;
    return t && GetTickCount() - t < ms;
}

int nat_live_match_recent(unsigned ms)
{
    DWORD t = s_last_live_match;
    return t && GetTickCount() - t < ms;
}

int nat_enabled(void)
{
    if (s_on < 0) { const char *e = getenv("RECOMP_NET_NAT"); s_on = !(e && e[0] == '0'); }
    return s_on && s_inject != NULL;
}

int nat_dns_servers(uint32_t out[2])
{
    int i;
    for (i = 0; i < s_ndns; i++) out[i] = s_dns[i];
    return s_ndns;
}

void nat_set_internet_gate(int (*blocked)(void)) { s_gate = blocked; }

void nat_init(void (*inject)(const uint8_t *frame, int len))
{
    const char *d = getenv("RECOMP_NET_DNS");
    char buf[256], *tok, *ctx = NULL;
    int i;
    if (s_inject) return;
    {   const char *e = getenv("RECOMP_NET_NAT"); if (e && e[0] == '0') { s_on = 0; return; } }
    InitializeCriticalSection(&s_lock);
    for (i = 0; i < MAX_UDP; i++) s_udp[i].s = INVALID_SOCKET;
    strncpy(buf, d && *d ? d : "46.101.64.175,1.1.1.1", sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (tok = strtok_s(buf, ", ", &ctx); tok && s_ndns < 2; tok = strtok_s(NULL, ", ", &ctx)) {
        struct in_addr a;
        if (inet_pton(AF_INET, tok, &a) == 1) s_dns[s_ndns++] = a.s_addr;
    }
    {
        /* Only a console identity verified against the player's own BIOS
         * (console_id.c) reaches the internet. */
        s_identity = xbox_ConsoleIdentityVerified();
    }
    s_inject = inject;
    s_on = 1;
    {
        HANDLE th = CreateThread(NULL, 0, nat_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    tcp_init();
    fprintf(stderr, "  [NAT] router at 10.0.0.1, %d DNS server(s)%s\n", s_ndns,
            s_identity ? "" : "; internet blocked: no verified console identity");
}

#else
#include <string.h>
const uint8_t g_nat_router_mac[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x01 };
void nat_init(void (*inject)(const uint8_t *frame, int len)) { (void)inject; }
void nat_set_internet_gate(int (*blocked)(void)) { (void)blocked; }
int  nat_enabled(void) { return 0; }
int  nat_live_recent(unsigned ms) { (void)ms; return 0; }
int  nat_live_match_recent(unsigned ms) { (void)ms; return 0; }
int  nat_dns_servers(uint32_t out[2]) { (void)out; return 0; }
int  nat_from_console(const uint8_t *frame, int len) { (void)frame; (void)len; return 0; }
#endif
