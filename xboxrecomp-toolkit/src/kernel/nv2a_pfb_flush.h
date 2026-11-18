#ifndef NV2A_PFB_FLUSH_H
#define NV2A_PFB_FLUSH_H

#include <stdint.h>

#define NV2A_PFB_FLUSH_BUSY_MASK 0x00010000u

typedef void (*Nv2aPfbFlushSafePoint)(void);

int nv2a_pfb_flush_wait_enabled(void);
void nv2a_pfb_flush_wait(volatile uint32_t *reg,
                         Nv2aPfbFlushSafePoint safe_point);
int nv2a_pfb_flush_ack(volatile uint32_t *reg, int flip_held);

#endif
