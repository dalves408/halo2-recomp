/* media.h: the Map Pack disc's bonus videos for config.exe's EXTRAS page.
 *
 * A video is the disc's CRI Sofdec file(s) (.sfd: an MPEG-1 system stream with MPEG-1 video and CRI ADX or AIX
 * audio; Killtacular is three parts played as one) or the H.264/AAC .mp4 that config.exe re-encoded them to. */
#ifndef H2_MEDIA_H
#define H2_MEDIA_H

#include <windows.h>
#include <stdint.h>

typedef struct Media Media;

/* Opens n .sfd parts played in a row, or one .mp4. NULL when it cannot be read. */
Media *media_open(const WCHAR *const *paths, int n);
void media_close(Media *m);

int media_width(const Media *m);
int media_height(const Media *m);
double media_fps(const Media *m);
int media_channels(const Media *m);       /* 2, or 6 (5.1: FL FR FC LFE BL BR) */
int media_rate(const Media *m);
double media_progress(const Media *m);    /* how far through the file(s) reading is, 0..1 */

/* The next picture: 1 (its time in seconds in *t), 0 at the end. */
int media_next_frame(Media *m, double *t);
/* The last picture as 32-bit BGRX, top row first. */
void media_frame_bgrx(Media *m, uint8_t *dst, int stride);
/* The last picture as NV12 (the encoder's input): the Y plane then interleaved Cb Cr, both with stride w. */
int media_frame_nv12(Media *m, uint8_t *dst);   /* 0 when the source has no YUV picture (an .mp4) */

/* Up to `frames` audio frames (interleaved 16-bit, media_channels() each); 0 at the end. */
int media_audio(Media *m, int16_t *out, int frames);

/* Re-encodes to H.264 + AAC (.mp4) with Windows' own encoders. progress gets 0..1; a non-zero *cancel stops it.
 * 1 done, 0 failed (err says why), -1 cancelled. The file is written as dst only when complete. */
int media_encode_mp4(const WCHAR *const *src, int n, const WCHAR *dst, void (*progress)(double, void *), void *user,
                     volatile LONG *cancel, char *err, size_t errn);

#endif
