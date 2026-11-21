/*
 * keybinds.c -- the player's key map as Config.exe and the F1 menu edit it
 * (see keybinds.h).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h2_layout.h"
#include "keybinds.h"

/* The controls, in the key map's names (input_map.c's k_control_names). */
static const char *const k_control[KB_NCTRL] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white",
    "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

/* After the Halo 2 for Windows manual's "Keyboard controls". */
const KbAction kb_actions[] = {
    { "Movement", "Forward",               NULL, 0, 16 },
    { NULL,       "Backward",              NULL, 0, 17 },
    { NULL,       "Strafe left",           NULL, 0, 18 },
    { NULL,       "Strafe right",          NULL, 0, 19 },
    { "Look (without a mouse)", "Look up", NULL, 0, 20 },
    { NULL,       "Look down",             NULL, 0, 21 },
    { NULL,       "Look left",             NULL, 0, 22 },
    { NULL,       "Look right",            NULL, 0, 23 },
    { "Combat",   "Fire weapon",           "right weapon", 0, 15 },
    { NULL,       "Throw grenade",         "left weapon", 1, 14 },
    { NULL,       "Switch grenade",        NULL, 0, 12 },
    { NULL,       "Melee attack",          NULL, 0, 9 },
    { NULL,       "Reload / pick up (hold)", NULL, 0, 10 },
    { NULL,       "Switch weapon",         "hold to dual", 0, 11 },
    { "Actions",  "Jump",                  NULL, 0, 8 },
    { NULL,       "Crouch",                NULL, 0, 6 },
    { NULL,       "Flashlight",            NULL, 0, 13 },
    { NULL,       "Zoom",                  NULL, 0, 7 },
    { NULL,       "Multiplayer score",     NULL, 0, 5 },
    { NULL,       "Pause",                 NULL, 0, 4 },
    { "Menus",    "Menu up",               NULL, 0, 0 },
    { NULL,       "Menu down",             NULL, 0, 1 },
    { NULL,       "Menu left",             NULL, 0, 2 },
    { NULL,       "Menu right",            NULL, 0, 3 },
};
const int kb_action_count = (int)(sizeof kb_actions / sizeof kb_actions[0]);

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
    for (i = 0; i < KB_NCTRL; i++)
        if (!_stricmp(name, k_control[i])) return i;
    return -1;
}

static int read_map(const char *path, KbMap *m)
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
        m->count[c] = 0;
        p = eq + 1;
        for (tok = strtok_s(p, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
            trim(&tok);
            if (!*tok || !_stricmp(tok, "none") || m->count[c] >= KB_MAXKEYS) continue;
            strncpy_s(m->key[c][m->count[c]], sizeof m->key[c][0], tok, _TRUNCATE);
            m->count[c]++;
        }
    }
    fclose(f);
    return 1;
}

int kb_load_defaults(const char *root, KbMap *m)
{
    char path[MAX_PATH];
    memset(m, 0, sizeof *m);
    snprintf(path, sizeof path, "%s\\keymap.ini", root);
    return read_map(path, m);
}

int kb_load(const char *root, KbMap *m)
{
    char path[MAX_PATH];
    /* The defaults first, so a control the player's file leaves out keeps them. */
    kb_load_defaults(root, m);
    h2_data_path(root, "keymap.ini", path, sizeof path);
    read_map(path, m);
    return 1;
}

