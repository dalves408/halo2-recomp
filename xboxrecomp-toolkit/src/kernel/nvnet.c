/*
 * nvnet.c -- the MCPX Ethernet controller, enough for the XDK's own driver.
 *
 * Register map and descriptor format as the nForce MAC (forcedeth, and xemu's
 * model of the Xbox's): an 8-byte descriptor {buffer, length, flags}, TX
 * length stored minus one, VALID/AVAIL in flag bit 15 handing a descriptor to
 * the controller. The driver programs the ring bases and sizes, sets the
 * transmitter and receiver going, and kicks TX through TX_RX_CONTROL; the
 * controller walks the rings, raises TX/RX in IRQ_STATUS, and the title's ISR
 * (connected on vector 4) acknowledges by writing the bits back.
 *
 * Registers are answered by trapping the page (PAGE_NOACCESS) and decoding
 * the faulting instruction, as the OHCI model does. A device thread receives
 * frames from the link and raises interrupts; register accesses come from
 * guest threads. One lock guards the register file and the rings, and is
 * never held while guest code runs.
 */
#include "nvnet.h"
#include "nvnet_link.h"
#include "console_id.h"
#include "nvnet_nat.h"
#include "../platform/mmio_decode.h"
#include "xbox_memory_layout.h"
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#if defined(_WIN32)

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __declspec(thread) uint32_t g_ebx, g_esi, g_edi;
extern __declspec(thread) uint32_t g_fs_base;
extern uint32_t xbox_AllocThreadTib(void);

#define NIC_BASE    0xFEF00000u
#define NIC_PAGE    0x1000u
#define NIC_REGS    0x400u
#define NIC_VECTOR  4u          /* HalGetInterruptVector(4): the NIC's IRQ */

/* Registers (byte offsets) */
#define R_IRQ_STATUS        0x000
#define R_IRQ_MASK          0x004
#define R_TX_CONTROL        0x084
#define R_TX_STATUS         0x088
#define R_PACKET_FILTER     0x08C
#define R_RX_CONTROL        0x094
#define R_RX_STATUS         0x098
#define R_MAC_A             0x0A8
#define R_MAC_B             0x0AC
#define R_MCAST_ADDR_A      0x0B0
#define R_MCAST_ADDR_B      0x0B4
#define R_MCAST_MASK_A      0x0B8
#define R_MCAST_MASK_B      0x0BC
#define R_TX_RING           0x100
#define R_RX_RING           0x104
#define R_RING_SIZE         0x108
#define R_LINKSPEED         0x110
#define R_TX_CUR            0x11C
#define R_RX_CUR            0x120
#define R_SETUP5            0x130
#define R_TX_NEXT           0x134
#define R_RX_NEXT           0x138
#define R_TXRX_CONTROL      0x144
#define R_MII_STATUS        0x180
#define R_ADAPTER_CONTROL   0x188
#define R_MDIO_ADDR         0x190
#define R_MDIO_DATA         0x194
#define R_POWERSTATE        0x26C

#define IRQ_RX          0x02u
#define IRQ_RX_NOBUF    0x04u
#define IRQ_TX          0x10u
#define IRQ_MIIEVENT    0x40u

#define TXRX_KICK       0x01u
#define TXRX_BIT1       0x02u
#define TXRX_DMA_OFF    0x04u
#define TXRX_IDLE       0x08u
#define TXRX_RESET      0x10u

#define ADAPTER_LINKUP  0x04u
#define MDIO_INUSE      0x8000u
#define MDIO_WRITE      0x0400u
#define FILTER_MYADDR   0x20u
#define SETUP5_BIT31    0x80000000u
#define MII_LINKCHANGE  0x08u

#define TX_LAST         0x0001u
#define TX_LAST1        0x0100u
#define TX_VALID        0x8000u
#define TX_ERRBITS      0x7C08u     /* RETRYERROR, DEFERRED..ERROR */
#define RX_DESCVALID    0x0001u
#define RX_BIT4         0x0010u
#define RX_AVAIL        0x8000u

#define RXQ             64

typedef struct { uint8_t data[NVLINK_MAX_FRAME]; int len; } Frame;

