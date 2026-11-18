#include "nv2a_idle_wait.h"
#include "platform/xbox_winnt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

#ifndef NV2A_IDLE_BACKSTOP_NS
#define NV2A_IDLE_BACKSTOP_NS 200000u
#endif
#ifndef NV2A_IDLE_SPIN_PASSES
#define NV2A_IDLE_SPIN_PASSES 64u
#endif

static volatile LONG s_enabled = -1;
static unsigned s_spin_remaining;
static unsigned long long s_waits, s_events, s_timers;
static DWORD s_last_report;

static int option(void)
{
    const char *value = getenv("RECOMP_NV2A_IDLE_WAIT");
    if (!value || !*value || strcmp(value, "0") == 0)
        return 0;
    if (strcmp(value, "1") == 0)
        return 1;
    fprintf(stderr, "[NV2A-IDLE] RECOMP_NV2A_IDLE_WAIT must be 0 or 1\n");
    fflush(stderr);
    abort();
}

#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static HANDLE s_event, s_timer;

static BOOL CALLBACK initialize(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    int enabled = option();
    (void)once; (void)parameter; (void)context;
    if (enabled) {
        s_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        s_timer = CreateWaitableTimerExW(NULL, NULL,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
        if (!s_event || !s_timer) {
            fprintf(stderr, "[NV2A-IDLE] cannot create private wake/timer: %lu\n",
                    (unsigned long)GetLastError());
            fflush(stderr);
            abort();
        }
        fprintf(stderr, "[NV2A-IDLE] independent idle wait enabled"
                        " (%u ns backstop)\n", (unsigned)NV2A_IDLE_BACKSTOP_NS);
    }
    InterlockedExchange(&s_enabled, enabled);
    return TRUE;
}

static void ensure_initialized(void)
{
    if (!InitOnceExecuteOnce(&s_once, initialize, NULL, NULL)) {
        fprintf(stderr, "[NV2A-IDLE] initialization failed: %lu\n",
                (unsigned long)GetLastError());
        abort();
    }
}

static void signal_wake(void)
{
    if (!SetEvent(s_event)) {
        fprintf(stderr, "[NV2A-IDLE] wake failed: %lu\n",
                (unsigned long)GetLastError());
        abort();
    }
}

static int wait_once(void)
{
    HANDLE handles[2] = { s_event, s_timer };
    LARGE_INTEGER due;
    DWORD result;
    due.QuadPart = -(LONGLONG)(NV2A_IDLE_BACKSTOP_NS / 100u);
    if (!SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE)) {
        fprintf(stderr, "[NV2A-IDLE] timer arm failed: %lu\n",
                (unsigned long)GetLastError());
        abort();
    }
    result = WaitForMultipleObjects(2, handles, FALSE, 1000);
    if (result == WAIT_OBJECT_0)
        return 1;
    if (result == WAIT_OBJECT_0 + 1)
        return 0;
    fprintf(stderr, "[NV2A-IDLE] wait failed or timer stalled:"
                    " result=%lu error=%lu\n",
            (unsigned long)result, (unsigned long)GetLastError());
    fflush(stderr);
    abort();
}

#else
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond;
static int s_pending;

static void initialize(void)
{
    int enabled = option();
    if (enabled) {
#if defined(__APPLE__)
        if (pthread_cond_init(&s_cond, NULL) != 0)
            abort();
#else
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) != 0 ||
            pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
            pthread_cond_init(&s_cond, &attr) != 0 ||
            pthread_condattr_destroy(&attr) != 0)
            abort();
#endif
        fprintf(stderr, "[NV2A-IDLE] independent idle wait enabled"
                        " (%u ns backstop)\n", (unsigned)NV2A_IDLE_BACKSTOP_NS);
    }
    InterlockedExchange(&s_enabled, enabled);
}

static void ensure_initialized(void)
{
    if (pthread_once(&s_once, initialize) != 0) {
        fprintf(stderr, "[NV2A-IDLE] initialization failed\n");
        abort();
    }
}

static void signal_wake(void)
{
    if (pthread_mutex_lock(&s_lock) != 0)
        abort();
    s_pending = 1;
    if (pthread_cond_signal(&s_cond) != 0 || pthread_mutex_unlock(&s_lock) != 0)
        abort();
}

static int wait_once(void)
{
    int result = 0, signalled;
#if defined(__APPLE__)
    struct timespec deadline = { NV2A_IDLE_BACKSTOP_NS / 1000000000u,
                                 NV2A_IDLE_BACKSTOP_NS % 1000000000u };
#else
    struct timespec deadline;
#endif
    if (pthread_mutex_lock(&s_lock) != 0)
        abort();
    if (!s_pending) {
#if defined(__APPLE__)
        result = pthread_cond_timedwait_relative_np(&s_cond, &s_lock, &deadline);
#else
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
            abort();
        deadline.tv_sec += NV2A_IDLE_BACKSTOP_NS / 1000000000u;
        deadline.tv_nsec += NV2A_IDLE_BACKSTOP_NS % 1000000000u;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        result = pthread_cond_timedwait(&s_cond, &s_lock, &deadline);
#endif
    }
    signalled = s_pending;
    s_pending = 0;
    if (pthread_mutex_unlock(&s_lock) != 0)
        abort();
    if (result != 0 && result != ETIMEDOUT) {
        fprintf(stderr, "[NV2A-IDLE] wait failed: %d\n", result);
        abort();
    }
    return signalled;
}
#endif

int nv2a_idle_wait_enabled(void)
{
    ensure_initialized();
    return InterlockedCompareExchange(&s_enabled, 0, 0) == 1;
}

void nv2a_idle_wake(void)
{
    if (nv2a_idle_wait_enabled())
        signal_wake();
}

void nv2a_idle_wait_after_pass(int did_work)
{
    DWORD now;
    if (!nv2a_idle_wait_enabled()) {
        Sleep(0);
        return;
    }
    if (did_work)
        s_spin_remaining = NV2A_IDLE_SPIN_PASSES;
    if (s_spin_remaining) {
        s_spin_remaining--;
        Sleep(0);
        return;
    }
    s_waits++;
    if (wait_once())
        s_events++;
    else
        s_timers++;
    now = GetTickCount();
    if (!s_last_report)
        s_last_report = now ? now : 1;
    else if ((DWORD)(now - s_last_report) >= 10000u) {
        fprintf(stderr, "[NV2A-IDLE] waits=%llu events=%llu timers=%llu\n",
                s_waits, s_events, s_timers);
        s_last_report = now;
    }
}
