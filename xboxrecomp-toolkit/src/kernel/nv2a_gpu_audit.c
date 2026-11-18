/*
 * Bounded, opt-in GPU request accounting. The method/value accounting and
 * per-batch observer are adapted from bloodwake-recomp's
 * src/gpu/bw_gpu_audit.c, commit e631edddfe56d22344e9f696923aeb6d78a9e7ba.
 *
 * Copyright (c) 2026 the bloodwake-recomp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "nv2a_gpu_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define AUDIT_LOCK() AcquireSRWLockExclusive(&s_lock)
#define AUDIT_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define AUDIT_LOCK() pthread_mutex_lock(&s_lock)
#define AUDIT_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

#define AUDIT_METHODS 2048u
#define AUDIT_SUBCHANNELS 8u
#define AUDIT_VALUES 6u
#define AUDIT_SLOTS 4096u
#define AUDIT_LIMIT (AUDIT_SLOTS * 3u / 4u)
#define AUDIT_RECENT 8u
#define AUDIT_RECENT_AGE 256u

typedef struct {
    uint64_t count, unhandled, mirror_only, other;
    uint64_t value_count[AUDIT_VALUES];
    uint32_t values[AUDIT_VALUES];
    uint32_t nvalues;
} AuditMethod;

typedef struct {
    uint32_t category;
    uint64_t key;
    uint64_t count, vertices;
} AuditEntry;

typedef struct {
    uint32_t primitive, route, reasons, stride;
    uint32_t unsupported_state, width, height;
    uint64_t requested[NV2A_AUDIT_INPUT_COUNT];
    uint64_t buffered[NV2A_AUDIT_INPUT_COUNT];
    int active, has_unsupported_state;
} AuditBatch;

typedef struct {
    uint64_t serial;
    uint32_t subchannel, method, mirror;
} AuditRecent;

enum {
    CAT_SUMMARY = 1, CAT_ROUTE, CAT_SOURCE, CAT_PRIMITIVE, CAT_REASON,
    CAT_BACKEND, CAT_FALLBACK, CAT_VISIBILITY, CAT_CULL, CAT_INPUT,
    CAT_CLEAR_FLAGS, CAT_CLEAR_SIZE, CAT_CLEAR_RECT_SET, CAT_CLEAR_RECT_H,
    CAT_CLEAR_RECT_V, CAT_FLIP_SIZE, CAT_FLIP_FORMAT, CAT_SEMAPHORE,
    CAT_OUTPUT, CAT_REJECTION, CAT_BATCH_GEOMETRY, CAT_POST_CLIP,
    CAT_UNSUPPORTED_STATE, CAT_UNSUPPORTED_SIZE, CAT_RECENT_METHOD,
    CAT_CPU_DEPTH_BINDING
};

static AuditMethod s_methods[AUDIT_SUBCHANNELS][AUDIT_METHODS];
static AuditEntry s_entries[AUDIT_SLOTS];
static AuditBatch s_batch;
static AuditRecent s_recent[AUDIT_RECENT];
static uint64_t s_method_serial;
static uint32_t s_recent_next;
static uint32_t s_used;
static uint64_t s_table_overflow, s_value_overflow, s_saturations;
static uint64_t s_bad_methods, s_bad_unhandled, s_bad_mirror;
static int s_enabled, s_path_ok, s_warned;
static char s_path[4096];
static time_t s_last_report;

static void add_saturating(uint64_t *value, uint64_t amount)
{
    if (UINT64_MAX - *value < amount) {
        *value = UINT64_MAX;
        if (s_saturations != UINT64_MAX)
            s_saturations++;
    } else {
        *value += amount;
    }
}

static void add_entry(uint32_t category, uint64_t key, uint64_t count, uint64_t vertices)
{
    uint32_t i, n;
    if (!count && !vertices)
        return;
    i = (category * 0x9E3779B1u ^ (uint32_t)(key ^ (key >> 32)) * 0x85EBCA6Bu)
        & (AUDIT_SLOTS - 1);
    for (n = 0; n < AUDIT_SLOTS; n++, i = (i + 1) & (AUDIT_SLOTS - 1)) {
        AuditEntry *e = &s_entries[i];
        if (!e->category) {
            if (s_used >= AUDIT_LIMIT)
                break;
            e->category = category;
            e->key = key;
            s_used++;
        } else if (e->category != category || e->key != key) {
            continue;
        }
        add_saturating(&e->count, count);
        add_saturating(&e->vertices, vertices);
        return;
    }
    add_saturating(&s_table_overflow, 1);
}

static int valid_method(uint32_t subch, uint32_t method)
{
    return subch < AUDIT_SUBCHANNELS && method < 0x2000u && !(method & 3u);
}

/* Payload words are still counted, but their values are not state. */
static int bulk_method(uint32_t subch, uint32_t method)
{
    return subch == 0 && ((method >= 0x0B00 && method < 0x0C00)
        || (method >= 0x1500 && method < 0x1720)
        || (method >= 0x1800 && method < 0x1B00));
}