static int s_enabled;
static CRITICAL_SECTION s_lock;
static uint32_t s_reg[NIC_REGS / 4];
static uint16_t s_phy[32];
static uint8_t  s_mac[6];
static uint8_t  s_txbuf[NVLINK_MAX_FRAME];
static int      s_txlen;
static Frame    s_rxq[RXQ];
static int      s_rxq_head, s_rxq_n;
static unsigned s_reads, s_writes, s_undecoded, s_tx_frames, s_rx_frames, s_rx_dropped, s_irqs;
static int      s_trace;
static unsigned s_rx_link, s_rx_filtered, s_stat_sum;
static DWORD    s_stat_at;

/* An Ethernet address for the log: the maker prefix only (a console's address
 * is part of the identity it registered with Xbox Live, and logs are sent with
 * problem reports). Broadcast is shown whole. */
static void mac_text(const uint8_t *m, char out[20])
{
    if (m[0] == 0xFF && m[1] == 0xFF && m[2] == 0xFF && m[3] == 0xFF && m[4] == 0xFF && m[5] == 0xFF)
        strcpy(out, "FF:FF:FF:FF:FF:FF");
    else
        sprintf(out, "%02X:%02X:%02X:..", m[0], m[1], m[2]);
}

/* One line about a frame for the log: MACs, and IPv4/UDP addresses and ports. */
static void frame_desc(const uint8_t *f, int n, char *out, size_t cap)
{
    char src[20], dst[20];
    int k;
    mac_text(f + 6, src); mac_text(f, dst);
    k = snprintf(out, cap, "%d B %s->%s %02X%02X", n, src, dst, f[12], f[13]);
    if (n >= 34 && f[12] == 8 && f[13] == 0 && k > 0 && (size_t)k < cap) {
        const uint8_t *ip = f + 14;
        int ihl = (ip[0] & 15) * 4;
        k += snprintf(out + k, cap - (size_t)k, " %u.%u.%u.%u->%u.%u.%u.%u p%u", ip[12], ip[13], ip[14], ip[15],
                      ip[16], ip[17], ip[18], ip[19], ip[9]);
        if (ip[9] == 17 && n >= 14 + ihl + 4 && k > 0 && (size_t)k < cap)
            snprintf(out + k, cap - (size_t)k, " udp %u->%u", ip[ihl] << 8 | ip[ihl + 1], ip[ihl + 2] << 8 | ip[ihl + 3]);
    }
}

#define REG(o) s_reg[(o) / 4]

/* ---- DMA: the driver hands over physical addresses (see ohci.c) ---------- */
#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE (64u * 1024u * 1024u)

/* XNet builds its rings in contiguous memory but its packet buffers in the
 * pool, and MmGetPhysicalAddress passes a pool address through unchanged:
 * the same number can name either. A heap page the title named through
 * MmGetPhysicalAddress is RAM (xbox_PhysRamNamedAt); otherwise an address
 * below the window's high-water mark is the window's. */
static uint32_t bus_resolve(uint32_t addr)
{
    if (addr >= g_xbox_image_lo && addr < g_xbox_image_hi)
        return addr;
    if (xbox_PhysRamNamedAt(addr))
        return addr;
    if (addr && addr < xbox_ContiguousAllocatedBytes())
        return CONTIG_BASE + addr;
    return addr;
}

static uint8_t *guest(uint32_t phys, uint32_t bytes)
{
    uint32_t va = bus_resolve(phys);
    size_t mapped = xbox_GetMappedSize();
    if (!va || !mapped)
        return NULL;
    if (va >= CONTIG_BASE && (uint64_t)va + bytes <= (uint64_t)CONTIG_BASE + CONTIG_SIZE)
        return (uint8_t *)xbox_GetMemoryOffset() + va;
    if ((uint64_t)va + bytes <= (uint64_t)mapped)
        return (uint8_t *)xbox_GetMemoryOffset() + va;
    return NULL;
}

