/* One game window across Halo 2's hand-overs to its downloader (h2_frame.c). */
#ifndef H2_FRAME_H
#define H2_FRAME_H

/* Called first in main(): when this process is the one the player started,
 * open the frame, run the game inside it until the last process ends and
 * return 1 with that process's exit code. 0: carry on as the game itself
 * (a process inside the frame, or H2_NO_FRAME=1). */
int h2_frame_main(int *exit_code);

#endif