static void audit_initialize(void)
{
    const char *on = getenv("RECOMP_GPU_AUDIT");
    s_enabled = on && strcmp(on, "1") == 0;
    if (s_enabled) {
        const char *path = getenv("RECOMP_GPU_AUDIT_FILE");
        size_t len;
        if (!path || !*path)
            path = "gpu_audit.csv";
        len = strlen(path);
        s_path_ok = len < sizeof s_path;
        if (s_path_ok)
            memcpy(s_path, path, len + 1);
        else
            fprintf(stderr, "[GPU-AUDIT] report path is too long\n");
        s_last_report = time(NULL);
        atexit(nv2a_gpu_audit_write);
    }
}

#ifdef _WIN32
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK audit_initialize_once(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    audit_initialize();
    return TRUE;
}
#else
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
#endif

static volatile int s_audit_ready;

int nv2a_gpu_audit_enabled(void)
{
    if (s_audit_ready)      /* called per method; skip the once-gate */
        return s_enabled;
#ifdef _WIN32
    InitOnceExecuteOnce(&s_once, audit_initialize_once, NULL, NULL);
#else
    pthread_once(&s_once, audit_initialize);
#endif
    s_audit_ready = 1;
    return s_enabled;
}

void nv2a_gpu_audit_method(uint32_t subch, uint32_t method, uint32_t value)
{
    AuditMethod *m;
    uint32_t i;
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    if (++s_method_serial == 0) {
        memset(s_recent, 0, sizeof s_recent);
        s_method_serial = 1;
    }
    if (!valid_method(subch, method)) {
        add_saturating(&s_bad_methods, 1);
        AUDIT_UNLOCK();
        return;
    }
    m = &s_methods[subch][method / 4];
    add_saturating(&m->count, 1);
    if (!bulk_method(subch, method)) {
        for (i = 0; i < m->nvalues && m->values[i] != value; i++)
            ;
        if (i < m->nvalues) {
            add_saturating(&m->value_count[i], 1);
        } else if (m->nvalues < AUDIT_VALUES) {
            m->values[i] = value;
            m->value_count[i] = 1;
            m->nvalues++;
        } else {
            add_saturating(&m->other, 1);
            add_saturating(&s_value_overflow, 1);
        }
    }
    AUDIT_UNLOCK();
}

static void remember_method(uint32_t subch, uint32_t method, uint32_t mirror)
{
    AuditRecent *recent = &s_recent[s_recent_next++ % AUDIT_RECENT];
    recent->serial = s_method_serial;
    recent->subchannel = subch;
    recent->method = method;
    recent->mirror = mirror;
}

void nv2a_gpu_audit_unhandled(uint32_t subch, uint32_t method)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    if (valid_method(subch, method)) {
        add_saturating(&s_methods[subch][method / 4].unhandled, 1);
        remember_method(subch, method, 0);
    } else
        add_saturating(&s_bad_unhandled, 1);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_mirror_only(uint32_t subch, uint32_t method)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    if (valid_method(subch, method)) {
        add_saturating(&s_methods[subch][method / 4].mirror_only, 1);
        remember_method(subch, method, 1);
    } else
        add_saturating(&s_bad_mirror, 1);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_semaphore(enum Nv2aAuditSemaphore event)
{
    if (!nv2a_gpu_audit_enabled() || (unsigned)event >= NV2A_AUDIT_SEMAPHORE_COUNT)
        return;
    AUDIT_LOCK();
    add_entry(CAT_SEMAPHORE, (uint32_t)event, 1, 0);
    AUDIT_UNLOCK();
}

