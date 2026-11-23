/* The build's version, compared with the GitHub release tags (update_check.c).
 * Test builds count up to 1.0 as 0.<perf>.0; releases are tagged v<major>.<minor>.<patch>. */
#ifndef H2N_VERSION_H
#define H2N_VERSION_H

#define H2N_VERSION "1.0.6"

/* The GitHub repository whose latest release the menus check ("owner/name").
 * Empty: the check is off and nothing is contacted. Each release carries
 * Halo2-Recomp-v<version>-Windows-x64.zip (scripts/build_installer.py), laid
 * out like the install, which the updater copies over it (src/updater.c). */
#define H2N_UPDATE_REPO "dalves408/halo2-recomp"

#endif
