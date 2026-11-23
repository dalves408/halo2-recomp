/*
 * usb_hub.c -- the hub inside an Xbox controller. See usb_hub.h.
 *
 * Runs on the OHCI controller thread only (ohci.c), so nothing is locked.
 */
#include "usb_hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- descriptors ------------------------------------------------------- */

static const uint8_t s_device_desc[18] = {
    18, 0x01,
    0x10, 0x01,     /* bcdUSB 1.10                                */
    0x09,           /* bDeviceClass: hub                          */
    0x00, 0x00,
    0x08,           /* bMaxPacketSize0: 8                         */
    0x5E, 0x04,     /* idVendor  0x045E Microsoft                 */
    0x88, 0x02,     /* idProduct 0x0288, the controller's hub     */
    0x00, 0x01,     /* bcdDevice                                  */
    0x00, 0x00, 0x00,
    0x01            /* bNumConfigurations                         */
};

static const uint8_t s_config_desc[25] = {
    /* configuration: one interface, bus powered, 100 mA */
    9, 0x02, 25, 0x00, 0x01, 0x01, 0x00, 0x80, 50,
    /* interface: hub class, no subclass or protocol */
    9, 0x04, 0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00,
    /* endpoint 0x81 IN, interrupt, 1 byte, 255 ms -- status change */
    7, 0x05, 0x81, 0x03, 0x01, 0x00, 0xFF
};

/* Hub descriptor (USB 1.1 11.15.2.1): three ports, ganged power switching,
 * 100 ms to power good, the gamepad's port marked non-removable. */
static const uint8_t s_hub_desc[9] = {
    9, 0x29,
    USB_HUB_PORTS,
    0x00, 0x00,     /* wHubCharacteristics: ganged power, ganged over-current */
    50,             /* bPwrOn2PwrGood, 2 ms units                 */
    0,              /* bHubContrCurrent                           */
    0x02,           /* DeviceRemovable: port 1 is built in        */
    0xFF            /* PortPwrCtrlMask, USB 1.1 compatibility     */
};

/* wPortStatus and wPortChange bits (USB 1.1 11.16.2.6). */
#define PS_CONNECTION   0x0001u
#define PS_ENABLE       0x0002u
#define PS_SUSPEND      0x0004u
#define PS_RESET        0x0010u
#define PS_POWER        0x0100u
#define PC_CONNECTION   0x0001u
#define PC_ENABLE       0x0002u
#define PC_SUSPEND      0x0004u
#define PC_OVERCURRENT  0x0008u
#define PC_RESET        0x0010u

/* Feature selectors. */
#define F_PORT_ENABLE        1
#define F_PORT_SUSPEND       2
#define F_PORT_RESET         4
#define F_PORT_POWER         8
#define F_C_PORT_CONNECTION  16
#define F_C_PORT_ENABLE      17
#define F_C_PORT_SUSPEND     18
#define F_C_PORT_OVERCURRENT 19
#define F_C_PORT_RESET       20

typedef struct {
    uint8_t  address;
    uint8_t  configuration;
    uint16_t status[USB_HUB_PORTS + 1];     /* 1-based */
    uint16_t change[USB_HUB_PORTS + 1];
} UsbHub;

static UsbHub s_hub[USB_HUB_MAX];

static int trace_on(void)
{
    static int t = -1;
    if (t < 0)
        t = getenv("RECOMP_USB_TRACE") != NULL;
    return t;
}

uint8_t usb_hub_address(int hub)    { return s_hub[hub & 3].address; }
int usb_hub_configured(int hub)     { return s_hub[hub & 3].configuration != 0; }

int usb_hub_port_powered(int hub, int port)
{
    return port >= 1 && port <= USB_HUB_PORTS
        && (s_hub[hub & 3].status[port] & PS_POWER) != 0;
}

int usb_hub_port_enabled(int hub, int port)
{
    return port >= 1 && port <= USB_HUB_PORTS
        && (s_hub[hub & 3].status[port] & PS_ENABLE) != 0;
}

void usb_hub_plug(int hub, int port)
{
    UsbHub *h = &s_hub[hub & 3];
    if (port < 1 || port > USB_HUB_PORTS)
        return;
    h->status[port] |= PS_CONNECTION;
    h->change[port] |= PC_CONNECTION;
}

int usb_hub_status_change(int hub, uint8_t *out, int max)
{
    UsbHub *h = &s_hub[hub & 3];
    uint8_t map = 0;
    int p;

    for (p = 1; p <= USB_HUB_PORTS; p++)
        if (h->change[p])
            map |= (uint8_t)(1u << p);
    if (!map || max < 1)
        return 0;
    out[0] = map;
    return 1;
}

static int copy_out(uint8_t *out, int max, const uint8_t *src, int len,
                    uint16_t wLength)
{
    if (len > (int)wLength) len = (int)wLength;
    if (len > max)          len = max;
    if (len > 0)            memcpy(out, src, (size_t)len);
    return len;
}

