// Android test bed (build/android): what the Quest build leaves out for now - the start screen (its background video
// needs FFmpeg); the game starts with --play.
#include <stdio.h>
void launcher_run(const char *root) { (void)root; fprintf(stderr, "[android] no start screen: run with --play\n"); }
void launcher_play_seed(const char *seed) { (void)seed; }

// (FFmpeg: the real one now, build_ffmpeg_android.sh -> ffprefix)

// (runtime/ui/launcher.mm on the desktop: the shader caches tag, runtime/nvn/shader_prep.c)
const char *larecomp_version(void) { return "android"; }
