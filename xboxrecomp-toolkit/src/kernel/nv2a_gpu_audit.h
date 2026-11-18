/*
 * Opt-in executor observer. Adapted from the generic accounting design in
 * bloodwake-recomp src/gpu/bw_gpu_audit.{c,h}, commit
 * e631edddfe56d22344e9f696923aeb6d78a9e7ba.
 *
 * Copyright (c) 2026 the bloodwake-recomp contributors
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md in the
 * consuming project.
 *
 * RECOMP_GPU_AUDIT=1 enables this observer on the executor thread, regardless
 * of whether a rendering backend is registered. RECOMP_GPU_AUDIT_FILE names
 * the CSV report (default gpu_audit.csv). Submissions are not visibility.
 */
#ifndef NV2A_GPU_AUDIT_H
#define NV2A_GPU_AUDIT_H

#include <stdint.h>

enum Nv2aAuditInput {
    NV2A_AUDIT_ARRAY,
    NV2A_AUDIT_INLINE,
    NV2A_AUDIT_IMMEDIATE,
    NV2A_AUDIT_INPUT_COUNT
};

enum Nv2aAuditRoute {
    NV2A_AUDIT_DROPPED,
    NV2A_AUDIT_GPU_VERTEX,
    NV2A_AUDIT_CPU_PROGRAM,
    NV2A_AUDIT_FIXED_FUNCTION,
    NV2A_AUDIT_SCREEN_SPACE
};

enum Nv2aAuditReason {
    NV2A_AUDIT_NO_VERTICES,
    NV2A_AUDIT_SHORT_BATCH,
    NV2A_AUDIT_INLINE_LAYOUT,
    NV2A_AUDIT_UNSUPPORTED_PRIMITIVE,
    NV2A_AUDIT_UNTRANSFORMED,
    NV2A_AUDIT_PROBE_DROP,
    NV2A_AUDIT_ALLOCATION_FAILED,
    NV2A_AUDIT_TRANSFORM_FAILED,
    NV2A_AUDIT_SUPERSEDED,
    NV2A_AUDIT_NO_EXECUTION,
    NV2A_AUDIT_TRAILING_VERTICES,
    NV2A_AUDIT_REASON_COUNT
};

enum Nv2aAuditBackend {
    NV2A_AUDIT_BASE_SUBMITTED,
    NV2A_AUDIT_EXTENDED_SUBMITTED,
    NV2A_AUDIT_GPU_VERTEX_SUBMITTED,
    NV2A_AUDIT_GPU_VERTEX_REJECTED,
    NV2A_AUDIT_BASE_STAGING_DROP,
    NV2A_AUDIT_EXTENDED_STAGING_DROP,
    NV2A_AUDIT_BACKEND_COUNT
};

enum Nv2aAuditFallback {
    NV2A_AUDIT_VSH_UNAVAILABLE,
    NV2A_AUDIT_VSH_PREPARATION,
    NV2A_AUDIT_VSH_REJECTED,
    NV2A_AUDIT_FALLBACK_COUNT
};

enum Nv2aAuditRejection {
    NV2A_AUDIT_INVALID_SURFACE_FORMAT,
    NV2A_AUDIT_POSITION_FETCH_FAILED,
    NV2A_AUDIT_SURFACE_REFUSED,
    NV2A_AUDIT_SCREEN_DEGENERATE,
    NV2A_AUDIT_NONFINITE_CLIP,
    NV2A_AUDIT_REJECTION_COUNT
};

enum Nv2aAuditSemaphore {
    NV2A_AUDIT_SEMAPHORE_DIRECT,
    NV2A_AUDIT_SEMAPHORE_NO_TARGET,
    NV2A_AUDIT_SEMAPHORE_COUNT
};

int nv2a_gpu_audit_enabled(void);
void nv2a_gpu_audit_method(uint32_t subch, uint32_t method, uint32_t value);
void nv2a_gpu_audit_unhandled(uint32_t subch, uint32_t method);
void nv2a_gpu_audit_mirror_only(uint32_t subch, uint32_t method);
void nv2a_gpu_audit_semaphore(enum Nv2aAuditSemaphore event);

void nv2a_gpu_audit_begin(uint32_t primitive);
void nv2a_gpu_audit_input(enum Nv2aAuditInput source, uint64_t requested,
                          uint64_t buffered);
void nv2a_gpu_audit_inline_stride(uint32_t bytes);
void nv2a_gpu_audit_route(enum Nv2aAuditRoute route);
void nv2a_gpu_audit_drop(enum Nv2aAuditReason reason);
void nv2a_gpu_audit_backend(enum Nv2aAuditBackend event, uint32_t vertices);
void nv2a_gpu_audit_fallback(enum Nv2aAuditFallback reason);
void nv2a_gpu_audit_rejection(enum Nv2aAuditRejection reason);
void nv2a_gpu_audit_unsupported_state(uint32_t state, uint32_t width, uint32_t height);
void nv2a_gpu_audit_cpu_depth_binding(uint32_t stage, uint32_t format, int exact_zeta);
void nv2a_gpu_audit_end(uint32_t culled, uint32_t drawn, uint32_t clipped,
                        uint32_t offscreen, uint32_t degenerate);

void nv2a_gpu_audit_clear(uint32_t flags, uint32_t width, uint32_t height,
                          uint32_t rect_set, uint32_t rect_h, uint32_t rect_v);
void nv2a_gpu_audit_flip(uint32_t width, uint32_t height, uint32_t format);
void nv2a_gpu_audit_write(void);

#endif
