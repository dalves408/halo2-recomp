/*
 * h2_layout.h -- where an install keeps its programs, game files and data.
 *
 *   installed (Setup, the release ZIP):   <install>\halo2_recomp.exe, config.exe, ...
 *                                          <install>\game_files\   the disc's files (halo2_import)
 *                                          <install>\data\         settings, key map, saves, identity, logs
 *   development tree:                     <tree>\build-ninja\halo2_recomp.exe, ...
 *                                          <tree>\game\, <tree>\SaveData\
 *
 * The program tells the two apart by the folder it runs from (build-ninja or
 * not), so the game, Config.exe, Insignia Setup and the updater agree without
 * a build switch. The install is portable: nothing outside its folder.
 */
#ifndef H2_LAYOUT_H
#define H2_LAYOUT_H

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* 1 when this program runs from a development tree's build-ninja folder */
static __inline int h2_dev_layout(void)
{
    static int dev = -1;
    if (dev < 0) {
        char p[MAX_PATH], *sl;
        dev = 0;
        if (GetModuleFileNameA(NULL, p, sizeof p) && (sl = strrchr(p, '\\')) != NULL) {
            *sl = 0;
            if ((sl = strrchr(p, '\\')) != NULL && _stricmp(sl + 1, "build-ninja") == 0) dev = 1;
        }
    }
    return dev;
}

static __inline const char *h2_data_name(void) { return h2_dev_layout() ? "SaveData" : "data"; }
static __inline const char *h2_game_name(void) { return h2_dev_layout() ? "game" : "game_files"; }

/* <root>\<program>: a program beside the game (build-ninja\ in a development tree) */
static __inline void h2_bin_path(const char *root, const char *program, char *out, size_t n)
{
    if (h2_dev_layout()) snprintf(out, n, "%s\\build-ninja\\%s", root, program);
    else snprintf(out, n, "%s\\%s", root, program);
}

/* <root>\<data>\<rel> ("" for the data folder itself) */
static __inline void h2_data_path(const char *root, const char *rel, char *out, size_t n)
{
    if (rel && *rel) snprintf(out, n, "%s\\%s\\%s", root, h2_data_name(), rel);
    else snprintf(out, n, "%s\\%s", root, h2_data_name());
}

#endif
