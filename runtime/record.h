// Inside The Dream's recorder (runtime/nvn/vulkan/record_vk.c on Windows and Linux, runtime/nvn/metal.m on the Mac):
// what the window shows, with the game's sound, into Videos/Inside The Dream. F10 or the Video tab's Record a video.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void rec_toggle(void);          // start or stop (it happens at the next image the window shows)
bool rec_active(void);          // recording now
double rec_seconds(void);       // how long the current recording runs
const char *rec_status(void);   // the last thing to say about it ("" at first): where it went, or what failed
const char *rec_folder(void);   // where the videos go (rec_dir in the settings, else Videos/Inside The Dream)
#ifdef __cplusplus
}
#endif