/* ---- identity ------------------------------------------------------------ */
void xbox_ConsoleMac(uint8_t mac[6])
{
    const char *e = getenv("RECOMP_XBOX_MAC");
    unsigned v[6];
    if (e && sscanf(e, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        int i;
        for (i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
        return;
    }
    if (xbox_ConsoleHasEeprom()) {          /* the player's console's (console_id.c) */
        memcpy(mac, xbox_ConsoleEeprom() + 0x40, 6);
        return;
    }
    {
        char name[256];
        DWORD n = sizeof name;
        uint32_t h = 2166136261u;
        DWORD i;
        if (!GetComputerNameA(name, &n)) { strcpy(name, "xbox"); n = 4; }
        for (i = 0; i < n; i++) h = (h ^ (uint8_t)name[i]) * 16777619u;
        mac[0] = 0x00; mac[1] = 0x50; mac[2] = 0xF2;
        mac[3] = (uint8_t)(h >> 16); mac[4] = (uint8_t)(h >> 8); mac[5] = (uint8_t)h;
    }
}

/* ---- rings ------------------------------------------------------------- */
static uint32_t ring_entries(int tx)
{
    uint32_t rs = REG(R_RING_SIZE);
    return (tx ? (rs & 0xFFFFu) : (rs >> 16)) + 1u;
}

/* The descriptor to work on: NEXT, pulled back to the base when it is outside
 * the ring, and CUR made to match. */
static uint32_t ring_at(int tx)
{
    uint32_t base = REG(tx ? R_TX_RING : R_RX_RING);
    uint32_t end = base + ring_entries(tx) * 8u;
    uint32_t next = REG(tx ? R_TX_NEXT : R_RX_NEXT);
    if (next < base || next + 8u > end)
        next = base;
    REG(tx ? R_TX_NEXT : R_RX_NEXT) = next;
    REG(tx ? R_TX_CUR : R_RX_CUR) = next;
    return next;
}

static void ring_advance(int tx)
{
    uint32_t base = REG(tx ? R_TX_RING : R_RX_RING);
    uint32_t end = base + ring_entries(tx) * 8u;
    uint32_t next = REG(tx ? R_TX_CUR : R_RX_CUR) + 8u;
    if (next >= end)
        next = base;
    REG(tx ? R_TX_NEXT : R_RX_NEXT) = next;
}

static int dma_on(void)
{
    return !(REG(R_TXRX_CONTROL) & TXRX_DMA_OFF);
}

static void rxq_push(const uint8_t *f, int len)
{
    Frame *fr;
    if (s_rxq_n >= RXQ || len > NVLINK_MAX_FRAME) { s_rx_dropped++; return; }
    fr = &s_rxq[(s_rxq_head + s_rxq_n) % RXQ];
    memcpy(fr->data, f, (size_t)len);
    fr->len = len;
    s_rxq_n++;
}

/* ---- DHCP, answered here --------------------------------------------------
 * XNet starts with DHCP for every game, single player included, and waits out
 * a 20 s timeout before falling back when nobody answers -- once per map load.
 * A console on a LAN with no router is in the same place, so the NIC answers
 * its own DHCP at once: 169.254.x.y/16 from the MAC, no gateway, no DNS. Every
 * instance on the link is then on one subnet, which is what system link needs.
 * RECOMP_NET_DHCP=0 lets the request out instead. */
static uint16_t ip_csum(const uint8_t *p, int n)
{
    uint32_t s = 0;
    int i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)~s;
}

/* peer: the request came over the link from an xemu (nvlink_last_from_xemu),
 * whose udp backend has no DHCP server. It gets the address its MAC gives
 * under the same rule -- the same subnet as ours, which system link needs --
 * but no router and no DNS (its traffic is not ours to route), and the
 * answer goes back to it over the link. */
static int dhcp_answer_for(const uint8_t *f, int len, int peer)
{
    static const uint8_t srv_mac[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x01 };
    const uint8_t *ip, *udp, *bp, *opt;
    const uint8_t *m = peer ? f + 6 : s_mac;
    uint8_t r[400], *rb, *o;
    int ihl, type = 0, i, blen;
    uint8_t yi[4], sv[4] = { 169, 254, 0, 1 };

    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00)
        return 0;
    ip = f + 14;
    ihl = (ip[0] & 15) * 4;
    if (ip[9] != 17 || len < 14 + ihl + 8 + 240)
        return 0;
    udp = ip + ihl;
    if (udp[0] != 0 || udp[1] != 68 || udp[2] != 0 || udp[3] != 67)
        return 0;
    bp = udp + 8;
    if (bp[0] != 1 || bp[236] != 99 || bp[237] != 130 || bp[238] != 83 || bp[239] != 99)
        return 0;
    for (opt = bp + 240; opt < f + len && *opt != 255; ) {
        if (*opt == 0) { opt++; continue; }
        if (opt + 2 > f + len) break;
        if (opt[0] == 53 && opt[1] >= 1) type = opt[2];
        opt += 2 + opt[1];
    }
    if (type != 1 && type != 3)             /* DISCOVER, REQUEST */
        return type != 0;                   /* other DHCP: swallow */
    if (nat_enabled()) {
        /* behind the router (nvnet_nat.c): 10.x.y.z/8 from the MAC, every
         * console on one subnet for system link, 10.0.0.1 the way out */
        yi[0] = 10; yi[1] = m[3]; yi[2] = m[4]; yi[3] = (uint8_t)(2 + m[5] % 252);
        sv[0] = 10; sv[1] = 0; sv[2] = 0; sv[3] = 1;
    } else {
        yi[0] = 169; yi[1] = 254;
        yi[2] = (uint8_t)(1 + m[4] % 253); yi[3] = (uint8_t)(1 + (m[5] ^ m[3]) % 253);
    }

    memset(r, 0, sizeof r);
    memset(r, 0xFF, 6); memcpy(r + 6, srv_mac, 6); r[12] = 0x08; r[13] = 0x00;
    rb = r + 14 + 20 + 8;
    rb[0] = 2; rb[1] = 1; rb[2] = 6;
    memcpy(rb + 4, bp + 4, 4);              /* xid */
    memcpy(rb + 10, bp + 10, 2);            /* flags */
    memcpy(rb + 16, yi, 4);                 /* yiaddr */
    memcpy(rb + 20, sv, 4);                 /* siaddr */
    memcpy(rb + 28, bp + 28, 16);           /* chaddr */
    rb[236] = 99; rb[237] = 130; rb[238] = 83; rb[239] = 99;
    o = rb + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)(type == 1 ? 2 : 5);   /* OFFER / ACK */
    *o++ = 54; *o++ = 4; memcpy(o, sv, 4); o += 4;
    *o++ = 51; *o++ = 4; *o++ = 0xFF; *o++ = 0xFF; *o++ = 0xFF; *o++ = 0xFF;
    if (nat_enabled() && !peer) {
        uint32_t dns[2];
        int nd = nat_dns_servers(dns), k;
        *o++ = 1; *o++ = 4; *o++ = 255; *o++ = 0; *o++ = 0; *o++ = 0;          /* 255.0.0.0 */
        *o++ = 3; *o++ = 4; memcpy(o, sv, 4); o += 4;                         /* router */
        if (nd) {
            *o++ = 6; *o++ = (uint8_t)(4 * nd);                               /* DNS */
            for (k = 0; k < nd; k++) { memcpy(o, &dns[k], 4); o += 4; }
        }
    } else if (nat_enabled()) {
        *o++ = 1; *o++ = 4; *o++ = 255; *o++ = 0; *o++ = 0; *o++ = 0;          /* 255.0.0.0, no router */
    } else {
        *o++ = 1;  *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 0; *o++ = 0;
    }
    *o++ = 255;
    blen = (int)(o - rb);
    if (blen < 300) blen = 300;
    {   /* UDP 67 -> 68, no checksum; IPv4 to broadcast */
        uint8_t *u = r + 14 + 20, *h = r + 14;
        int ulen = 8 + blen;
        u[0] = 0; u[1] = 67; u[2] = 0; u[3] = 68;
        u[4] = (uint8_t)(ulen >> 8); u[5] = (uint8_t)ulen;
        h[0] = 0x45; h[2] = (uint8_t)((20 + ulen) >> 8); h[3] = (uint8_t)(20 + ulen);
        h[8] = 64; h[9] = 17;
        memcpy(h + 12, sv, 4);
        h[16] = h[17] = h[18] = h[19] = 255;
        i = ip_csum(h, 20); h[10] = (uint8_t)(i >> 8); h[11] = (uint8_t)i;
        if (peer) nvlink_send_xemu(r, 14 + 20 + ulen);
        else rxq_push(r, 14 + 20 + ulen);
    }
    if (peer) {
        static unsigned npeer;
        if (npeer++ < 4)
            fprintf(stderr, "  [NVNET] xemu's DHCP %s answered: same subnet, no router\n", type == 1 ? "discover" : "request");
    }
    if (s_trace)
        fprintf(stderr, "  [NVNET] DHCP %s ->%u.%u.%u.%u\n", type == 1 ? "discover" : "request", yi[0], yi[1], yi[2], yi[3]);
    return 1;
}

