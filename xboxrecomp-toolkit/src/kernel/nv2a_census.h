/*
 * GPU feature census (RECOMP_GPU_CENSUS=<file>).
 *
 * Which NV2A features a title actually uses decides which ones a renderer
 * must implement. The executor counts, per draw, the transform path,
 * primitive, render states, texture stages and formats, combiner setup,
 * surfaces and render-to-texture, and the methods it does not handle; this
 * keeps the counts and writes them as a table. Values are the registers'
 * own encodings, written in hex, so nothing about a renderer's model of
 * them is assumed.
 *
 * Counting can be confined to part of a run: with RECOMP_GPU_CENSUS_GATE
 * naming a file, counting is on only while that file exists (checked at
 * each flip), so a harness can census one scene. The table is rewritten at
 * every periodic report, so a run ended by a watchdog still leaves it.
 *
 * Executor-thread only; no locking.
 */
#ifndef NV2A_CENSUS_H
#define NV2A_CENSUS_H

#include <stdint.h>

/* 1 when counting is enabled and, if gated, the gate is open. */
int nv2a_census_active(void);

/* Re-check the gate. Called once per flip. */
void nv2a_census_flip(void);

/* Count one occurrence of key in category, with a vertex weight. The
 * category must be a string literal: entries keep the pointer. */
void nv2a_census_add(const char *category, uint32_t key, uint32_t vertices);

/* Rewrite the census file. Does nothing when disabled. */
void nv2a_census_write(void);

/* For tests: forget the switches and all counts. */
void nv2a_census_reset(void);

#endif /* NV2A_CENSUS_H */
