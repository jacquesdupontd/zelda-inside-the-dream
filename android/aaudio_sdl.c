// aaudio_sdl.c: the game's sound on the Quest app (android/xr_app.c). The runtime plays through SDL audio streams
// (runtime/services/audio.c: open, put data, read how much is queued), but SDL's Android audio needs its Java side,
// which a NativeActivity doesn't have: these few calls are answered here with AAudio, Android's native audio. Being
// defined in the app's own library, they take precedence over libSDL3's for the runtime's calls.

#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

struct SDL_AudioStream {
    AAudioStream *as;
    SDL_AudioSpec spec;
    int frame_bytes;
    _Atomic float gain;
    uint8_t *ring;  // what the game queued, read by AAudio's callback (one writer, one reader)
    uint32_t size;
    _Atomic uint64_t r, w;
};

#define MAX_STREAMS 8
static SDL_AudioStream *g_streams[MAX_STREAMS];  // device id = index + 1

static aaudio_data_callback_result_t feed(AAudioStream *as, void *user, void *audio, int32_t frames) {
    (void)as;
    SDL_AudioStream *s = user;
    uint8_t *out = audio;
    uint64_t want = (uint64_t)frames * s->frame_bytes, r = s->r, have = s->w - r;
    uint64_t n = have < want ? have : want;
    for (uint64_t i = 0; i < n;) {  // (two pieces at most: the ring's end, then its start)
        uint32_t at = (uint32_t)((r + i) % s->size), piece = s->size - at;
        if (piece > n - i) piece = (uint32_t)(n - i);
        memcpy(out + i, s->ring + at, piece);
        i += piece;
    }
    memset(out + n, 0, want - n);  // (nothing queued: silence)
    s->r = r + n;
    float g = s->gain;
    if (g != 1.0f) {
        if (s->spec.format == SDL_AUDIO_F32)
            for (uint64_t i = 0; i < want / 4; i++) ((float *)out)[i] *= g;
        else
            for (uint64_t i = 0; i < want / 2; i++) ((int16_t *)out)[i] = (int16_t)(((int16_t *)out)[i] * g);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags == SDL_INIT_AUDIO) return true;  // (AAudio needs no setup)
    static bool (*real)(SDL_InitFlags);
    if (!real) real = (bool (*)(SDL_InitFlags))dlsym(RTLD_NEXT, "SDL_InitSubSystem");
    return real ? real(flags) : false;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback cb, void *user) {
    (void)devid, (void)cb, (void)user;
    int slot = 0;
    while (slot < MAX_STREAMS && g_streams[slot]) slot++;
    if (slot == MAX_STREAMS || (spec->format != SDL_AUDIO_F32 && spec->format != SDL_AUDIO_S16)) return NULL;
    SDL_AudioStream *s = calloc(1, sizeof *s);
    s->spec = *spec;
    s->frame_bytes = spec->channels * (spec->format == SDL_AUDIO_F32 ? 4 : 2);
    s->gain = 1.0f;
    s->size = (uint32_t)(spec->freq / 2) * (uint32_t)s->frame_bytes;  // half a second
    s->ring = calloc(1, s->size);
    AAudioStreamBuilder *b = NULL;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setFormat(b, spec->format == SDL_AUDIO_F32 ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, spec->channels);
    AAudioStreamBuilder_setSampleRate(b, spec->freq);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, feed, s);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s->as);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        fprintf(stderr, "[aaudio] %d channels at %d Hz refused: %s\n", spec->channels, spec->freq, AAudio_convertResultToText(r));
        free(s->ring), free(s);
        return NULL;
    }
    fprintf(stderr, "[aaudio] stream %d: %d channels, %d Hz, %s, burst %d frames\n", slot + 1, spec->channels, spec->freq,
            spec->format == SDL_AUDIO_F32 ? "float" : "s16", AAudioStream_getFramesPerBurst(s->as));
    g_streams[slot] = s;
    return s;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) { return s && AAudioStream_requestStart(s->as) == AAUDIO_OK; }

bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len) {
    if (!s || len <= 0) return s != NULL;
    uint64_t w = s->w, room = s->size - (w - s->r);
    uint64_t n = (uint64_t)len < room ? (uint64_t)len : room;  // (full: the excess is dropped)
    for (uint64_t i = 0; i < n;) {
        uint32_t at = (uint32_t)((w + i) % s->size), piece = s->size - at;
        if (piece > n - i) piece = (uint32_t)(n - i);
        memcpy(s->ring + at, (const uint8_t *)buf + i, piece);
        i += piece;
    }
    s->w = w + n;
    return true;
}

int SDL_GetAudioStreamQueued(SDL_AudioStream *s) { return s ? (int)(s->w - s->r) : 0; }
float SDL_GetAudioStreamGain(SDL_AudioStream *s) { return s ? s->gain : 1.0f; }
bool SDL_SetAudioStreamGain(SDL_AudioStream *s, float g) { if (s) s->gain = g; return s != NULL; }

SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s) {
    for (int i = 0; i < MAX_STREAMS; i++) if (g_streams[i] == s) return (SDL_AudioDeviceID)(i + 1);
    return 0;
}

bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID id, SDL_AudioSpec *spec, int *frames) {
    if (!id || id > MAX_STREAMS || !g_streams[id - 1]) return false;
    SDL_AudioStream *s = g_streams[id - 1];
    if (spec) *spec = s->spec;
    if (frames) *frames = AAudioStream_getFramesPerBurst(s->as);
    return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    if (!s) return;
    for (int i = 0; i < MAX_STREAMS; i++) if (g_streams[i] == s) g_streams[i] = NULL;
    AAudioStream_requestStop(s->as);
    AAudioStream_close(s->as);
    free(s->ring), free(s);
}
