/*
 * update_check.h -- a quiet check for a newer release on GitHub, for the F1
 * menu and Config.exe. At most one request every 2 hours, shared through
 * SaveData\update.ini; it runs on its own thread and never shows a window.
 */
#ifndef H2N_UPDATE_CHECK_H
#define H2N_UPDATE_CHECK_H

#include <stddef.h>

enum { UPD_OFF, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_FAILED };

typedef struct {
    int state;
    char latest[32];          /* "1.0.1" */
    char url[512];            /* the release's zip */
} UpdInfo;

void upd_start(const char *root);       /* once per program start */
void upd_get(UpdInfo *out);
/* Copies the updater out of build-ninja\ and starts it: it waits for this
 * process to end, installs the zip and runs `relaunch` (a full path). The
 * caller then exits. Returns 0 if it could not start. */
int  upd_launch(const char *root, const char *relaunch);

#endif
