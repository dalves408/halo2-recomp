#include "display_awake.h"

#include <stdio.h>
#include <string.h>

static const RecompDisplayAwakeApi s_windows = {
    IsWindow, GetAncestor, GetForegroundWindow, IsIconic, IsWindowVisible,
    SetThreadExecutionState, GetLastError
};

static void display_awake_failure(RecompDisplayAwake *state, const char *operation)
{
    unsigned count = ++state->failures;
    if (count <= 8 || (count % 256) == 0) {
        fprintf(stderr, "  [DISPLAY-AWAKE] %s failed (Windows error %lu)%s\n",
                operation, (unsigned long)state->api->last_error(),
                count == 8 ? "; further failures are rate-limited" : "");
        fflush(stderr);
    }
}

static void display_awake_set(RecompDisplayAwake *state, int hold)
{
    EXECUTION_STATE flags;
    if (!state->enabled || state->held == hold)
        return;
    flags = ES_CONTINUOUS | (hold ? ES_DISPLAY_REQUIRED : 0);
    if (!state->api->set_execution_state(flags)) {
        display_awake_failure(state, hold ? "acquire" : "release");
        return;
    }
    state->held = hold;
}

int recomp_display_awake_init(RecompDisplayAwake *state, const char *option,
                              const RecompDisplayAwakeApi *api)
{
    memset(state, 0, sizeof *state);
    state->api = api ? api : &s_windows;
    if (!option || !*option || !strcmp(option, "0"))
        return 1;
    if (strcmp(option, "1")) {
        fprintf(stderr, "  [DISPLAY-AWAKE] RECOMP_DISPLAY_AWAKE must be 0 or 1; disabled\n");
        return 0;
    }
    state->enabled = 1;
    return 1;
}

void recomp_display_awake_update(RecompDisplayAwake *state, HWND window)
{
    HWND host = NULL, foreground = NULL, foreground_host = NULL;
    int hold = 0;

    if (!state->enabled)
        return;
    if (window && state->api->is_window(window)) {
        host = state->api->root(window, GA_ROOT);
        if (!host) {
            display_awake_failure(state, "resolving window root");
        } else if (state->api->is_window(host) && state->api->is_visible(host) &&
                   !state->api->is_iconic(host)) {
            foreground = state->api->foreground();
            if (foreground && state->api->is_window(foreground)) {
                foreground_host = state->api->root(foreground, GA_ROOT);
                if (!foreground_host)
                    display_awake_failure(state, "resolving foreground root");
                hold = foreground_host == host;
            }
        }
    }
    display_awake_set(state, hold);
}

void recomp_display_awake_shutdown(RecompDisplayAwake *state)
{
    display_awake_set(state, 0);
}
