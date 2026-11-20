/* mouse_binds.h -- what each mouse button and wheel direction does in play,
 * read from and written to the key map (mouse_binds.c). Shared by the game's
 * F1 menu and Config.exe; no toolkit dependency. */
#ifndef MOUSE_BINDS_H
#define MOUSE_BINDS_H

#define MB_COUNT 7     /* left, right, middle, 4, 5, wheel up, wheel down */

extern const char *const mb_key_label[MB_COUNT];    /* "Left click", ... */
extern const int mb_target_count;
extern const char *const mb_target_label[];         /* "Nothing", "Fire (right trigger)", ... */

/* Each mouse input's target index (into mb_target_label; 0 = nothing), from
 * <root>\SaveData\keymap.ini, else <root>\keymap.ini. 1 when a file was read. */
int mb_load(const char *root, int target[MB_COUNT]);
/* The install's own keymap.ini only (Config.exe's Reset). */
int mb_load_defaults(const char *root, int target[MB_COUNT]);

/* Writes the whole key map to <root>\SaveData\keymap.ini with these mouse
 * bindings (the keyboard keys stay as they were). 1 on success. */
int mb_save(const char *root, const int target[MB_COUNT]);

/* <root>\SaveData\keymap.ini */
void mb_user_keymap_path(const char *root, char *out, size_t n);

#endif
