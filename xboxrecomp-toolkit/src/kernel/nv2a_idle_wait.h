#ifndef NV2A_IDLE_WAIT_H
#define NV2A_IDLE_WAIT_H

int nv2a_idle_wait_enabled(void);
void nv2a_idle_wake(void);
void nv2a_idle_wait_after_pass(int did_work);

#endif
