/*
 * recomp_exit_hook.h - work that must run before ExitProcess.
 *
 * The Win32 windows end the run with ExitProcess, which skips atexit. The
 * kernel's exit-time reports (the missing-file summary) register here, and
 * each window and the firmware-exit bridge call recomp_exit_hook_run before
 * ExitProcess. It lives in the platform layer because the D3D library is
 * linked without the kernel in its standalone tests.
 */
#ifndef RECOMP_EXIT_HOOK_H
#define RECOMP_EXIT_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*recomp_exit_hook_fn)(const char *why);

/* One hook; a second call replaces the first. Any thread. */
void recomp_exit_hook_set(recomp_exit_hook_fn fn);

/* Add an independent hook without replacing the primary hook. Returns zero
 * when the bounded registry is full. Register during startup. */
int recomp_exit_hook_add(recomp_exit_hook_fn fn);

/* Calls the registered hooks with why ("window", "firmware", or "shutdown"). */
void recomp_exit_hook_run(const char *why);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_EXIT_HOOK_H */
