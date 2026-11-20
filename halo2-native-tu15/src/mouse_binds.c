/*
 * mouse_binds.c -- the mouse part of the key map (see mouse_binds.h).
 *
 * The key map (keymap.ini, read by the toolkit's input/keyboard.c) binds up
 * to four keys or mouse inputs to each Xbox controller input:
 *     [keys]
 *     right_trigger = mouse1, 3
 * A mouse input is bound to at most one control here: setting it removes it
 * from every other line and adds it to its new one. The player's copy is
 * SaveData\keymap.ini; the first change starts from the install's keymap.ini
 * so the keyboard keys carry over. A control left with no keys is written as
 * "none", so the built-in defaults do not come back for it.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mouse_binds.h"

static const char *const k_mouse_key[MB_COUNT] = {
    "mouse1", "mouse2", "mouse3", "mouse4", "mouse5", "mwheelup", "mwheeldown"
};
const char *const mb_key_label[MB_COUNT] = {
    "Left click", "Right click", "Middle click", "Mouse button 4", "Mouse button 5",
    "Wheel up", "Wheel down"
};

/* The controls, in the key map's names (input_map.c's k_control_names). */
#define NCTRL 24
static const char *const k_control[NCTRL] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white",
    "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

/* What a mouse input can do: index 0 is nothing, the rest name a control. */
static const char *const k_target_control[] = {
    NULL, "right_trigger", "left_trigger", "right_thumb", "left_thumb",
    "a", "b", "x", "y", "white", "black", "start", "back",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};
const char *const mb_target_label[] = {
    "Nothing", "Fire (right trigger)", "Grenade (left trigger)", "Zoom (right stick click)",
    "Crouch (left stick click)", "Jump (A)", "Melee (B)", "Reload / action (X)",
    "Switch weapon (Y)", "White", "Black", "Start", "Back",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
};
const int mb_target_count = (int)(sizeof k_target_control / sizeof k_target_control[0]);

#define MAXKEYS 4
typedef struct {
    char key[NCTRL][MAXKEYS][24];
    int count[NCTRL];
    int named[NCTRL];          /* the file had a line for it */
} KeyMap;

static void trim(char **p)
{
    char *s = *p, *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    *p = s;
}

static int control_ix(const char *name)
{
    int i;
    for (i = 0; i < NCTRL; i++)
        if (!_stricmp(name, k_control[i])) return i;
    return -1;
}

static int read_map(const char *path, KeyMap *m)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int in_keys = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *eq, *tok, *ctx = NULL, *name;
        int c;
        trim(&p);
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') { in_keys = !_strnicmp(p, "[keys]", 6); continue; }
        if (!in_keys || !(eq = strchr(p, '='))) continue;
        *eq = 0;
        name = p;
        trim(&name);
        if ((c = control_ix(name)) < 0) continue;
        m->named[c] = 1;
        m->count[c] = 0;
        p = eq + 1;
        for (tok = strtok_s(p, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
            trim(&tok);
            if (!*tok || !_stricmp(tok, "none") || m->count[c] >= MAXKEYS) continue;
            strncpy_s(m->key[c][m->count[c]], sizeof m->key[c][0], tok, _TRUNCATE);
            m->count[c]++;
        }
    }
    fclose(f);
    return 1;
}

void mb_user_keymap_path(const char *root, char *out, size_t n)
{
    snprintf(out, n, "%s\\SaveData\\keymap.ini", root);
}

static int load_map(const char *root, KeyMap *m)
{
    char path[MAX_PATH];
    memset(m, 0, sizeof *m);
    mb_user_keymap_path(root, path, sizeof path);
    if (read_map(path, m)) return 1;
    snprintf(path, sizeof path, "%s\\keymap.ini", root);
    return read_map(path, m);
}

static int targets_from(const KeyMap *mp, int ok, int target[MB_COUNT]);

int mb_load(const char *root, int target[MB_COUNT])
{
    KeyMap m;
    return targets_from(&m, load_map(root, &m), target);
}

int mb_load_defaults(const char *root, int target[MB_COUNT])
{
    KeyMap m;
    char path[MAX_PATH];
    memset(&m, 0, sizeof m);
    snprintf(path, sizeof path, "%s\\keymap.ini", root);
    return targets_from(&m, read_map(path, &m), target);
}

static int targets_from(const KeyMap *mp, int ok, int target[MB_COUNT])
{
    const KeyMap m = *mp;
    int i, c, k, t;
    for (i = 0; i < MB_COUNT; i++) {
        target[i] = 0;
        for (c = 0; c < NCTRL && !target[i]; c++)
            for (k = 0; k < m.count[c]; k++)
                if (!_stricmp(m.key[c][k], k_mouse_key[i]))
                    for (t = 1; t < mb_target_count; t++)
                        if (!strcmp(k_target_control[t], k_control[c])) { target[i] = t; break; }
    }
    return ok;
}

int mb_save(const char *root, const int target[MB_COUNT])
{
    KeyMap m;
    char path[MAX_PATH], dir[MAX_PATH];
    FILE *f;
    int i, c, k, ok;
    load_map(root, &m);
    for (i = 0; i < MB_COUNT; i++) {
        /* off every line first */
        for (c = 0; c < NCTRL; c++)
            for (k = 0; k < m.count[c]; k++)
                if (!_stricmp(m.key[c][k], k_mouse_key[i])) {
                    memmove(m.key[c][k], m.key[c][k + 1], (size_t)(m.count[c] - k - 1) * sizeof m.key[c][0]);
                    m.count[c]--;
                    k--;
                }
        if (target[i] > 0 && target[i] < mb_target_count && (c = control_ix(k_target_control[target[i]])) >= 0) {
            if (m.count[c] >= MAXKEYS) m.count[c] = MAXKEYS - 1;   /* full: the last key gives way */
            strncpy_s(m.key[c][m.count[c]], sizeof m.key[c][0], k_mouse_key[i], _TRUNCATE);
            m.count[c]++;
            m.named[c] = 1;
        }
    }
    snprintf(dir, sizeof dir, "%s\\SaveData", root);
    CreateDirectoryA(dir, NULL);
    mb_user_keymap_path(root, path, sizeof path);
    if (!(f = fopen(path, "w"))) return 0;
    fprintf(f, "; Halo 2 keyboard and mouse layout (your copy; the install's keymap.ini has the\n"
               "; defaults and the list of key names). Up to four keys per Xbox controller\n"
               "; input, comma separated; \"none\" leaves it unbound. Mouse buttons and the wheel\n"
               "; count only while Mouse aim is on, and only in play: outside play left click\n"
               "; is Start and right click is B. The F1 menu and Config.exe edit the mouse lines.\n\n[keys]\n");
    for (c = 0; c < NCTRL; c++) {
        if (!m.named[c]) continue;
        fprintf(f, "%s = ", k_control[c]);
        if (!m.count[c]) fprintf(f, "none");
        for (k = 0; k < m.count[c]; k++) fprintf(f, "%s%s", k ? ", " : "", m.key[c][k]);
        fprintf(f, "\n");
    }
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}