int kb_save(const char *root, const KbMap *m)
{
    char path[MAX_PATH], dir[MAX_PATH];
    FILE *f;
    int c, k, ok;
    h2_data_path(root, "", dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    h2_data_path(root, "keymap.ini", path, sizeof path);
    if (!(f = fopen(path, "w"))) return 0;
    fprintf(f, "; Halo 2 keyboard and mouse layout (your copy, written by Config.exe and the F1\n"
               "; menu; the install's keymap.ini has the defaults and the list of key names).\n"
               "; Up to four keys per Xbox controller input, comma separated; \"none\" leaves\n"
               "; it unbound. Mouse buttons and the wheel count only while Mouse aim is on.\n\n[keys]\n");
    for (c = 0; c < KB_NCTRL; c++) {
        fprintf(f, "%s = ", k_control[c]);
        if (!m->count[c]) fprintf(f, "none");
        for (k = 0; k < m->count[c]; k++) fprintf(f, "%s%s", k ? ", " : "", m->key[c][k]);
        fprintf(f, "\n");
    }
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

const char *kb_slot(const KbMap *m, int action, int slot)
{
    int c;
    if (action < 0 || action >= kb_action_count || slot < 0 || slot >= KB_MAXKEYS) return NULL;
    c = kb_actions[action].ctrl;
    return slot < m->count[c] ? m->key[c][slot] : NULL;
}

static void remove_at(KbMap *m, int c, int k)
{
    memmove(m->key[c][k], m->key[c][k + 1], (size_t)(m->count[c] - k - 1) * sizeof m->key[c][0]);
    m->count[c]--;
}

void kb_clear(KbMap *m, int action, int slot)
{
    int c;
    if (action < 0 || action >= kb_action_count) return;
    c = kb_actions[action].ctrl;
    if (slot >= 0 && slot < m->count[c])
        remove_at(m, c, slot);
}

int kb_bind(KbMap *m, int action, int slot, const char *name)
{
    int c, k, from = -1, i, target;
    if (action < 0 || action >= kb_action_count || !name || !*name) return -1;
    target = kb_actions[action].ctrl;
    /* Off every control first (the same key twice on one action included). */
    for (c = 0; c < KB_NCTRL; c++)
        for (k = 0; k < m->count[c]; k++)
            if (!_stricmp(m->key[c][k], name)) {
                if (c != target)
                    for (i = 0; i < kb_action_count; i++)
                        if (kb_actions[i].ctrl == c) { from = i; break; }
                if (c == target && k < slot) slot--;
                remove_at(m, c, k);
                k--;
            }
    if (slot < 0) slot = 0;
    if (slot >= m->count[target]) {
        if (m->count[target] >= KB_MAXKEYS) m->count[target] = KB_MAXKEYS - 1;
        slot = m->count[target];
        m->count[target]++;
    }
    strncpy_s(m->key[target][slot], sizeof m->key[target][0], name, _TRUNCATE);
    return from;
}

/* ---- key names ---------------------------------------------------------- */

typedef struct { int vk; const char *name; const char *show; } KeyInfo;

/* The key map's names (input_map.c) and how they read. */
static const KeyInfo k_info[] = {
    { 0x01, "mouse1", "Left Mouse" }, { 0x02, "mouse2", "Right Mouse" },
    { 0x04, "mouse3", "Mid Mouse" }, { 0x05, "mouse4", "Mouse 4" }, { 0x06, "mouse5", "Mouse 5" },
    { 0x0E, "mwheelup", "Wheel Up" }, { 0x0F, "mwheeldown", "Wheel Down" },
    { VK_BACK, "backspace", "Backspace" }, { VK_TAB, "tab", "Tab" }, { VK_RETURN, "enter", "Enter" },
    { VK_CAPITAL, "capslock", "Caps Lock" }, { VK_SPACE, "space", "Space" },
    { VK_PRIOR, "pageup", "Page Up" }, { VK_NEXT, "pagedown", "Page Down" },
    { VK_END, "end", "End" }, { VK_HOME, "home", "Home" },
    { VK_LEFT, "left", "Left" }, { VK_UP, "up", "Up" },
    { VK_RIGHT, "right", "Right" }, { VK_DOWN, "down", "Down" },
    { VK_INSERT, "insert", "Insert" }, { VK_PAUSE, "pause", "Pause" },
    { VK_MULTIPLY, "numpadmultiply", "Num *" }, { VK_ADD, "numpadadd", "Num +" },
    { VK_SUBTRACT, "numpadsubtract", "Num -" }, { VK_DECIMAL, "numpaddecimal", "Num ." },
    { VK_DIVIDE, "numpaddivide", "Num /" },
    { VK_LSHIFT, "lshift", "Left Shift" }, { VK_RSHIFT, "rshift", "Right Shift" },
    { VK_LCONTROL, "lctrl", "Left Ctrl" }, { VK_RCONTROL, "rctrl", "Right Ctrl" },
    { VK_LMENU, "lalt", "Left Alt" }, { VK_RMENU, "ralt", "Right Alt" },
    { VK_OEM_1, "semicolon", ";" }, { VK_OEM_PLUS, "equals", "=" }, { VK_OEM_COMMA, "comma", "," },
    { VK_OEM_MINUS, "minus", "-" }, { VK_OEM_PERIOD, "period", "." }, { VK_OEM_2, "slash", "/" },
    { VK_OEM_3, "grave", "`" }, { VK_OEM_4, "leftbracket", "[" }, { VK_OEM_5, "backslash", "\\" },
    { VK_OEM_6, "rightbracket", "]" }, { VK_OEM_7, "quote", "'" },
    /* names only read from a file */
    { -1, "shift", "Shift" }, { -1, "ctrl", "Ctrl" }, { -1, "control", "Ctrl" }, { -1, "alt", "Alt" },
    { -1, "return", "Enter" }, { -1, "delete", "Delete" }, { -1, "escape", "Esc" }, { -1, "esc", "Esc" },
};
#define NINFO ((int)(sizeof k_info / sizeof k_info[0]))

const char *kb_name_from_vk(int vk)
{
    static char gen[64][8];
    static int next;
    char *b;
    int i;
    for (i = 0; i < NINFO; i++)
        if (k_info[i].vk == vk) return k_info[i].name;
    b = gen[next++ & 63];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { b[0] = (char)vk; b[1] = 0; return b; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(b, 8, "numpad%d", vk - VK_NUMPAD0); return b; }
    /* F1 is the settings menu and F9 frees the mouse pointer. */
    if (vk >= VK_F2 && vk <= VK_F12 && vk != VK_F9) { snprintf(b, 8, "f%d", vk - VK_F1 + 1); return b; }
    return NULL;
}

void kb_display(const char *name, char *out, size_t n)
{
    int i;
    if (!name || !*name) { snprintf(out, n, "%s", ""); return; }
    for (i = 0; i < NINFO; i++)
        if (!_stricmp(k_info[i].name, name)) { snprintf(out, n, "%s", k_info[i].show); return; }
    if (!_strnicmp(name, "numpad", 6) && name[6] >= '0' && name[6] <= '9' && !name[7]) {
        snprintf(out, n, "Num %c", name[6]);
        return;
    }
    if ((name[0] == 'f' || name[0] == 'F') && name[1] >= '0' && name[1] <= '9') {
        snprintf(out, n, "F%s", name + 1);
        return;
    }
    snprintf(out, n, "%s", name);
    if (!name[1] && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}
