/*
 * h2_release.c -- what the launch scripts did, for an installed game started
 * straight from its exe (Setup's shortcuts, RecompLauncher).
 *
 * In an install (h2_layout.h: the exe not in build-ninja\) the process the
 * player starts, before anything else:
 *   - works from the install folder, so game_files\ and data\ are found;
 *   - in a player release (H2_RELEASE), drops every RECOMP_* switch it
 *     inherited: developer switches have no say in a player's game;
 *   - sets the launch scripts' choices: its own window, keyboard, Direct3D 11,
 *     the network (out to the internet only for a verified console identity:
 *     console_id.c, nvnet_nat.c), the hand-over to the content downloader, and
 *     the console identity Insignia Setup saved in data\Identity.
 * The game and downloader processes it starts inherit all of it (H2_FRAME_HWND
 * marks them), so they leave it alone.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h2_layout.h"
#include "settings.h"

static void set_default(const char *name, const char *value)
{
    if (!getenv(name) || !*getenv(name))
        _putenv_s(name, value);
}

#ifdef H2_RELEASE
static void drop_developer_switches(void)
{
    char *env = GetEnvironmentStringsA(), *p, names[64][96];
    int n = 0, i;
    if (!env) return;
    for (p = env; *p && n < 64; p += strlen(p) + 1) {
        const char *eq = strchr(p + 1, '=');
        size_t len = eq ? (size_t)(eq - p) : 0;
        if (len && len < sizeof names[0] && !_strnicmp(p, "RECOMP", 6) && _strnicmp(p, "RECOMPLAUNCHER", 14)) {
            memcpy(names[n], p, len);
            names[n++][len] = 0;
        }
    }
    FreeEnvironmentStringsA(env);
    for (i = 0; i < n; i++)
        _putenv_s(names[i], "");        /* removes it from the C runtime's and Windows' copies */
}
#endif

/* "XboxHDKey: 00 11 22 ..." in hddinfo.txt: 32 hex digits */
static int hdkey_from(const char *path, char out[33])
{
    FILE *f;
    char line[256];
    int n = 0;
    if (fopen_s(&f, path, "r") || !f) return 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "XboxHDKey");
        if (!p) continue;
        for (p += 9; *p && n < 32; p++)
            if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
                out[n++] = *p;
        break;
    }
    fclose(f);
    out[n] = 0;
    SecureZeroMemory(line, sizeof line);
    return n == 32;
}

void h2_release_startup(void)
{
    char root[MAX_PATH], p[MAX_PATH], q[MAX_PATH], map[3 * MAX_PATH], key[33];

    if (h2_dev_layout() || getenv("H2_FRAME_HWND"))
        return;
#ifdef H2_RELEASE
    drop_developer_switches();
#endif
    halo2_install_root(root, sizeof root);
    SetCurrentDirectoryA(root);
    h2_data_path(root, "", p, sizeof p);
    CreateDirectoryA(p, NULL);

    set_default("RECOMP_FB_WINDOW", "1");
    set_default("RECOMP_KEYBOARD", "1");
    set_default("RECOMP_GPU", "d3d11");
    set_default("RECOMP_NET", "1");
    set_default("RECOMP_LAUNCH_DIR", h2_data_name());
    h2_bin_path(root, "halo2_downloader.exe", p, sizeof p);
    h2_bin_path(root, "halo2_recomp.exe", q, sizeof q);
    snprintf(map, sizeof map, "downloader.xbe=%s;default.xbe=%s", p, q);
    set_default("RECOMP_LAUNCH_MAP", map);

    /* The console identity, when Insignia Setup has saved a verified one. The
     * game's disk caches are made with it, so it is given on every start. */
    h2_data_path(root, "Identity\\eeprom.bin", p, sizeof p);
    h2_data_path(root, "Identity\\hddinfo.txt", q, sizeof q);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES && hdkey_from(q, key)) {
        set_default("RECOMP_XBOX_EEPROM", p);
        set_default("RECOMP_XBOX_HDKEY", key);
        h2_data_path(root, "Identity\\eepromkey.bin", p, sizeof p);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) set_default("RECOMP_XBOX_EEPROM_KEY_FILE", p);
        h2_data_path(root, "Identity\\titlekeys.bin", p, sizeof p);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) set_default("RECOMP_XBOX_TITLE_KEYS", p);
    }
    SecureZeroMemory(key, sizeof key);
}

/* RecompLauncher's Big Picture mode always starts fullscreen; desktop mode and
 * no launcher use the saved setting. */
int h2_start_fullscreen(int saved)
{
    const char *m = getenv("RECOMPLAUNCHER_MODE");
    if (m && !_stricmp(m, "bigpicture")) return 1;
    return saved;
}
