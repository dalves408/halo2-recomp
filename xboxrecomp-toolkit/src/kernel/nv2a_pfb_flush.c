#include "nv2a_pfb_flush.h"
#include "platform/xbox_winnt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

#define PFB_FLUSH_BACKSTOP_NS 200000u
#ifndef NV2A_PFB_DIAG_INTERVAL_MS
#define NV2A_PFB_DIAG_INTERVAL_MS 2000u
#endif
#ifndef NV2A_PFB_WAIT_LIMIT_MS
#define NV2A_PFB_WAIT_LIMIT_MS 30000u
#endif

enum PfbWake {
    PFB_WAKE_EVENT,
    PFB_WAKE_TIMER,
    PFB_WAKE_SPURIOUS
};

static volatile LONG s_enabled = -1;
static volatile LONGLONG s_waits;
static volatile LONGLONG s_acks;
static volatile LONGLONG s_event_wakes;
static volatile LONGLONG s_timer_wakes;
static volatile LONGLONG s_spurious_wakes;
static volatile LONGLONG s_rechecks;
static volatile LONGLONG s_safe_points;
static volatile LONG s_last_report_ms;
static volatile LONG s_wait_diag;
static volatile LONG s_wait_diag_reports;
static volatile LONG s_last_flip_held = -1;

static void counter_increment(volatile LONGLONG *counter)
{
    LONGLONG value = InterlockedCompareExchange64(counter, 0, 0);

    while (value != LLONG_MAX) {
        LONGLONG previous =
            InterlockedCompareExchange64(counter, value + 1, value);
        if (previous == value)
            return;
        value = previous;
    }
}

static LONGLONG counter_read(volatile LONGLONG *counter)
{
    return InterlockedCompareExchange64(counter, 0, 0);
}

static int parse_enabled(void)
{
    const char *value = getenv("RECOMP_PFB_FLUSH_WAIT");

    if (!value || strcmp(value, "0") == 0)
        return 0;
    if (strcmp(value, "1") == 0)
        return 1;
    fprintf(stderr, "[PFB-WAIT] RECOMP_PFB_FLUSH_WAIT must be 0 or 1\n");
    fflush(stderr);
    abort();
}

static int parse_wait_diag(void)
{
    const char *value = getenv("RECOMP_NV2A_WAIT_DIAG");
    if (!value || !*value || strcmp(value, "0") == 0)
        return 0;
    if (strcmp(value, "1") == 0)
        return 1;
    fprintf(stderr, "[NV2A-WAIT] RECOMP_NV2A_WAIT_DIAG must be 0 or 1\n");
    fflush(stderr);
    abort();
}

static int reserve_wait_diag_report(void)
{
    LONG count = InterlockedCompareExchange(&s_wait_diag_reports, 0, 0);
    while (count < 8) {
        LONG previous = InterlockedCompareExchange(&s_wait_diag_reports, count + 1, count);
        if (previous == count)
            return 1;
        count = previous;
    }
    return 0;
}

#if defined(_WIN32)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static HANDLE s_ack_event;
static XBOX_THREAD_LOCAL HANDLE s_backstop_timer;

static BOOL CALLBACK pfb_flush_init(PINIT_ONCE once, PVOID parameter,
                                    PVOID *context)
{
    int enabled = parse_enabled();
    int diagnostic = parse_wait_diag();
    (void)once;
    (void)parameter;
    (void)context;

    if (enabled) {
        s_ack_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!s_ack_event) {
            fprintf(stderr, "[PFB-WAIT] cannot create acknowledgement event: %lu\n",
                    (unsigned long)GetLastError());
            fflush(stderr);
            abort();
        }
        fprintf(stderr, "[PFB-WAIT] event-driven flush wait enabled "
                        "(200 us IRQ backstop)\n");
        fflush(stderr);
    }
    InterlockedExchange(&s_wait_diag, diagnostic);
    InterlockedExchange(&s_enabled, enabled);
    return TRUE;
}

static void ensure_initialized(void)
{
    if (!InitOnceExecuteOnce(&s_once, pfb_flush_init, NULL, NULL)) {
        fprintf(stderr, "[PFB-WAIT] cannot initialize flush wait: %lu\n",
                (unsigned long)GetLastError());
        fflush(stderr);
        abort();
    }
}

static enum PfbWake pfb_flush_wait_once(void)
{
    HANDLE handles[2];
    LARGE_INTEGER due;
    DWORD result;

