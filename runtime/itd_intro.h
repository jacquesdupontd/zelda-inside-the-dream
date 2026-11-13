// Inside The Dream: Port0r's animated logo before the start screen (runtime/itd_intro.c, runtime/ui/launcher.mm).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ItdIntro ItdIntro;

// Opens the video at path, its pictures scaled to w x h RGBA; its sound decoded whole into *pcm (interleaved float,
// *channels per frame, *rate Hz, *frames frames; free() it). NULL: not there or not readable (then nothing is shown).
ItdIntro *itd_intro_open(const char *path, int w, int h, float **pcm, int *frames, int *channels, int *rate);
// The picture for time t (seconds from the start) when it is a new one since the last call, else NULL.
const uint8_t *itd_intro_picture(ItdIntro *in, double t);
// Its length in seconds; true once its last picture was given.
double itd_intro_duration(const ItdIntro *in);
bool itd_intro_ended(const ItdIntro *in);
void itd_intro_close(ItdIntro *in);

#ifdef __cplusplus
}
#endif
