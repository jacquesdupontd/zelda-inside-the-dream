// The on-screen controller of a touch screen (the Android tablet: android/flat_app.c lays it out and reads the touches,
// runtime/ui/touch_pad.mm draws it over the game, from menu.mm). Places in fractions of the image: x of its width, y
// and r of its height (round on any shape).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TP_ROUND, TP_PILL, TP_STICK, TP_MENU };
typedef struct {
    float x, y, r;   // the centre, the radius (a pill: half its height)
    float kx, ky;    // a stick: its knob
    char label[4];
    unsigned char shape, down;
} TouchPadItem;
#define TOUCH_PAD_MAX 24

void touch_pad_set(const TouchPadItem *items, int n, bool shown);  // (any thread: copied)
bool touch_pad_wanted(void);

#ifdef __cplusplus
}
#endif