static int dhcp_answer(const uint8_t *f, int len) { return dhcp_answer_for(f, len, 0); }

static int s_dhcp_local = 1;

/* A frame from the router (nvnet_nat.c) for the console: into the receive
 * queue. The lock is the NIC's; a transmit already holding it re-enters. */
static void nic_inject(const uint8_t *f, int len)
{
    EnterCriticalSection(&s_lock);
    rxq_push(f, len);
    LeaveCriticalSection(&s_lock);
}

static void tx_kick(void)
{
    uint32_t n = ring_entries(1), i;
    int sent = 0;
    if (!(REG(R_TX_CONTROL) & 1u) || !dma_on() || !REG(R_TX_RING))
        return;
    for (i = 0; i < n; i++) {
        uint32_t at = ring_at(1);
        uint8_t *d = guest(at, 8);
        uint32_t buf, len;
        uint16_t flags;
        uint8_t *src;
        if (!d) break;
        memcpy(&buf, d, 4);
        len = (uint32_t)*(uint16_t *)(d + 4) + 1u;
        flags = *(uint16_t *)(d + 6);
        if (!(flags & TX_VALID))
            break;
        src = guest(buf, len);
        if (s_trace && s_tx_frames < 8)
            fprintf(stderr, "  [NVNET] tx desc %08X buf %08X -> %08X len %u flags %04X contig %08X img %08X-%08X\n",
                    at, buf, bus_resolve(buf), len, flags, xbox_ContiguousAllocatedBytes(), g_xbox_image_lo, g_xbox_image_hi);
        if (src && s_txlen + (int)len <= NVLINK_MAX_FRAME) {
            memcpy(s_txbuf + s_txlen, src, len);
            s_txlen += (int)len;
        }
        flags &= (uint16_t)~(TX_VALID | TX_ERRBITS);
        *(uint16_t *)(d + 6) = flags;
        ring_advance(1);
        if (flags & (TX_LAST | TX_LAST1)) {
            if (s_tx_frames < (s_trace ? 200u : 16u)) {
                char dsc[160];
                frame_desc(s_txbuf, s_txlen, dsc, sizeof dsc);
                fprintf(stderr, "  [NVNET] tx %s\n", dsc);
            }
            if (!(s_dhcp_local && dhcp_answer(s_txbuf, s_txlen)) && !nat_from_console(s_txbuf, s_txlen))
                nvlink_send(s_txbuf, s_txlen);
            s_tx_frames++;
            s_txlen = 0;
            sent = 1;
        }
    }
    if (sent)
        REG(R_IRQ_STATUS) |= IRQ_TX;
}

