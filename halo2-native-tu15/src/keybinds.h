/*
 * keybinds.h -- the player's key map as Config.exe and the F1 menu edit it.
 *
 * keymap.ini (read by the toolkit's input/keyboard.c) binds up to four keys
 * or mouse inputs to each Xbox controller input. Here the controller inputs
 * are listed as Halo 2's actions, after the Halo 2 for Windows manual, and a
 * key belongs to one action at a time: binding it takes it off any other.
 * The player's copy is SaveData\keymap.ini; the install's keymap.ini holds
 * the defaults.
 */
#ifndef H2_KEYBINDS_H
#define H2_KEYBINDS_H

#include <stddef.h>

#define KB_NCTRL   24      /* controller inputs in the key map */
#define KB_MAXKEYS 4       /* keys per input in the file */
#define KB_SLOTS   3       /* keys per action shown for editing */

typedef struct {
    char key[KB_NCTRL][KB_MAXKEYS][24];   /* key map names: "w", "mouse1", "capslock" */
    int count[KB_NCTRL];
} KbMap;

/* An action row. tone: 0 white, 1 single wield only (yellow). dual: what it
 * does when dual wielding (shown in orange), or NULL. */
typedef struct {
    const char *section;      /* first row of a section names it, else NULL */
    const char *label;
    const char *dual;
    int tone;
    int ctrl;                 /* index into the key map's controls */
} KbAction;

extern const KbAction kb_actions[];
extern const int kb_action_count;

int  kb_load(const char *root, KbMap *m);            /* the player's copy, else the install's */
int  kb_load_defaults(const char *root, KbMap *m);   /* the install's keymap.ini */
int  kb_save(const char *root, const KbMap *m);      /* SaveData\keymap.ini */

/* The key in an action's slot, or NULL. */
const char *kb_slot(const KbMap *m, int action, int slot);
/* Bind a key (key map name) to an action's slot. It comes off any other
 * action first; the action it came from is returned (or -1). */
int  kb_bind(KbMap *m, int action, int slot, const char *name);
void kb_clear(KbMap *m, int action, int slot);

/* Windows virtual-key code (with the left/right side for Shift, Ctrl and Alt,
 * and the wheel as 0x0E up / 0x0F down) to a key map name, or NULL for a key
 * that cannot be bound (Esc, F1, F9 and the like). */
const char *kb_name_from_vk(int vk);
/* A key map name as the player reads it: "Left Mouse", "Caps Lock", "Q". */
void kb_display(const char *name, char *out, size_t n);

#endif