    if (!s_backstop_timer) {
        s_backstop_timer =
            CreateWaitableTimerExW(NULL, NULL,
                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
        if (!s_backstop_timer) {
            fprintf(stderr, "[PFB-WAIT] cannot create high-resolution timer: %lu\n",
                    (unsigned long)GetLastError());
            fflush(stderr);
            abort();
        }
    }

    due.QuadPart = -(LONGLONG)(PFB_FLUSH_BACKSTOP_NS / 100u);
    if (!SetWaitableTimer(s_backstop_timer, &due, 0, NULL, NULL, FALSE)) {
        fprintf(stderr, "[PFB-WAIT] cannot arm high-resolution timer: %lu\n",
                (unsigned long)GetLastError());
        fflush(stderr);
        abort();
    }

    handles[0] = s_ack_event;
    handles[1] = s_backstop_timer;
    result = WaitForMultipleObjects(2, handles, FALSE, 1000);
    if (result == WAIT_OBJECT_0)
        return PFB_WAKE_EVENT;
    if (result == WAIT_OBJECT_0 + 1)
        return PFB_WAKE_TIMER;

    fprintf(stderr, "[PFB-WAIT] acknowledgement wait failed or timer stalled:"
                    " result=%lu error=%lu\n",
            (unsigned long)result, (unsigned long)GetLastError());
    fflush(stderr);
    abort();
}

static void signal_ack(void)
{
    if (!SetEvent(s_ack_event)) {
        fprintf(stderr, "[PFB-WAIT] cannot signal acknowledgement: %lu\n",
                (unsigned long)GetLastError());
        fflush(stderr);
        abort();
    }
}

#else

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t s_ack_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ack_cond;
static uint64_t s_ack_generation;

static void pfb_flush_init(void)
{
    int enabled = parse_enabled();
    int diagnostic = parse_wait_diag();

    if (enabled) {
#if defined(__APPLE__)
        if (pthread_cond_init(&s_ack_cond, NULL) != 0) {
            fprintf(stderr, "[PFB-WAIT] cannot initialize acknowledgement condition\n");
            abort();
        }
#else
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) != 0 ||
            pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
            pthread_cond_init(&s_ack_cond, &attr) != 0 ||
            pthread_condattr_destroy(&attr) != 0) {
            fprintf(stderr, "[PFB-WAIT] cannot initialize acknowledgement condition\n");
            abort();
        }
#endif
        fprintf(stderr, "[PFB-WAIT] event-driven flush wait enabled "
                        "(200 us IRQ backstop)\n");
        fflush(stderr);
    }
    InterlockedExchange(&s_wait_diag, diagnostic);
    InterlockedExchange(&s_enabled, enabled);
}

static void ensure_initialized(void)
{
    if (pthread_once(&s_once, pfb_flush_init) != 0) {
        fprintf(stderr, "[PFB-WAIT] cannot initialize flush wait\n");
        abort();
    }
}

static enum PfbWake pfb_flush_wait_once(void)
{
    uint64_t generation;
    uint64_t current_generation;
    int result;
#if defined(__APPLE__)
    struct timespec deadline = { 0, PFB_FLUSH_BACKSTOP_NS };
#else
    struct timespec deadline;
#endif

    if (pthread_mutex_lock(&s_ack_lock) != 0) {
        fprintf(stderr, "[PFB-WAIT] cannot lock acknowledgement condition\n");
        abort();
    }
    generation = s_ack_generation;
#if defined(__APPLE__)
    result = pthread_cond_timedwait_relative_np(&s_ack_cond, &s_ack_lock,
                                                &deadline);
#else
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        fprintf(stderr, "[PFB-WAIT] cannot read the monotonic clock\n");
        abort();
    }
    deadline.tv_nsec += PFB_FLUSH_BACKSTOP_NS;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    result = pthread_cond_timedwait(&s_ack_cond, &s_ack_lock, &deadline);
#endif
    current_generation = s_ack_generation;
    if (pthread_mutex_unlock(&s_ack_lock) != 0) {
        fprintf(stderr, "[PFB-WAIT] cannot unlock acknowledgement condition\n");
        abort();
    }
    if (current_generation != generation)
        return PFB_WAKE_EVENT;
    if (result == ETIMEDOUT)
        return PFB_WAKE_TIMER;
    if (result == 0)
        return PFB_WAKE_SPURIOUS;

    fprintf(stderr, "[PFB-WAIT] acknowledgement wait failed: %d\n", result);
    abort();
}

static void signal_ack(void)
{
    if (pthread_mutex_lock(&s_ack_lock) != 0) {
        fprintf(stderr, "[PFB-WAIT] cannot lock acknowledgement condition\n");
        abort();
    }
    s_ack_generation++;
    if (pthread_cond_broadcast(&s_ack_cond) != 0) {
        fprintf(stderr, "[PFB-WAIT] cannot signal acknowledgement condition\n");
        abort();
    }
    if (pthread_mutex_unlock(&s_ack_lock) != 0) {
        fprintf(stderr, "[PFB-WAIT] cannot unlock acknowledgement condition\n");
        abort();
    }
}

#endif