static void finish_batch(uint32_t culled, uint32_t drawn, uint32_t clipped,
                         uint32_t offscreen, uint32_t degenerate)
{
    uint32_t source, i, tail = 0, geometry;
    uint64_t requested, buffered;
    if (!s_batch.active)
        return;
    source = s_batch.buffered[NV2A_AUDIT_IMMEDIATE] ? NV2A_AUDIT_IMMEDIATE :
             s_batch.buffered[NV2A_AUDIT_INLINE] ? NV2A_AUDIT_INLINE :
             s_batch.requested[NV2A_AUDIT_ARRAY] ? NV2A_AUDIT_ARRAY :
             NV2A_AUDIT_INPUT_COUNT;
    requested = source < NV2A_AUDIT_INPUT_COUNT ? s_batch.requested[source] : 0;
    buffered = source < NV2A_AUDIT_INPUT_COUNT ? s_batch.buffered[source] : 0;
    if (source == NV2A_AUDIT_INLINE) {
        uint32_t words = s_batch.stride / 4;
        requested = words ? requested / words : 0;
        buffered = words ? buffered / words : 0;
    }
    if (s_batch.route == NV2A_AUDIT_DROPPED && !s_batch.reasons)
        s_batch.reasons = 1u << (requested ? NV2A_AUDIT_NO_EXECUTION : NV2A_AUDIT_NO_VERTICES);
    if (s_batch.primitive == 5)
        tail = (uint32_t)(buffered % 3);
    else if (s_batch.primitive == 8)
        tail = (uint32_t)(buffered % 4);
    else if (s_batch.primitive == 9 && buffered >= 4)
        tail = (uint32_t)((buffered - 2) % 2);
    if (tail)
        s_batch.reasons |= 1u << NV2A_AUDIT_TRAILING_VERTICES;

    add_entry(CAT_SUMMARY, 0, 1, requested);
    add_entry(CAT_SUMMARY, 1, buffered, 0);
    add_entry(CAT_ROUTE, s_batch.route, 1, requested);
    add_entry(CAT_SOURCE, source, 1, requested);
    add_entry(CAT_PRIMITIVE, s_batch.primitive, 1, requested);
    for (i = 0; i < NV2A_AUDIT_REASON_COUNT; i++)
        if (s_batch.reasons & (1u << i))
            add_entry(CAT_REASON, i, 1, requested);
    add_entry(CAT_VISIBILITY, s_batch.route == NV2A_AUDIT_DROPPED ? 1 : 0, 1, requested);
    add_entry(CAT_OUTPUT, drawn ? 0 : 1, 1, requested);
    if (s_batch.route == NV2A_AUDIT_DROPPED)
        geometry = 0; /* Not submitted; visibility is unavailable. */
    else if (s_batch.route == NV2A_AUDIT_GPU_VERTEX)
        geometry = 1; /* Submission precedes GPU clipping. */
    else if (s_batch.route == NV2A_AUDIT_SCREEN_SPACE)
        geometry = drawn ? 2 : 3;
    else
        geometry = drawn ? 4 : 5;
    add_entry(CAT_BATCH_GEOMETRY, (uint64_t)s_batch.primitive << 32 |
              (uint64_t)source << 16 | (uint64_t)s_batch.route << 8 | geometry, 1, requested);
    if ((s_batch.route == NV2A_AUDIT_CPU_PROGRAM ||
         s_batch.route == NV2A_AUDIT_FIXED_FUNCTION) && drawn)
        add_entry(CAT_POST_CLIP, s_batch.route, drawn, 0);
    if (s_batch.has_unsupported_state) {
        add_entry(CAT_UNSUPPORTED_STATE, (uint64_t)s_batch.primitive << 32 |
                  s_batch.unsupported_state, 1, requested);
        add_entry(CAT_UNSUPPORTED_SIZE, (uint64_t)s_batch.width << 32 |
                  s_batch.height, 1, requested);
    }
    if (s_batch.route == NV2A_AUDIT_DROPPED) {
        for (i = 0; i < AUDIT_RECENT; i++) {
            const AuditRecent *recent = &s_recent[i];
            uint32_t j;
            if (!recent->serial || recent->serial > s_method_serial ||
                s_method_serial - recent->serial > AUDIT_RECENT_AGE)
                continue;
            for (j = 0; j < i; j++)
                if (s_recent[j].serial && s_recent[j].serial <= s_method_serial &&
                    s_method_serial - s_recent[j].serial <= AUDIT_RECENT_AGE &&
                    s_recent[j].subchannel == recent->subchannel &&
                    s_recent[j].method == recent->method &&
                    s_recent[j].mirror == recent->mirror)
                    break;
            if (j != i)
                continue;
            add_entry(CAT_RECENT_METHOD, (uint64_t)s_batch.primitive << 32 |
                      (uint64_t)recent->mirror << 31 |
                      (uint64_t)recent->subchannel << 16 | recent->method, 1, requested);
        }
    }
    if (culled) {
        add_entry(CAT_CULL, 0, culled, 0);
        add_entry(CAT_CULL, 1, 1, requested);
        if (!drawn)
            add_entry(CAT_CULL, 2, 1, requested);
    }
    add_entry(CAT_CULL, 3, clipped, 0);
    add_entry(CAT_CULL, 4, offscreen, 0);
    add_entry(CAT_CULL, 5, degenerate, 0);
    for (i = 0; i < NV2A_AUDIT_INPUT_COUNT; i++) {
        add_entry(CAT_INPUT, i * 3, s_batch.requested[i], 0);
        add_entry(CAT_INPUT, i * 3 + 1, s_batch.buffered[i], 0);
        if (s_batch.requested[i] > s_batch.buffered[i])
            add_entry(CAT_INPUT, i * 3 + 2,
                      s_batch.requested[i] - s_batch.buffered[i], 0);
    }
    add_entry(CAT_INPUT, 9, tail, 0);
    memset(&s_batch, 0, sizeof s_batch);
}

