/*
 * usb_xblc.h -- an Xbox Live Communicator, the console's voice headset.
 *
 * A USB 1.1 audio device with two interfaces of Microsoft's class 0x78: a
 * speaker on isochronous OUT endpoint 0x04 and a microphone on isochronous IN
 * endpoint 0x85, 48 bytes a frame each, 16-bit mono PCM at a rate the host
 * picks with a vendor request (8000, 11025, 16000, 22050 or 24000 Hz).
 *
 * The title's own XAPI drives it through the OHCI model and its XHV library
 * does the voice chat, exactly as on a console; this file only has to be the
 * device. The microphone is the PC's default recording device. What the
 * console sends to the earpiece is counted and dropped for now: on hardware
 * it comes out of a DSP effect the APU model does not run, so it is silence,
 * and the title already plays voice through the speakers.
 *
 * Written from the device's descriptors as captured on the Xbox Dev Wiki and
 * from xemu's hw/xbox/xblc.c, which emulates the same device.
 */
#ifndef XBOX_USB_XBLC_H
#define XBOX_USB_XBLC_H

#include <stdint.h>
#include "usb_gamepad.h"            /* UsbSetup */

/* Answer a control request. Same contract as usb_gamepad_control: bytes
 * written, or -1 for a stall. */
int usb_xblc_control(const UsbSetup *setup, uint8_t *out, int max);

/* The address the host gave the headset, 0 until SET_ADDRESS, and whether it
 * has been configured. */
uint8_t usb_xblc_address(void);
int usb_xblc_configured(void);

/* One frame of microphone, for an isochronous IN packet of at most `max`
 * bytes. Returns the byte count written: one millisecond of samples at the
 * current rate, silence when the host microphone has nothing. */
int usb_xblc_mic_frame(uint8_t *out, int max);

/* One frame for the earpiece, from an isochronous OUT packet. */
void usb_xblc_speaker_frame(const uint8_t *data, int len);

/* Open the host microphone. Called when the headset is plugged in; the
 * microphone is never opened otherwise. */
void usb_xblc_start(void);

/* Counters for RECOMP_USB_STATS. */
void usb_xblc_stats(unsigned long *mic_frames, unsigned long *mic_underruns,
                    unsigned long *speaker_frames, unsigned *rate);

#endif /* XBOX_USB_XBLC_H */
