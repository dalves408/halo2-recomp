#include "nv2a_segments.h"
#include "nv2a_idle_wait.h"
#include "platform/xbox_winnt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NV2A_SEGMENT_QUEUE_CAPACITY
#define NV2A_SEGMENT_QUEUE_CAPACITY 128u
#endif
#ifndef NV2A_SEGMENT_WAIT_MS
#define NV2A_SEGMENT_WAIT_MS 5000u
#endif
#define NV2A_SEGMENT_RAM_BYTES (64u * 1024u * 1024u)
#define NV2A_SEGMENT_CALLERS 32u

typedef struct { uint32_t put, caller; } Segment;
typedef struct { uint32_t caller; unsigned long long published, completed; } Caller;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_lock;
static HANDLE s_space;
static int s_enabled, s_bound, s_first;
static volatile uint32_t *s_put, *s_get;
static uint32_t s_last_put, s_last_get;
static Segment s_queue[NV2A_SEGMENT_QUEUE_CAPACITY];
static unsigned s_head, s_count, s_high_water;
static volatile LONG s_outstanding;
static unsigned long long s_waits, s_published, s_completed;
static Caller s_callers[NV2A_SEGMENT_CALLERS];
static unsigned long long s_caller_overflow;
static DWORD s_report_ms;

static void fail(const char *reason)
{
    fprintf(stderr, "[NV2A-SEGMENT] %s\n", reason);
    fflush(stderr);
    abort();
}

static int valid_put(uint32_t put)
{
    return !(put & 3u) && put < NV2A_SEGMENT_RAM_BYTES;
}

static BOOL CALLBACK initialize(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    const char *value = getenv("RECOMP_NV2A_SEGMENTS");
    (void)once; (void)parameter; (void)context;
    if (!value || !*value || strcmp(value, "0") == 0)
        return TRUE;
    if (strcmp(value, "1") != 0)
        fail("RECOMP_NV2A_SEGMENTS must be 0 or 1");
    if (!getenv("RECOMP_PB_EXEC") || !getenv("RECOMP_PB_WALK") ||
        strcmp(getenv("RECOMP_PB_WALK"), "staged") != 0 ||
        !getenv("RECOMP_PGRAPH_IRQ") || strcmp(getenv("RECOMP_PGRAPH_IRQ"), "1") != 0)
        fail("requires RECOMP_PB_EXEC, RECOMP_PB_WALK=staged and RECOMP_PGRAPH_IRQ=1");
    InitializeCriticalSection(&s_lock);
    s_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_space)
        fail("cannot create queue-space event");
    s_enabled = 1;
    fprintf(stderr, "[NV2A-SEGMENT] guarded per-PUT queue enabled"
                    " (capacity %u, wait limit %u ms)\n",
            (unsigned)NV2A_SEGMENT_QUEUE_CAPACITY, (unsigned)NV2A_SEGMENT_WAIT_MS);
    return TRUE;
}

int nv2a_segments_enabled(void)
{
    if (!InitOnceExecuteOnce(&s_once, initialize, NULL, NULL))
        fail("cannot initialize");
    return s_enabled;
}

