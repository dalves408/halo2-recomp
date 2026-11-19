/*
 * nvnet.h -- the MCPX Ethernet controller (NVNET) at 0xFEF00000.
 *
 * The XDK's network stack (XNet) is linked into every title that uses it and
 * drives the NIC's registers and descriptor rings itself, so system link and
 * Xbox Live run the title's own code over a model of the controller. Frames
 * go out through nvnet_link.c. Off unless RECOMP_NET=1.
 */
#ifndef NVNET_H
#define NVNET_H

#include <stdint.h>

void xbox_NvnetInit(void);
int  xbox_NvnetOwnsAddress(uint32_t xbox_va);
int  xbox_NvnetHandleMmio(void *ctx, uint32_t xbox_va);
void xbox_NvnetReport(void);

/* This console's Ethernet address: RECOMP_XBOX_MAC (aa:bb:cc:dd:ee:ff), else
 * one made from the computer's name under Microsoft's 00:50:F2 prefix. The
 * EEPROM's XC_FACTORY_ETHERNET_ADDR answers with it too. */
void xbox_ConsoleMac(uint8_t mac[6]);

#endif
