#ifndef NV2A_SEGMENTS_H
#define NV2A_SEGMENTS_H

#include <stdint.h>

typedef void (*Nv2aSegmentSafePoint)(void);

int nv2a_segments_enabled(void);
void nv2a_segments_bind(volatile uint32_t *put, volatile uint32_t *get);
void nv2a_segments_publish(uint32_t put, uint32_t caller, Nv2aSegmentSafePoint safe_point);
int nv2a_segments_peek(uint32_t *from, uint32_t *put, uint32_t *caller, int *first);
void nv2a_segments_complete(uint32_t put);
int nv2a_segments_pending(void);
int nv2a_segments_pfb_blocked(int graph_trap_pending);
int nv2a_segments_service(int flip_held, void (*resync)(uint32_t),
                          void (*walk)(uint32_t));
void nv2a_segments_shutdown(void);

#endif
