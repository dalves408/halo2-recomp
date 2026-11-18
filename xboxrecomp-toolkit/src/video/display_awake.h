#ifndef RECOMP_DISPLAY_AWAKE_H
#define RECOMP_DISPLAY_AWAKE_H

#include <windows.h>

typedef struct RecompDisplayAwakeApi {
    BOOL (WINAPI *is_window)(HWND);
    HWND (WINAPI *root)(HWND, UINT);
    HWND (WINAPI *foreground)(void);
    BOOL (WINAPI *is_iconic)(HWND);
    BOOL (WINAPI *is_visible)(HWND);
    EXECUTION_STATE (WINAPI *set_execution_state)(EXECUTION_STATE);
    DWORD (WINAPI *last_error)(void);
} RecompDisplayAwakeApi;

typedef struct RecompDisplayAwake {
    const RecompDisplayAwakeApi *api;
    unsigned failures;
    int enabled;
    int held;
} RecompDisplayAwake;

/* These calls must all run on the window thread: execution state is per-thread.
 * The API parameter is NULL in production and may be substituted by tests. */
int recomp_display_awake_init(RecompDisplayAwake *state, const char *option,
                              const RecompDisplayAwakeApi *api);
void recomp_display_awake_update(RecompDisplayAwake *state, HWND window);
void recomp_display_awake_shutdown(RecompDisplayAwake *state);

#endif
