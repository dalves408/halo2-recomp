/* Test clips (RECOMP_CLIP=1): Delete saves the last 5 s and the next 1 s of frames
 * with per-frame notes to data\clips. See recomp_clip.c. */
#ifndef RECOMP_CLIP_H
#define RECOMP_CLIP_H
#include <stdint.h>

/* One line the title fills each frame (what it did that frame); copied with each flip. */
extern char g_rclip_note[512];
/* Set by the vblank schedule while it follows the screen's refresh. */
extern volatile int g_rclip_vblank_locked;

/* Each presented frame: the frame's pixels in guest memory and the title's update counter. */
void rclip_on_flip(const uint8_t *mem, uint32_t base, uint32_t pitch, uint32_t cx, uint32_t cy,
                   uint32_t w, uint32_t h, uint32_t bpp, uint32_t flip, uint32_t tick);
#endif