static void maybe_report(void)
{
    DWORD now = GetTickCount();
    LONG last = InterlockedCompareExchange(&s_last_report_ms, 0, 0);
    LONG stamp = (LONG)(now ? now : 1);

    if (!last) {
        InterlockedCompareExchange(&s_last_report_ms, stamp, 0);
        return;
    }
    if ((DWORD)(now - (DWORD)last) < 10000u ||
        InterlockedCompareExchange(&s_last_report_ms, stamp, last) != last)
        return;

    fprintf(stderr,
            "[PFB-WAIT] waits=%lld acks=%lld event_wakes=%lld "
            "timer_wakes=%lld spurious_wakes=%lld rechecks=%lld "
            "safe_points=%lld\n",
            (long long)counter_read(&s_waits),
            (long long)counter_read(&s_acks),
            (long long)counter_read(&s_event_wakes),
            (long long)counter_read(&s_timer_wakes),
            (long long)counter_read(&s_spurious_wakes),
            (long long)counter_read(&s_rechecks),
            (long long)counter_read(&s_safe_points));
    fflush(stderr);
}

int nv2a_pfb_flush_wait_enabled(void)
{
    ensure_initialized();
    return InterlockedCompareExchange(&s_enabled, 0, 0) == 1;
}

void nv2a_pfb_flush_wait(volatile uint32_t *reg,
                         Nv2aPfbFlushSafePoint safe_point)
{
    int diagnostic;
    DWORD started, last;
    if (!reg || !safe_point) {
        fprintf(stderr, "[PFB-WAIT] flush register and safe point are required\n");
        abort();
    }
    if (InterlockedCompareExchange(&s_enabled, 0, 0) != 1) {
        fprintf(stderr, "[PFB-WAIT] wait called while the option is disabled\n");
        abort();
    }

    counter_increment(&s_waits);
    diagnostic = InterlockedCompareExchange(&s_wait_diag, 0, 0) == 1;
    started = last = GetTickCount();
    while ((uint32_t)InterlockedCompareExchange((volatile LONG *)reg, 0, 0)
           & NV2A_PFB_FLUSH_BUSY_MASK) {
        enum PfbWake wake = pfb_flush_wait_once();

        if (wake == PFB_WAKE_EVENT)
            counter_increment(&s_event_wakes);
        else if (wake == PFB_WAKE_TIMER)
            counter_increment(&s_timer_wakes);
        else
            counter_increment(&s_spurious_wakes);
        safe_point();
        counter_increment(&s_safe_points);
        if ((uint32_t)InterlockedCompareExchange((volatile LONG *)reg, 0, 0)
            & NV2A_PFB_FLUSH_BUSY_MASK)
            counter_increment(&s_rechecks);
        if ((uint32_t)InterlockedCompareExchange((volatile LONG *)reg, 0, 0)
            & NV2A_PFB_FLUSH_BUSY_MASK) {
            DWORD now = GetTickCount();
            if ((DWORD)(now - started) >= NV2A_PFB_WAIT_LIMIT_MS) {
                fprintf(stderr, "[PFB-WAIT] acknowledgement timed out after %lu ms:"
                                " register=%08X last_flip_hold=%ld; busy bit remains set\n",
                        (unsigned long)(DWORD)(now - started),
                        (unsigned)InterlockedCompareExchange((volatile LONG *)reg, 0, 0),
                        (long)InterlockedCompareExchange(&s_last_flip_held, 0, 0));
                fflush(stderr);
                abort();
            }
            if (diagnostic && (DWORD)(now - last) >= NV2A_PFB_DIAG_INTERVAL_MS) {
                last = now;
                if (reserve_wait_diag_report()) {
                    fprintf(stderr, "[NV2A-WAIT] PFB flush still busy after %lu ms:"
                                    " register=%08X last_flip_hold=%ld waits=%lld acks=%lld"
                                    " rechecks=%lld%s\n",
                            (unsigned long)(DWORD)(now - started), (unsigned)*reg,
                            (long)InterlockedCompareExchange(&s_last_flip_held, 0, 0),
                            (long long)counter_read(&s_waits),
                            (long long)counter_read(&s_acks),
                            (long long)counter_read(&s_rechecks),
                            InterlockedCompareExchange(&s_wait_diag_reports, 0, 0) >= 8
                                ? " (further wait reports suppressed)" : "");
                    fflush(stderr);
                }
            }
        }
    }
    maybe_report();
}

int nv2a_pfb_flush_ack(volatile uint32_t *reg, int flip_held)
{
    LONG previous;

    if (!reg) {
        fprintf(stderr, "[PFB-WAIT] acknowledgement requires a flush register\n");
        abort();
    }
    if (s_wait_diag == 1)
        InterlockedExchange(&s_last_flip_held, flip_held != 0);
    if (flip_held)
        return 0;
    if (!(*reg & NV2A_PFB_FLUSH_BUSY_MASK))
        return 0;

    previous = InterlockedAnd((volatile LONG *)reg,
                              (LONG)~NV2A_PFB_FLUSH_BUSY_MASK);
    if (!((uint32_t)previous & NV2A_PFB_FLUSH_BUSY_MASK))
        return 0;
    if (InterlockedCompareExchange(&s_enabled, 0, 0) == 1) {
        counter_increment(&s_acks);
        signal_ack();
    }
    return 1;
}