static int rx_accept(const uint8_t *f, int len)
{
    static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };
    uint8_t mine[6];
    uint32_t a = REG(R_MAC_A), b = REG(R_MAC_B);
    if (len < 14)
        return 0;
    if (!memcmp(f, bcast, 6))
        return 1;
    if (!(REG(R_PACKET_FILTER) & FILTER_MYADDR))
        return 1;
    if (f[0] & 1u) {            /* multicast: the masked compare */
        uint32_t ma = REG(R_MCAST_ADDR_A), mb = REG(R_MCAST_ADDR_B);
        uint32_t ka = REG(R_MCAST_MASK_A), kb = REG(R_MCAST_MASK_B);
        uint32_t fa = (uint32_t)f[0] | ((uint32_t)f[1] << 8) | ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
        uint32_t fb = (uint32_t)f[4] | ((uint32_t)f[5] << 8);
        return (fa & ka) == (ma & ka) && (fb & kb & 0xFFFFu) == (mb & kb & 0xFFFFu);
    }
    mine[0] = (uint8_t)a; mine[1] = (uint8_t)(a >> 8); mine[2] = (uint8_t)(a >> 16); mine[3] = (uint8_t)(a >> 24);
    mine[4] = (uint8_t)b; mine[5] = (uint8_t)(b >> 8);
    return !memcmp(f, mine, 6);
}

/* Queued frames into RX descriptors the driver has made available. */
static void rx_deliver(void)
{
    while (s_rxq_n) {
        Frame *fr = &s_rxq[s_rxq_head];
        uint32_t at, buf;
        uint8_t *d, *dst;
        uint16_t flags, cap;
        if (!(REG(R_RX_CONTROL) & 1u) || !dma_on() || !REG(R_RX_RING))
            return;
        at = ring_at(0);
        d = guest(at, 8);
        if (!d) return;
        flags = *(uint16_t *)(d + 6);
        if (!(flags & RX_AVAIL))
            return;                         /* no buffer yet: keep it queued */
        memcpy(&buf, d, 4);
        cap = (uint16_t)(*(uint16_t *)(d + 4) + 1u);
        if (fr->len <= cap && (dst = guest(buf, (uint32_t)fr->len)) != NULL) {
            memcpy(dst, fr->data, (size_t)fr->len);
            *(uint16_t *)(d + 4) = (uint16_t)fr->len;
            *(uint16_t *)(d + 6) = RX_BIT4 | RX_DESCVALID;
            s_rx_frames++;
        } else {
            *(uint16_t *)(d + 6) = RX_BIT4 | RX_DESCVALID | 0x4000u;    /* RX_ERROR */
            s_rx_dropped++;
        }
        ring_advance(0);
        REG(R_IRQ_STATUS) |= IRQ_RX;
        s_rxq_head = (s_rxq_head + 1) % RXQ;
        s_rxq_n--;
    }
}

