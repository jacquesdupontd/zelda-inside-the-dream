// Inside The Dream: Port0r's animated logo before the start screen (12/11/2026, Guillaume: « notre nouveau logo avec
// animation au lancement dans le launcher »): the site's port0r-logo-animation (3.6 s, with its sound), decoded with
// the FFmpeg the game already links (as runtime/hle/movie.c) - its pictures as RGBA at the caller's size, one when its
// time comes; its sound decoded whole at the start (a few hundred kilobytes), handed to the caller to play.
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

#include "itd_intro.h"

struct ItdIntro {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    int vs;
    struct SwsContext *sws;
    AVFrame *fr;
    AVPacket *pkt;
    int w, h;
    uint8_t *rgba;
    double next_pts, tb, duration;
    bool have, eof;
};

// the next video frame into in->fr (in->next_pts its time); false at the end
static bool next_frame(ItdIntro *in) {
    for (;;) {
        int r = avcodec_receive_frame(in->dec, in->fr);
        if (r == 0) {
            int64_t p = in->fr->best_effort_timestamp != AV_NOPTS_VALUE ? in->fr->best_effort_timestamp : in->fr->pts;
            in->next_pts = p == AV_NOPTS_VALUE ? in->next_pts + 1.0 / 60 : p * in->tb;
            return true;
        }
        if (r == AVERROR_EOF) return false;
        if (r != AVERROR(EAGAIN)) return false;
        if (av_read_frame(in->fmt, in->pkt) < 0) {
            avcodec_send_packet(in->dec, NULL);  // (drain the decoder's last pictures)
            continue;
        }
        if (in->pkt->stream_index == in->vs) avcodec_send_packet(in->dec, in->pkt);
        av_packet_unref(in->pkt);
    }
}

// the whole sound of the file at path, interleaved float
static void decode_sound(const char *path, float **pcm, int *frames, int *channels, int *rate) {
    *pcm = NULL, *frames = 0, *channels = 0, *rate = 0;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return; }
    int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    const AVCodec *c = as >= 0 ? avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id) : NULL;
    AVCodecContext *dec = c ? avcodec_alloc_context3(c) : NULL;
    if (!dec || avcodec_parameters_to_context(dec, fmt->streams[as]->codecpar) < 0 || avcodec_open2(dec, c, NULL) < 0) {
        if (dec) avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    size_t cap = 0, n = 0;
    float *out = NULL;
    int ch = 0;
    bool flushing = false;
    for (;;) {
        int r = avcodec_receive_frame(dec, fr);
        if (r == 0) {
            ch = fr->ch_layout.nb_channels;
            if (ch < 1 || ch > 8) break;
            if (n + (size_t)fr->nb_samples > cap) {
                cap = (n + fr->nb_samples) * 2;
                float *g = realloc(out, cap * ch * sizeof(float));
                if (!g) break;
                out = g;
            }
            for (int i = 0; i < fr->nb_samples; i++)
                for (int k = 0; k < ch; k++) {
                    float v = 0;
                    switch (fr->format) {
                    case AV_SAMPLE_FMT_FLTP: v = ((const float *)fr->extended_data[k])[i]; break;
                    case AV_SAMPLE_FMT_FLT: v = ((const float *)fr->extended_data[0])[i * ch + k]; break;
                    case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)fr->extended_data[k])[i] / 32768.0f; break;
                    case AV_SAMPLE_FMT_S16: v = ((const int16_t *)fr->extended_data[0])[i * ch + k] / 32768.0f; break;
                    default: break;
                    }
                    out[(n + i) * ch + k] = v;
                }
            n += fr->nb_samples;
            *rate = fr->sample_rate;
            continue;
        }
        if (r == AVERROR_EOF) break;
        if (r != AVERROR(EAGAIN)) break;
        if (flushing) break;
        if (av_read_frame(fmt, pkt) < 0) { avcodec_send_packet(dec, NULL); flushing = true; continue; }
        if (pkt->stream_index == as) avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    if (!n) { free(out); return; }
    *pcm = out, *frames = (int)n, *channels = ch;
}

ItdIntro *itd_intro_open(const char *path, int w, int h, float **pcm, int *frames, int *channels, int *rate) {
    *pcm = NULL, *frames = 0, *channels = 0, *rate = 0;
    ItdIntro *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    if (avformat_open_input(&in->fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(in->fmt, NULL) < 0) goto fail;
    in->vs = av_find_best_stream(in->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (in->vs < 0) goto fail;
    AVStream *st = in->fmt->streams[in->vs];
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c || !(in->dec = avcodec_alloc_context3(c))) goto fail;
    if (avcodec_parameters_to_context(in->dec, st->codecpar) < 0 || avcodec_open2(in->dec, c, NULL) < 0) goto fail;
    in->tb = av_q2d(st->time_base);
    in->duration = in->fmt->duration > 0 ? in->fmt->duration / (double)AV_TIME_BASE : 3.6;
    in->w = w, in->h = h;
    in->rgba = malloc((size_t)w * h * 4);
    in->fr = av_frame_alloc();
    in->pkt = av_packet_alloc();
    if (!in->rgba || !in->fr || !in->pkt) goto fail;
    in->have = next_frame(in);
    if (!in->have) goto fail;
    decode_sound(path, pcm, frames, channels, rate);
    return in;
fail:
    itd_intro_close(in);
    return NULL;
}

const uint8_t *itd_intro_picture(ItdIntro *in, double t) {
    bool got = false;
    while (in->have && in->next_pts <= t) {
        in->sws = sws_getCachedContext(in->sws, in->fr->width, in->fr->height, in->fr->format, in->w, in->h, AV_PIX_FMT_RGBA,
                                       SWS_BILINEAR, NULL, NULL, NULL);
        if (in->sws) {
            uint8_t *dst[4] = {in->rgba, NULL, NULL, NULL};
            int ls[4] = {in->w * 4, 0, 0, 0};
            sws_scale(in->sws, (const uint8_t *const *)in->fr->data, in->fr->linesize, 0, in->fr->height, dst, ls);
            got = true;
        }
        in->have = next_frame(in);
        if (!in->have) in->eof = true;
    }
    return got ? in->rgba : NULL;
}

double itd_intro_duration(const ItdIntro *in) { return in->duration; }
bool itd_intro_ended(const ItdIntro *in) { return in->eof; }

void itd_intro_close(ItdIntro *in) {
    if (!in) return;
    if (in->sws) sws_freeContext(in->sws);
    if (in->fr) av_frame_free(&in->fr);
    if (in->pkt) av_packet_free(&in->pkt);
    if (in->dec) avcodec_free_context(&in->dec);
    if (in->fmt) avformat_close_input(&in->fmt);
    free(in->rgba);
    free(in);
}
