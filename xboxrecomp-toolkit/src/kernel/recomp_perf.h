/*
 * recomp_perf -- per-frame timing zones (RECOMP_PERF_CSV=<file>).
 *
 * Off by default: every macro below is one predictable branch on a global
 * when RECOMP_PERF_CSV is unset. When set, each zone accumulates QPC ticks
 * and a count; rpz_flip() (called at every NV2A FLIP_STALL) writes one CSV
 * line per presented frame with the per-zone milliseconds since the previous
 * flip, plus CPU time of the guest main thread and the NV2A ack thread.
 * analysis/perf/perf_summary.py turns the CSV into a breakdown.
 *
 * Zones may nest (DRAW contains DRAW_TEX); the summary reports each one
 * independently. Zones are accumulated with interlocked adds, so any
 * thread may record.
 */
#ifndef RECOMP_PERF_H
#define RECOMP_PERF_H

#include <stdint.h>

typedef enum {
    RPZ_ACK_PASS = 0,   /* nv2a ack thread: one loop pass, excluding its idle wait */
    RPZ_ACK_IDLE,       /* nv2a ack thread: idle wait / yield after a pass */
    RPZ_PB_WALK,        /* pushbuffer walk (all methods, draws included) */
    RPZ_FLIP_HELD,      /* flip hold passes (vblank pacing), counted per pass */
    RPZ_READBACK,       /* flip: back end readback of the finished frame to guest RAM */
    RPZ_PRESENT,        /* flip: hand-off of the frame to the window */
    RPZ_DRAW,           /* back end: a draw call, all of it */
    RPZ_DRAW_PIXEL,     /* back end: pixel state (combiner, shader, blend, depth) */
    RPZ_DRAW_TEX,       /* back end: textures and samplers */
    RPZ_DRAW_VERTEX,    /* back end: vertex streams / CPU gather / constants */
    RPZ_DRAW_SUBMIT,    /* back end: D3D11 Draw* and state binding */
    RPZ_GPU_WAIT,       /* back end: CPU blocked on the GPU (Map, query, keyed mutex) */
    RPZ_SHADER_COMPILE, /* back end: HLSL compilation */
    RPZ_TEX_UPLOAD,     /* back end: texture decode + upload */
    RPZ_CLEAR,          /* back end: clears */
    RPZ_FB_PRESENT,     /* window thread: present of a frame */
    RPZ_GUEST_GPU_WAIT, /* guest: waiting for the GPU (GET / fence / PFB flush) */
    RPZ_USER0, RPZ_USER1, RPZ_USER2, RPZ_USER3,
    RPZ_COUNT
} RpzZone;

extern int g_rpz_on;

uint64_t rpz_now(void);
void rpz_add(RpzZone z, uint64_t ticks);
void rpz_count(RpzZone z, uint64_t n);   /* count only */
/* Called once at startup on the guest main thread (it records the handle). */
void rpz_init_main_thread(void);
/* Called by the ack thread once at its start. */
void rpz_register_ack_thread(void);
/* One line per presented frame. draws = draws since the previous flip. */
void rpz_flip(uint32_t draws);
/* RECOMP_PERF_AB=<file>: while the file exists, optimisations whose bit is
 * in its mask (the file's text as a number, empty = all) are switched off,
 * so one session can alternate A and B (bench.py --ab). Re-read every
 * 200 ms; 0 without the variable. 1 texture verify-once, 2 device-check
 * rate limit, 4 EP liveness word. */
int rpz_ab_off(int bit);

#define RPZ_BEGIN(var) uint64_t var = g_rpz_on ? rpz_now() : 0
#define RPZ_END(zone, var) do { if (g_rpz_on && (var)) rpz_add((zone), rpz_now() - (var)); } while (0)

#endif