static void check_registers(void)
{
    if (!s_bound)
        fail("USER registers are not bound or are shutting down");
    if ((uint32_t)InterlockedCompareExchange((volatile LONG *)s_put, 0, 0) != s_last_put)
        fail("untracked PUT write or reset");
    if ((uint32_t)InterlockedCompareExchange((volatile LONG *)s_get, 0, 0) != s_last_get)
        fail("untracked GET write or reset");
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static DWORD nv2a_segment_wait_once(HANDLE event)
{
    if (event) {
        /* Retain this frame after the syscall for profiler attribution. */
        volatile DWORD result = WaitForSingleObject(event, 1);
        return result;
    }
    Sleep(1);
    return WAIT_TIMEOUT;
}

static void lock_queue(Nv2aSegmentSafePoint safe_point, DWORD started)
{
    while (!TryEnterCriticalSection(&s_lock)) {
        if ((DWORD)(GetTickCount() - started) >= NV2A_SEGMENT_WAIT_MS)
            fail("queue lock wait timed out");
        if (safe_point) safe_point();
        nv2a_segment_wait_once(NULL);
    }
}

static Caller *caller_slot(uint32_t caller)
{
    unsigned i;
    if (!caller)
        caller = 0xFFFFFFFFu;
    for (i = 0; i < NV2A_SEGMENT_CALLERS; i++)
        if (s_callers[i].caller == caller || !s_callers[i].caller) {
            s_callers[i].caller = caller;
            return &s_callers[i];
        }
    s_caller_overflow++;
    return NULL;
}

static void report(void)
{
    DWORD now = GetTickCount();
    unsigned i;
    if (!s_report_ms) {
        s_report_ms = now ? now : 1;
        return;
    }
    if ((DWORD)(now - s_report_ms) < 10000u)
        return;
    s_report_ms = now;
    fprintf(stderr, "[NV2A-SEGMENT] published=%llu completed=%llu pending=%u"
                    " high_water=%u queue_waits=%llu caller_overflow=%llu\n",
            s_published, s_completed, s_count, s_high_water, s_waits, s_caller_overflow);
    for (i = 0; i < NV2A_SEGMENT_CALLERS; i++)
        if (s_callers[i].caller)
            fprintf(stderr, "[NV2A-SEGMENT-CALLER] caller=%08X published=%llu completed=%llu\n",
                    s_callers[i].caller, s_callers[i].published, s_callers[i].completed);
    fflush(stderr);
}

void nv2a_segments_bind(volatile uint32_t *put, volatile uint32_t *get)
{
    if (!nv2a_segments_enabled() || !put || !get)
        fail("cannot bind disabled or missing USER registers");
    lock_queue(NULL, GetTickCount());
    if (s_bound || s_count)
        fail("cannot bind an active queue");
    s_put = put;
    s_get = get;
    s_last_put = *put;
    s_last_get = *get;
    if (!valid_put(s_last_put) || !valid_put(s_last_get))
        fail("initial PUT or GET is outside aligned physical RAM");
    s_first = 1;
    s_bound = 1;
    LeaveCriticalSection(&s_lock);
}

void nv2a_segments_publish(uint32_t put, uint32_t caller, Nv2aSegmentSafePoint safe_point)
{
    DWORD started = GetTickCount();
    int counted_wait = 0;
    if (!nv2a_segments_enabled() || !safe_point)
        fail("publication requires the enabled queue and a safe point");
    if (!valid_put(put))
        fail("PUT is outside aligned physical RAM");
    for (;;) {
        Caller *counter;
        lock_queue(safe_point, started);
        check_registers();
        if (s_count < NV2A_SEGMENT_QUEUE_CAPACITY) {
            unsigned tail = (s_head + s_count) % NV2A_SEGMENT_QUEUE_CAPACITY;
            InterlockedExchange((volatile LONG *)s_put, (LONG)put);
            s_last_put = put;
            s_queue[tail].put = put;
            s_queue[tail].caller = caller;
            s_count++;
            InterlockedIncrement(&s_outstanding);
            if (s_count > s_high_water) s_high_water = s_count;
            s_published++;
            counter = caller_slot(caller);
            if (counter) counter->published++;
            report();
            LeaveCriticalSection(&s_lock);
            nv2a_idle_wake();
            return;
        }
        if (!counted_wait) {
            s_waits++;
            counted_wait = 1;
        }
        LeaveCriticalSection(&s_lock);
        if ((DWORD)(GetTickCount() - started) >= NV2A_SEGMENT_WAIT_MS)
            fail("queue-space wait timed out without publishing PUT");
        safe_point();
        {
            DWORD result = nv2a_segment_wait_once(s_space);
            if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT)
                fail("queue-space wait failed");
        }
    }
}

int nv2a_segments_peek(uint32_t *from, uint32_t *put, uint32_t *caller, int *first)
{
    int present;
    if (!nv2a_segments_enabled() || !from || !put || !caller || !first)
        fail("invalid queue peek");
    lock_queue(NULL, GetTickCount());
    check_registers();
    present = s_count != 0;
    if (present) {
        *from = s_last_get;
        *put = s_queue[s_head].put;
        *caller = s_queue[s_head].caller;
        *first = s_first;
    }
    LeaveCriticalSection(&s_lock);
    return present;
}

void nv2a_segments_complete(uint32_t put)
{
    Caller *counter;
    lock_queue(NULL, GetTickCount());
    check_registers();
    if (!s_count || s_queue[s_head].put != put)
        fail("segment completion does not match the queue head");
    counter = caller_slot(s_queue[s_head].caller);
    if (counter) counter->completed++;
    InterlockedExchange((volatile LONG *)s_get, (LONG)put);
    s_last_get = put;
    s_head = (s_head + 1) % NV2A_SEGMENT_QUEUE_CAPACITY;
    s_count--;
    InterlockedDecrement(&s_outstanding);
    s_first = 0;
    s_completed++;
    report();
    LeaveCriticalSection(&s_lock);
    if (!SetEvent(s_space))
        fail("cannot signal queue space");
}

int nv2a_segments_pending(void)
{
    return s_enabled && InterlockedCompareExchange(&s_outstanding, 0, 0) != 0;
}

int nv2a_segments_pfb_blocked(int graph_trap_pending)
{
    return nv2a_segments_pending() && !graph_trap_pending;
}

int nv2a_segments_service(int flip_held, void (*resync)(uint32_t),
                          void (*walk)(uint32_t))
{
    uint32_t from, put, caller;
    int first;
    if (flip_held || !nv2a_segments_pending())
        return 0;
    if (!resync || !walk)
        fail("missing segment walker");
    if (!nv2a_segments_peek(&from, &put, &caller, &first))
        return 0;
    (void)caller;
    if (first)
        resync(from);
    if (put != from)
        walk(put);
    nv2a_segments_complete(put);
    return 1;
}

void nv2a_segments_shutdown(void)
{
    if (!nv2a_segments_enabled())
        return;
    lock_queue(NULL, GetTickCount());
    if (s_count)
        fprintf(stderr, "[NV2A-SEGMENT] shutdown with %u unconsumed segments;"
                        " GET was not advanced\n", s_count);
    s_bound = 0;
    s_count = 0;
    s_head = 0;
    InterlockedExchange(&s_outstanding, 0);
    LeaveCriticalSection(&s_lock);
    if (!SetEvent(s_space))
        fail("cannot wake a producer during shutdown");
}
