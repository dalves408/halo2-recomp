/* h2_mouse.h -- mouse aim, kept off Xbox Live (h2_mouse.c). */
#ifndef H2_MOUSE_H
#define H2_MOUSE_H

void h2_mouse_init(void);    /* at start, after the settings are loaded */
void h2_mouse_apply(void);   /* after the mouse settings change (settings.c calls it) */
void h2_mouse_binds_changed(void);   /* SaveData\keymap.ini was rewritten (F1 menu) */

/* The pad poll's hooks for port 0 (overrides/xinput_device.c): outside play
 * left click is START and right click is B. */
void h2_mouse_pad_begin(void);
void h2_mouse_pad_end(unsigned short *buttons, unsigned char *analog);

#endif