static int port_feature(UsbHub *h, int set, unsigned port, unsigned feature)
{
    uint16_t *st, *ch;

    if (port < 1 || port > USB_HUB_PORTS)
        return -1;
    st = &h->status[port];
    ch = &h->change[port];

    if (set) {
        switch (feature) {
        case F_PORT_RESET:
            /* Completes at once: a present device comes back enabled, and
             * the reset-change bit says the reset finished. */
            if (*st & PS_CONNECTION) {
                *st |= PS_ENABLE;
                *ch |= PC_RESET;
            }
            *st &= ~(PS_RESET | PS_SUSPEND);
            return 0;
        case F_PORT_POWER:
            *st |= PS_POWER;
            return 0;
        case F_PORT_SUSPEND:
            *st |= PS_SUSPEND;
            return 0;
        case F_PORT_ENABLE:
            if (*st & PS_CONNECTION)
                *st |= PS_ENABLE;
            return 0;
        default:
            return -1;
        }
    }

    switch (feature) {
    case F_PORT_ENABLE:        *st &= ~PS_ENABLE;      return 0;
    case F_PORT_SUSPEND:       *st &= ~PS_SUSPEND;     return 0;
    case F_PORT_POWER:         *st &= ~(PS_POWER | PS_ENABLE); return 0;
    case F_C_PORT_CONNECTION:  *ch &= ~PC_CONNECTION;  return 0;
    case F_C_PORT_ENABLE:      *ch &= ~PC_ENABLE;      return 0;
    case F_C_PORT_SUSPEND:     *ch &= ~PC_SUSPEND;     return 0;
    case F_C_PORT_OVERCURRENT: *ch &= ~PC_OVERCURRENT; return 0;
    case F_C_PORT_RESET:       *ch &= ~PC_RESET;       return 0;
    default:                   return -1;
    }
}

int usb_hub_control(int hub, const UsbSetup *setup, uint8_t *out, int max)
{
    UsbHub *h = &s_hub[hub & 3];
    int is_in = (setup->bmRequestType & 0x80) != 0;
    int type  = (setup->bmRequestType >> 5) & 3;     /* 0 standard, 1 class */
    int recip = setup->bmRequestType & 0x1F;         /* 0 device, 3 other  */
    int r = -1;

    if (type == 0) {
        switch (setup->bRequest) {
        case 0x06:                                   /* GET_DESCRIPTOR */
            switch (setup->wValue >> 8) {
            case 0x01:
                r = copy_out(out, max, s_device_desc, (int)sizeof s_device_desc,
                             setup->wLength);
                break;
            case 0x02:
                r = copy_out(out, max, s_config_desc, (int)sizeof s_config_desc,
                             setup->wLength);
                break;
            case 0x29:          /* some hosts ask for it as a standard one */
                r = copy_out(out, max, s_hub_desc, (int)sizeof s_hub_desc,
                             setup->wLength);
                break;
            default:
                r = -1;
            }
            break;
        case 0x05:                                   /* SET_ADDRESS */
            h->address = (uint8_t)(setup->wValue & 0x7F);
            r = 0;
            break;
        case 0x09:                                   /* SET_CONFIGURATION */
            h->configuration = (uint8_t)(setup->wValue & 0xFF);
            fprintf(stderr, "  [HUB%d] configured (%u) at address %u\n", hub,
                    h->configuration, h->address);
            fflush(stderr);
            r = 0;
            break;
        case 0x08:                                   /* GET_CONFIGURATION */
            if (is_in && max >= 1) { out[0] = h->configuration; r = 1; }
            break;
        case 0x0A:                                   /* GET_INTERFACE */
            if (is_in && max >= 1) { out[0] = 0; r = 1; }
            break;
        case 0x00:                                   /* GET_STATUS */
            if (is_in && max >= 2) {
                static const uint8_t zero[2] = { 0, 0 };   /* bus powered */
                r = copy_out(out, max, zero, 2, setup->wLength);
            }
            break;
        case 0x01: case 0x03: case 0x0B:   /* CLEAR/SET_FEATURE, SET_INTERFACE */
            r = 0;
            break;
        }
    } else if (type == 1) {
        switch (setup->bRequest) {
        case 0x06:                                   /* GET_DESCRIPTOR (hub) */
            if (is_in && (setup->wValue >> 8) == 0x29)
                r = copy_out(out, max, s_hub_desc, (int)sizeof s_hub_desc,
                             setup->wLength);
            break;
        case 0x00:                                   /* GET_STATUS */
            if (is_in && max >= 4) {
                uint8_t st[4] = { 0, 0, 0, 0 };
                if (recip == 3) {
                    unsigned p = setup->wIndex & 0xFFu;
                    if (p < 1 || p > USB_HUB_PORTS)
                        break;
                    st[0] = (uint8_t)h->status[p];
                    st[1] = (uint8_t)(h->status[p] >> 8);
                    st[2] = (uint8_t)h->change[p];
                    st[3] = (uint8_t)(h->change[p] >> 8);
                }
                r = copy_out(out, max, st, 4, setup->wLength);
            }
            break;
        case 0x01:                                   /* CLEAR_FEATURE */
        case 0x03:                                   /* SET_FEATURE */
            if (recip == 3)
                r = port_feature(h, setup->bRequest == 0x03,
                                 setup->wIndex & 0xFFu, setup->wValue);
            else
                r = 0;              /* hub-level: C_HUB_* -- nothing to clear */
            break;
        }
    }

    if (trace_on() || r < 0) {
        fprintf(stderr, "  [HUB%d] %s %02X %02X value %04X index %04X len %u%s\n",
                hub, r < 0 ? "STALL" : "SETUP", setup->bmRequestType,
                setup->bRequest, setup->wValue, setup->wIndex, setup->wLength,
                r < 0 ? " (unhandled)" : "");
        fflush(stderr);
    }
    return r;
}
