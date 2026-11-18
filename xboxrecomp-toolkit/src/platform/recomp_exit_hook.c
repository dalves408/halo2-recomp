/*
 * recomp_exit_hook.c - hooks run before ExitProcess
 * (recomp_exit_hook.h).
 */
#include "recomp_exit_hook.h"

#include <stddef.h>

/* Set by whichever thread notes the first miss, read by the window thread:
 * an aligned pointer store, so the reader sees the old or the new hook. */
static recomp_exit_hook_fn volatile s_hook;
#define RECOMP_EXIT_HOOK_MAX 8
static recomp_exit_hook_fn s_additional[RECOMP_EXIT_HOOK_MAX];

void recomp_exit_hook_set(recomp_exit_hook_fn fn)
{
    s_hook = fn;
}

int recomp_exit_hook_add(recomp_exit_hook_fn fn)
{
    size_t i;

    if (fn == NULL)
        return 0;
    for (i = 0; i < RECOMP_EXIT_HOOK_MAX; ++i) {
        if (s_additional[i] == fn)
            return 1;
        if (s_additional[i] == NULL) {
            s_additional[i] = fn;
            return 1;
        }
    }
    return 0;
}

void recomp_exit_hook_run(const char *why)
{
    recomp_exit_hook_fn fn = s_hook;
    size_t i;

    if (fn != NULL)
        fn(why);
    for (i = 0; i < RECOMP_EXIT_HOOK_MAX; ++i) {
        fn = s_additional[i];
        if (fn != NULL)
            fn(why);
    }
}