/* ---- registers --------------------------------------------------------- */
static void phy_reset(void)
{
    memset(s_phy, 0, sizeof s_phy);
    s_phy[0] = 0x1100;      /* BMCR: autoneg on, full duplex */
    s_phy[1] = 0x782D;      /* BMSR: 10/100 capable, autoneg complete, link up */
    s_phy[2] = 0x0000; s_phy[3] = 0x0000;
    s_phy[4] = 0x01E1;      /* ANAR: 10/100 half/full */
    s_phy[5] = 0x45E1;      /* ANLPAR: the same, acknowledged */
}

static void nic_reset(void)
{
    memset(s_reg, 0, sizeof s_reg);
    REG(R_TXRX_CONTROL) = TXRX_IDLE;
    REG(R_ADAPTER_CONTROL) = ADAPTER_LINKUP;
    REG(R_LINKSPEED) = 100;
    REG(R_POWERSTATE) = 0x8000u;            /* powered up */
    phy_reset();
    s_txlen = 0;
}

static uint32_t nic_read32(uint32_t off)
{
    uint32_t v;
    if (off >= NIC_REGS)
        return 0;
    v = s_reg[off / 4];
    switch (off) {
    case R_TXRX_CONTROL: v |= TXRX_IDLE; break;
    case R_ADAPTER_CONTROL: v |= ADAPTER_LINKUP; break;
    case R_MDIO_ADDR: v &= ~MDIO_INUSE; break;
    case R_TX_STATUS: case R_RX_STATUS: v = 0; break;      /* never busy */
    }
    return v;
}

static void nic_write32(uint32_t off, uint32_t v)
{
    if (off >= NIC_REGS)
        return;
    switch (off) {
    case R_IRQ_STATUS:
    case R_MII_STATUS:
        s_reg[off / 4] &= ~v;                   /* write 1 to clear */
        return;
    case R_MDIO_ADDR: {
        uint32_t phyaddr = (v >> 5) & 0x1Fu, reg = v & 0x1Fu;
        if (v & MDIO_WRITE) {
            if (phyaddr == 1) s_phy[reg] = (uint16_t)REG(R_MDIO_DATA);
        } else {
            REG(R_MDIO_DATA) = phyaddr == 1 ? s_phy[reg] : 0xFFFFFFFFu;
        }
        REG(R_MDIO_ADDR) = v & ~MDIO_INUSE;
        return;
    }
    case R_TXRX_CONTROL:
        if (v == 0)
            REG(R_SETUP5) |= SETUP5_BIT31;
        if (v & TXRX_BIT1)
            REG(R_IRQ_STATUS) = 0;
        if (v & TXRX_RESET) {
            REG(R_TX_CUR) = REG(R_TX_NEXT) = REG(R_TX_RING);
            REG(R_RX_CUR) = REG(R_RX_NEXT) = REG(R_RX_RING);
            s_txlen = 0;
        }
        REG(R_TXRX_CONTROL) = v & ~(TXRX_KICK | TXRX_IDLE);
        if (v & TXRX_KICK)
            tx_kick();
        rx_deliver();
        return;
    case R_TX_RING:
        REG(R_TX_RING) = v; REG(R_TX_CUR) = REG(R_TX_NEXT) = v;
        return;
    case R_RX_RING:
        REG(R_RX_RING) = v; REG(R_RX_CUR) = REG(R_RX_NEXT) = v;
        return;
    }
    s_reg[off / 4] = v;
    if (off == R_RX_CONTROL)
        rx_deliver();
}

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    uint32_t v;
    (void)dev;
    EnterCriticalSection(&s_lock);
    s_reads++;
    v = nic_read32(off & ~3u);
    LeaveCriticalSection(&s_lock);
    if (size == 8) {
        uint64_t hi;
        EnterCriticalSection(&s_lock);
        hi = nic_read32((off & ~3u) + 4);
        LeaveCriticalSection(&s_lock);
        return v | (hi << 32);
    }
    v >>= (off & 3u) * 8u;
    if (size == 1) return v & 0xFFu;
    if (size == 2) return v & 0xFFFFu;
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    (void)dev;
    EnterCriticalSection(&s_lock);
    s_writes++;
    if (s_trace && s_writes < 200)
        fprintf(stderr, "  [NVNET] w %03X = %08X (%d)\n", off, (uint32_t)val, size);
    if (size == 8) {
        nic_write32(off, (uint32_t)val);
        nic_write32(off + 4, (uint32_t)(val >> 32));
    } else if (size == 4) {
        nic_write32(off, (uint32_t)val);
    } else {                                    /* merge a narrow write */
        uint32_t sh = (off & 3u) * 8u, m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        uint32_t cur = s_reg[(off & ~3u) / 4];
        nic_write32(off & ~3u, (cur & ~m) | (((uint32_t)val << sh) & m));
    }
    LeaveCriticalSection(&s_lock);
}

