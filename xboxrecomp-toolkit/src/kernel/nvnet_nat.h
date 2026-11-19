/* nvnet_nat.h -- the router behind the NIC model: the console's way to the
 * internet (Xbox Live / Insignia). See nvnet_nat.c. */
#ifndef NVNET_NAT_H
#define NVNET_NAT_H

#include <stdint.h>

/* Router and console addressing handed out by the NIC's DHCP answer. */
#define NAT_ROUTER_IP   0x0A000001u     /* 10.0.0.1 */
#define NAT_NETMASK     0xFF000000u     /* 255.0.0.0 */

extern const uint8_t g_nat_router_mac[6];

/* Start the router; inject(frame, len) hands a frame to the console (the NIC's
 * receive queue, taking its own lock). Off unless RECOMP_NET_NAT is not 0. */
void nat_init(void (*inject)(const uint8_t *frame, int len));
int  nat_enabled(void);

/* The DNS servers DHCP gives the console (network order, up to 2). */
int  nat_dns_servers(uint32_t out[2]);

/* A frame the console sent. Returns 1 if the router took it (it was for the
 * router or the internet), 0 to send it on the system link. */
int  nat_from_console(const uint8_t *frame, int len);

/* A title's own rule for the internet, asked for every packet bound beyond
 * the router: nonzero drops it (the console sees no answer, as with no
 * console identity). NULL (the default) lets everything out. Called on the
 * NIC's thread, so it must be quick. */
void nat_set_internet_gate(int (*blocked)(void));

/* Nonzero while the console is on Xbox Live: a packet went to or came from
 * the security gateway (UDP 3074) within the last `ms` milliseconds. System
 * link never goes through the router, so it never counts. */
int  nat_live_recent(unsigned ms);

/* Nonzero while an Xbox Live match is in progress: within the last `ms`
 * milliseconds a second carried at least 8 packets of UDP 3074 traffic (game
 * traffic with other consoles), far above a signed-in console's keep-alives. */
int  nat_live_match_recent(unsigned ms);

#endif