void nv2a_gpu_audit_begin(uint32_t primitive)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    if (s_batch.active) {
        s_batch.route = NV2A_AUDIT_DROPPED;
        s_batch.reasons |= 1u << NV2A_AUDIT_SUPERSEDED;
        finish_batch(0, 0, 0, 0, 0);
    }
    s_batch.active = 1;
    s_batch.primitive = primitive;
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_input(enum Nv2aAuditInput source, uint64_t requested,
                          uint64_t buffered)
{
    if (!nv2a_gpu_audit_enabled() || (unsigned)source >= NV2A_AUDIT_INPUT_COUNT)
        return;
    AUDIT_LOCK();
    if (s_batch.active) {
        add_saturating(&s_batch.requested[source], requested);
        add_saturating(&s_batch.buffered[source], buffered);
    }
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_inline_stride(uint32_t bytes)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    if (s_batch.active)
        s_batch.stride = bytes;
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_route(enum Nv2aAuditRoute route)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    if (s_batch.active && !s_batch.reasons)
        s_batch.route = (uint32_t)route;
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_drop(enum Nv2aAuditReason reason)
{
    if (!nv2a_gpu_audit_enabled() || (unsigned)reason >= NV2A_AUDIT_REASON_COUNT)
        return;
    AUDIT_LOCK();
    if (s_batch.active) {
        s_batch.route = NV2A_AUDIT_DROPPED;
        s_batch.reasons |= 1u << reason;
    }
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_backend(enum Nv2aAuditBackend event, uint32_t vertices)
{
    if (!nv2a_gpu_audit_enabled() || (unsigned)event >= NV2A_AUDIT_BACKEND_COUNT)
        return;
    AUDIT_LOCK();
    add_entry(CAT_BACKEND, (uint32_t)event, 1, vertices);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_fallback(enum Nv2aAuditFallback reason)
{
    if (!nv2a_gpu_audit_enabled() || (unsigned)reason >= NV2A_AUDIT_FALLBACK_COUNT)
        return;
    AUDIT_LOCK();
    add_entry(CAT_FALLBACK, (uint32_t)reason, 1, 0);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_rejection(enum Nv2aAuditRejection reason)
{
    if (!nv2a_gpu_audit_enabled() || (unsigned)reason >= NV2A_AUDIT_REJECTION_COUNT)
        return;
    AUDIT_LOCK();
    add_entry(CAT_REJECTION, (uint32_t)reason, 1, 0);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_unsupported_state(uint32_t state, uint32_t width, uint32_t height)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    if (s_batch.active) {
        s_batch.unsupported_state = state;
        s_batch.width = width;
        s_batch.height = height;
        s_batch.has_unsupported_state = 1;
    }
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_cpu_depth_binding(uint32_t stage, uint32_t format, int exact_zeta)
{
    if (!nv2a_gpu_audit_enabled() || stage >= 4 ||
        (format != 0x2Eu && format != 0x41u))
        return;
    AUDIT_LOCK();
    add_entry(CAT_CPU_DEPTH_BINDING, (uint64_t)stage << 16 |
              format << 8 | (exact_zeta != 0), 1, 0);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_end(uint32_t culled, uint32_t drawn, uint32_t clipped,
                        uint32_t offscreen, uint32_t degenerate)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    finish_batch(culled, drawn, clipped, offscreen, degenerate);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_clear(uint32_t flags, uint32_t width, uint32_t height,
                          uint32_t rect_set, uint32_t rect_h, uint32_t rect_v)
{
    if (!nv2a_gpu_audit_enabled())
        return;
    AUDIT_LOCK();
    add_entry(CAT_CLEAR_FLAGS, flags, 1, 0);
    add_entry(CAT_CLEAR_SIZE, (uint64_t)width << 32 | height, 1, 0);
    add_entry(CAT_CLEAR_RECT_SET, rect_set, 1, 0);
    if (rect_set & 1)
        add_entry(CAT_CLEAR_RECT_H, rect_h, 1, 0);
    if (rect_set & 2)
        add_entry(CAT_CLEAR_RECT_V, rect_v, 1, 0);
    AUDIT_UNLOCK();
}

void nv2a_gpu_audit_flip(uint32_t width, uint32_t height, uint32_t format)
{
    time_t now;
    int report = 0;
    if (!nv2a_gpu_audit_enabled())
        return;
    now = time(NULL);
    AUDIT_LOCK();
    add_entry(CAT_FLIP_SIZE, (uint64_t)width << 32 | height, 1, 0);
    add_entry(CAT_FLIP_FORMAT, format, 1, 0);
    memset(s_recent, 0, sizeof s_recent);
    if (now != (time_t)-1 && (s_last_report == (time_t)-1 ||
        now < s_last_report || now - s_last_report >= 20)) {
        s_last_report = now;
        report = 1;
    }
    AUDIT_UNLOCK();
    if (report)
        nv2a_gpu_audit_write();
}

static const char *category_name(uint32_t category)
{
    static const char *const names[] = {
        "", "summary", "route", "source", "primitive", "reason",
        "backend", "fallback", "visibility", "cull", "input",
        "clear_flags", "clear_size", "clear_rect_set", "clear_rect_h",
        "clear_rect_v", "flip_size", "flip_current_format", "semaphore",
        "output", "rejection", "batch_geometry", "post_clip_triangles",
        "unsupported_state", "unsupported_size", "recent_method_for_drop",
        "cpu_depth_binding"
    };
    return category < sizeof names / sizeof names[0] ? names[category] : "invalid";
}

static const char *key_name(uint32_t category, uint64_t key)
{
    static const char *const summary[] = {"REQUESTS", "BUFFERED_VERTICES"};
    static const char *const route[] = {"DROPPED", "GPU_VERTEX", "CPU_PROGRAM",
                                       "FIXED_FUNCTION", "SCREEN_SPACE"};
    static const char *const source[] = {"ARRAY", "INLINE", "IMMEDIATE", "NONE"};
    static const char *const reason[] = {
        "NO_VERTICES", "SHORT_BATCH", "INLINE_LAYOUT", "UNSUPPORTED_PRIMITIVE",
        "UNTRANSFORMED", "PROBE_DROP", "ALLOCATION_FAILED", "TRANSFORM_FAILED",
        "SUPERSEDED", "NO_EXECUTION", "TRAILING_VERTICES"
    };
    static const char *const backend[] = {"BASE_SUBMITTED", "EXTENDED_SUBMITTED",
        "GPU_VERTEX_SUBMITTED", "GPU_VERTEX_REJECTED", "BASE_STAGING_DROP",
        "EXTENDED_STAGING_DROP"};
    static const char *const fallback[] = {"VSH_UNAVAILABLE", "VSH_PREPARATION", "VSH_REJECTED"};
    static const char *const visibility[] = {"UNKNOWN", "NOT_SUBMITTED"};
    static const char *const cull[] = {"SOFTWARE_TRIANGLES", "BATCHES_WITH_CULL",
        "CULL_WITH_NO_TRIANGLES_EMITTED", "CLIPPED_OR_BEHIND", "OFFSCREEN_TRIANGLES",
        "DEGENERATE_TRIANGLES"};
    static const char *const input[] = {
        "ARRAY_REQUESTED", "ARRAY_BUFFERED", "ARRAY_DISCARDED",
        "INLINE_WORDS_REQUESTED", "INLINE_WORDS_BUFFERED", "INLINE_WORDS_DISCARDED",
        "IMMEDIATE_REQUESTED", "IMMEDIATE_BUFFERED", "IMMEDIATE_DISCARDED",
        "TRAILING_VERTICES"
    };
    static const char *const semaphore[] = {"DIRECT_RELEASE", "NO_TARGET"};
    static const char *const output[] = {"TRIANGLES_EMITTED", "NO_TRIANGLES_EMITTED"};
    static const char *const rejection[] = {"INVALID_SURFACE_FORMAT", "POSITION_FETCH_FAILED",
        "SURFACE_REFUSED", "SCREEN_DEGENERATE", "NONFINITE_CLIP"};
#define AUDIT_KEY(cat, array) \
    if (category == (cat) && key < sizeof(array) / sizeof((array)[0])) return (array)[key]
    AUDIT_KEY(CAT_SUMMARY, summary);
    AUDIT_KEY(CAT_ROUTE, route);
    AUDIT_KEY(CAT_SOURCE, source);
    AUDIT_KEY(CAT_REASON, reason);
    AUDIT_KEY(CAT_BACKEND, backend);
    AUDIT_KEY(CAT_FALLBACK, fallback);
    AUDIT_KEY(CAT_VISIBILITY, visibility);
    AUDIT_KEY(CAT_CULL, cull);
    AUDIT_KEY(CAT_INPUT, input);
    AUDIT_KEY(CAT_SEMAPHORE, semaphore);
    AUDIT_KEY(CAT_OUTPUT, output);
    AUDIT_KEY(CAT_REJECTION, rejection);
    AUDIT_KEY(CAT_POST_CLIP, route);
#undef AUDIT_KEY
    return NULL;
}

static void write_count(FILE *f, const char *category, const char *key, uint64_t count)
{
    fprintf(f, "%s,%s,%llu,0\n", category, key, (unsigned long long)count);
}

void nv2a_gpu_audit_write(void)
{
    FILE *f;
    uint32_t i, s;
    if (!nv2a_gpu_audit_enabled() || !s_path_ok)
        return;
    AUDIT_LOCK();
    f = fopen(s_path, "w");
    if (!f) {
        if (!s_warned) {
            fprintf(stderr, "[GPU-AUDIT] cannot write report: %s\n", s_path);
            s_warned = 1;
        }
        AUDIT_UNLOCK();
        return;
    }
    fprintf(f, "# GPU audit: submissions are not visibility; active_begin=%d\n", s_batch.active);
    fprintf(f, "category,key,count,vertices\n");
    for (i = 0; i < AUDIT_SLOTS; i++) {
        const AuditEntry *e = &s_entries[i];
        const char *name;
        if (!e->category)
            continue;
        name = key_name(e->category, e->key);
        if (name)
            fprintf(f, "%s,%s,%llu,%llu\n", category_name(e->category), name,
                    (unsigned long long)e->count, (unsigned long long)e->vertices);
        else if (e->category == CAT_CLEAR_SIZE || e->category == CAT_FLIP_SIZE ||
                 e->category == CAT_UNSUPPORTED_SIZE)
            fprintf(f, "%s,%u:%u,%llu,%llu\n", category_name(e->category),
                    (uint32_t)(e->key >> 32), (uint32_t)e->key,
                    (unsigned long long)e->count, (unsigned long long)e->vertices);
        else if (e->category == CAT_BATCH_GEOMETRY) {
            static const char *const source[] = {"ARRAY", "INLINE", "IMMEDIATE", "NONE"};
            static const char *const route[] = {"DROPPED", "GPU_VERTEX", "CPU_PROGRAM",
                                                "FIXED_FUNCTION", "SCREEN_SPACE"};
            static const char *const geometry[] = {"NOT_SUBMITTED", "GPU_PRECLIP_UNKNOWN",
                "SCREEN_OUTPUT", "SCREEN_NO_OUTPUT", "CPU_POSTCLIP_OUTPUT", "CPU_POSTCLIP_NONE"};
            uint32_t src = (uint32_t)(e->key >> 16) & 0xFF;
            uint32_t r = (uint32_t)(e->key >> 8) & 0xFF, g = (uint32_t)e->key & 0xFF;
            fprintf(f, "%s,%08X:%s:%s:%s,%llu,%llu\n", category_name(e->category),
                    (uint32_t)(e->key >> 32), src < 4 ? source[src] : "INVALID",
                    r < 5 ? route[r] : "INVALID", g < 6 ? geometry[g] : "INVALID",
                    (unsigned long long)e->count, (unsigned long long)e->vertices);
        } else if (e->category == CAT_UNSUPPORTED_STATE)
            fprintf(f, "%s,%08X:%08X,%llu,%llu\n", category_name(e->category),
                    (uint32_t)(e->key >> 32), (uint32_t)e->key,
                    (unsigned long long)e->count, (unsigned long long)e->vertices);
        else if (e->category == CAT_RECENT_METHOD)
            fprintf(f, "%s,%08X:%s:%u:%04X,%llu,%llu\n", category_name(e->category),
                    (uint32_t)(e->key >> 32), (e->key & 0x80000000u) ? "MIRROR" : "UNHANDLED",
                    (uint32_t)(e->key >> 16) & 0x7FFF, (uint32_t)e->key & 0xFFFF,
                    (unsigned long long)e->count, (unsigned long long)e->vertices);
        else if (e->category == CAT_CPU_DEPTH_BINDING)
            fprintf(f, "%s,%u:%02X:%s,%llu,%llu\n", category_name(e->category),
                    (uint32_t)(e->key >> 16), ((uint32_t)e->key >> 8) & 0xFF,
                    (e->key & 1) ? "EXACT_CURRENT_ZETA" : "OTHER_OR_UNKNOWN",
                    (unsigned long long)e->count, (unsigned long long)e->vertices);
        else
            fprintf(f, "%s,%08X,%llu,%llu\n", category_name(e->category), (uint32_t)e->key,
                    (unsigned long long)e->count, (unsigned long long)e->vertices);
    }
    if (s_batch.active) {
        fprintf(f, "open_request,%08X,1,0\n", s_batch.primitive);
        for (i = 0; i < NV2A_AUDIT_INPUT_COUNT; i++) {
            static const char *const open_input[] = {"ARRAY", "INLINE_WORDS", "IMMEDIATE"};
            fprintf(f, "open_input,%s,%llu,%llu\n", open_input[i],
                    (unsigned long long)s_batch.requested[i],
                    (unsigned long long)s_batch.buffered[i]);
        }
    }
    for (s = 0; s < AUDIT_SUBCHANNELS; s++) {
        for (i = 0; i < AUDIT_METHODS; i++) {
            const AuditMethod *m = &s_methods[s][i];
            uint32_t v;
            if (!m->count && !m->unhandled && !m->mirror_only)
                continue;
            fprintf(f, "method_seen,%u:%04X,%llu,0\n", s, i * 4,
                    (unsigned long long)m->count);
            if (m->unhandled)
                fprintf(f, "method_unhandled,%u:%04X,%llu,0\n", s, i * 4,
                        (unsigned long long)m->unhandled);
            if (m->mirror_only)
                fprintf(f, "method_mirror_only,%u:%04X,%llu,0\n", s, i * 4,
                        (unsigned long long)m->mirror_only);
            if (bulk_method(s, i * 4))
                fprintf(f, "method_bulk_values_omitted,%u:%04X,%llu,0\n", s, i * 4,
                        (unsigned long long)m->count);
            for (v = 0; v < m->nvalues; v++)
                fprintf(f, "method_value,%u:%04X=%08X,%llu,0\n", s, i * 4, m->values[v],
                        (unsigned long long)m->value_count[v]);
            if (m->other)
                fprintf(f, "method_value_other,%u:%04X,%llu,0\n", s, i * 4,
                        (unsigned long long)m->other);
        }
    }
    write_count(f, "overflow", "TABLE_EVENTS", s_table_overflow);
    write_count(f, "overflow", "METHOD_VALUE_OTHER", s_value_overflow);
    write_count(f, "overflow", "COUNTER_SATURATIONS", s_saturations);
    write_count(f, "overflow", "INVALID_METHODS", s_bad_methods);
    write_count(f, "overflow", "INVALID_UNHANDLED", s_bad_unhandled);
    write_count(f, "overflow", "INVALID_MIRROR_ONLY", s_bad_mirror);
    if (fclose(f) != 0 && !s_warned) {
        fprintf(stderr, "[GPU-AUDIT] cannot finish report: %s\n", s_path);
        s_warned = 1;
    }
    AUDIT_UNLOCK();
}