int xbox_NvnetOwnsAddress(uint32_t va)
{
    return s_enabled && va >= NIC_BASE && va < NIC_BASE + NIC_PAGE;
}

int xbox_NvnetHandleMmio(void *ctx, uint32_t va)
{
    int ok;
    if (!xbox_NvnetOwnsAddress(va))
        return 0;
    ok = mmio_emulate((PCONTEXT)ctx, va - NIC_BASE, NULL, nic_read, nic_write);
    if (!ok && s_undecoded++ < 20) {
        const uint8_t *ip = (const uint8_t *)((PCONTEXT)ctx)->Rip;
        fprintf(stderr, "  [NVNET] undecoded access at +0x%03X: %02X %02X %02X %02X %02X %02X\n",
                va - NIC_BASE, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
    }
    return ok;
}

/* ---- interrupts -------------------------------------------------------- */
/* XNet asks HalGetInterruptVector for the NIC's level (4); a route that
 * answers 0 leaves it connected on vector 0, which nothing else uses. */
static uint32_t nic_kint(void)
{
    uint32_t k = xbox_GetConnectedInterrupt(NIC_VECTOR);
    return k ? k : xbox_GetConnectedInterrupt(0);
}

static int isr_call(void)
{
    uint32_t kint = nic_kint();
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t routine, context;
    recomp_func_t fn;
    if (!kint)
        return -1;
    routine = *(uint32_t *)(mem + kint + 0);
    context = *(uint32_t *)(mem + kint + 4);
    if (!routine || !(fn = recomp_lookup(routine)))
        return -1;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;
    { int irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(irql); }
    s_irqs++;
    return (int)(g_eax & 1u);
}

static void isr_on_holder(void)
{
    isr_call();
}

static void isr_on_worker(void)
{
    int slot = xbox_worker_stack_alloc();
    if (slot < 0)
        return;
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    isr_call();
    xbox_worker_stack_free(slot);
}

/* Level-triggered: called each pass while an unmasked source is set. */
static void raise_line(void)
{
    static unsigned held_off;
    int gated;
    xbox_IrqSetHandler(XBOX_IRQ_NIC, isr_on_holder);
    if (xbox_IrqSafePointsOn()) {
        int how = xbox_IrqPost(XBOX_IRQ_NIC);
        if (how == XBOX_IRQ_POSTED)
            return;
        gated = how == XBOX_IRQ_GATED;
        if (!gated) xbox_IrqNoteUngated(XBOX_IRQ_NIC);
    } else {
        gated = xbox_DispatchGateOn() ? xbox_DispatchGateTryEnter() : !xbox_IrqlBlocksInterrupts();
        if (!gated && ++held_off <= 500)
            return;
        held_off = 0;
        if (!gated) xbox_IrqNoteUngated(XBOX_IRQ_NIC);
    }
    isr_on_worker();
    if (gated)
        xbox_DispatchGateLeave();
}

static DWORD WINAPI nic_thread(LPVOID unused)
{
    static uint8_t f[NVLINK_MAX_FRAME];
    uint32_t tib = xbox_AllocThreadTib();
    (void)unused;
    if (!tib) {
        fprintf(stderr, "  [NVNET] no TIB for the device thread; NIC off\n");
        return 0;
    }
    g_fs_base = tib;
    for (;;) {
        int n = nvlink_recv(f, sizeof f, 1);
        uint32_t pending;
        DWORD now = GetTickCount();
        EnterCriticalSection(&s_lock);
        if (n > 0 && s_dhcp_local && nvlink_last_from_xemu() && dhcp_answer_for(f, n, 1))
            n = 0;                          /* xemu's DHCP, answered over the link */
        if (n > 0) {
            int ok = rx_accept(f, n);
            s_rx_link++;
            if (!ok) s_rx_filtered++;
            if (s_rx_link <= 12) {
                char d[160];
                frame_desc(f, n, d, sizeof d);
                fprintf(stderr, "  [NVNET] rx %s%s\n", d, ok ? "" : " (filtered)");
            }
            if (ok) rxq_push(f, n);
        }
        rx_deliver();
        pending = REG(R_IRQ_STATUS) & REG(R_IRQ_MASK);
        if (now - s_stat_at >= 5000) {
            unsigned sum = s_tx_frames + s_rx_link + s_rx_frames + s_irqs;
            if (sum != s_stat_sum) {
                uint32_t at = REG(R_RX_NEXT);
                uint8_t *dd = REG(R_RX_RING) ? guest(at, 8) : NULL;
                fprintf(stderr, "  [NVNET] tx %u, link rx %u (filtered %u), delivered %u, dropped %u, queued %d, ISR %u; "
                                "rxctl %X txrx %X irq %X/%X rx desc %08X flags %04X\n",
                        s_tx_frames, s_rx_link, s_rx_filtered, s_rx_frames, s_rx_dropped, s_rxq_n, s_irqs,
                        REG(R_RX_CONTROL), REG(R_TXRX_CONTROL), REG(R_IRQ_STATUS), REG(R_IRQ_MASK),
                        at, dd ? *(uint16_t *)(dd + 6) : 0xFFFFu);
                s_stat_sum = sum;
            }
            s_stat_at = now;
        }
        LeaveCriticalSection(&s_lock);
        if (pending && nic_kint())
            raise_line();
    }
}

void xbox_NvnetInit(void)
{
    static int done;
    const char *e = getenv("RECOMP_NET");
    if (done) return;
    done = 1;
    if (!e || e[0] != '1')
        return;
    InitializeCriticalSection(&s_lock);
    s_trace = getenv("RECOMP_NET_TRACE") != NULL;
    { const char *d = getenv("RECOMP_NET_DHCP"); s_dhcp_local = !(d && d[0] == '0'); }
    xbox_ConsoleMac(s_mac);
    nic_reset();
    {
        ptrdiff_t off = xbox_GetMemoryOffset();
        DWORD old;
        if (!off || !VirtualProtect((LPVOID)((uintptr_t)off + NIC_BASE), NIC_PAGE, PAGE_NOACCESS, &old)) {
            fprintf(stderr, "  [NVNET] cannot trap the NIC registers; NIC off\n");
            return;
        }
    }
    s_enabled = 1;
    nat_init(nic_inject);
    if (nvlink_open(s_mac) != 0)
        fprintf(stderr, "  [NVNET] no network link; the NIC sends nowhere\n");
    {
        char m[20];
        mac_text(s_mac, m);
        fprintf(stderr, "  [NVNET] NIC at 0x%08X, MAC %s\n", NIC_BASE, m);
    }
    {
        HANDLE th = CreateThread(NULL, 0, nic_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
}

void xbox_NvnetReport(void)
{
    if (!s_enabled) return;
    fprintf(stderr, "  [NVNET] %u reads, %u writes, %u undecoded; tx %u, rx %u (%u dropped), %u ISR calls; "
                    "irq %08X/%08X txrx %08X rxctl %08X txctl %08X\n",
            s_reads, s_writes, s_undecoded, s_tx_frames, s_rx_frames, s_rx_dropped, s_irqs,
            REG(R_IRQ_STATUS), REG(R_IRQ_MASK), REG(R_TXRX_CONTROL), REG(R_RX_CONTROL), REG(R_TX_CONTROL));
}

#else   /* POSIX hosts: not yet */
void xbox_NvnetInit(void) {}
int  xbox_NvnetOwnsAddress(uint32_t va) { (void)va; return 0; }
int  xbox_NvnetHandleMmio(void *ctx, uint32_t va) { (void)ctx; (void)va; return 0; }
void xbox_NvnetReport(void) {}
void xbox_ConsoleMac(uint8_t mac[6])
{
    mac[0] = 0x00; mac[1] = 0x50; mac[2] = 0xF2; mac[3] = 0x01; mac[4] = 0x02; mac[5] = 0x03;
}
#endif
