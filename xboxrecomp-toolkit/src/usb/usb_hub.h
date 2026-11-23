/*
 * usb_hub.h -- the hub inside an Xbox controller.
 *
 * A console controller is a USB hub with the gamepad and two expansion slots
 * behind it: hub port 1 is the gamepad, port 2 the first (top) slot and port
 * 3 the second. A memory unit or a Communicator goes in a slot, and XAPI
 * decides which player a slot belongs to from the root port the hub is on.
 * xemu attaches its Communicator the same way, at <root>.<controller>.2.
 *
 * The model is a USB 1.1 hub with three ports: the standard and hub class
 * requests and a status-change interrupt endpoint. Port state follows the
 * USB 1.1 specification, chapter 11. Data never goes through it: every device
 * on the bus is answered by function address (ohci.c), as a real hub's
 * repeater is invisible to the host too.
 */
#ifndef XBOX_USB_HUB_H
#define XBOX_USB_HUB_H

#include <stdint.h>
#include "usb_gamepad.h"            /* UsbSetup */

#define USB_HUB_MAX    4            /* one per controller port */
#define USB_HUB_PORTS  3            /* gamepad, top slot, bottom slot */

/* Answer a control request to hub `hub`. Bytes written, or -1 to stall. */
int usb_hub_control(int hub, const UsbSetup *setup, uint8_t *out, int max);

/* The status-change endpoint: a bitmap (bit n = port n changed) when there
 * is a change to report, or 0 when there is none, which the controller
 * answers with NAK and keeps the transfer for later. */
int usb_hub_status_change(int hub, uint8_t *out, int max);

uint8_t usb_hub_address(int hub);
int usb_hub_configured(int hub);

/* Whether the host has powered port `port` (1-based), and whether it has
 * reset and enabled it -- a device behind a port only answers once it is. */
int usb_hub_port_powered(int hub, int port);
int usb_hub_port_enabled(int hub, int port);

/* Plug a device into port `port` (1-based): connection, and the change bit
 * the status endpoint will report. */
void usb_hub_plug(int hub, int port);

#endif /* XBOX_USB_HUB_H */
