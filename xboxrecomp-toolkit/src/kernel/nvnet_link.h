/* nvnet_link.h -- Ethernet frames over UDP between hosts (nvnet_link.c). */
#ifndef NVNET_LINK_H
#define NVNET_LINK_H

#include <stdint.h>

#define NVLINK_MAX_FRAME 1600

/* Bind the UDP port and find where to send; mac is this console's, to drop
 * our own frames when they come back. 0 on success. */
int  nvlink_open(const uint8_t mac[6]);
/* One frame, Ethernet header first, no FCS. */
void nvlink_send(const uint8_t *frame, int len);
/* Wait up to timeout_ms for one frame; its length, or 0. */
int  nvlink_recv(uint8_t *frame, int max, int timeout_ms);
/* The RECOMP_NET_XEMU peer: did the frame nvlink_recv just returned come
 * from it, and send one frame to it alone (untagged). */
int  nvlink_last_from_xemu(void);
void nvlink_send_xemu(const uint8_t *frame, int len);

#endif
