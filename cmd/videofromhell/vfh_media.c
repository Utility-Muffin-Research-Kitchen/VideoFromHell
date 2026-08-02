#include "vfh_media.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <SDL_image.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

bool vfh_media_file_supported(const char *name) {
    static const char *const extensions[] = {
        ".mp4", ".m4v", ".mkv", ".mov", ".avi", ".webm", ".ts",
        ".m2ts", ".mts", ".mpg", ".mpeg", ".3gp", ".flv",
    };
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        const char *a = dot;
        const char *b = extensions[i];
        while (*a && *b && tolower((unsigned char)*a) == *b) {
            a++;
            b++;
        }
        if (!*a && !*b) return true;
    }
    return false;
}

bool vfh_media_probe_duration(const char *path, double *out_seconds) {
    if (out_seconds) *out_seconds = 0.0;
    AVFormatContext *format = NULL;
    if (!path || avformat_open_input(&format, path, NULL, NULL) < 0) return false;
    bool ok = false;
    if (avformat_find_stream_info(format, NULL) >= 0 &&
        format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        if (out_seconds) *out_seconds = (double)format->duration / AV_TIME_BASE;
        ok = true;
    }
    avformat_close_input(&format);
    return ok;
}

void vfh_media_format_duration(double seconds, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    if (seconds <= 0.0) {
        snprintf(out, (size_t)out_size, "--:--");
        return;
    }
    unsigned total = (unsigned)(seconds + 0.5);
    unsigned hours = total / 3600;
    unsigned minutes = (total / 60) % 60;
    unsigned secs = total % 60;
    if (hours) snprintf(out, (size_t)out_size, "%u:%02u:%02u", hours, minutes, secs);
    else snprintf(out, (size_t)out_size, "%u:%02u", minutes, secs);
}

static SDL_Surface *vfh_surface_from_frame(AVFrame *frame, int max_dimension) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || max_dimension <= 0)
        return NULL;
    int width = frame->width;
    int height = frame->height;
    if (width > max_dimension || height > max_dimension) {
        if (width >= height) {
            height = (height * max_dimension) / width;
            width = max_dimension;
        } else {
            width = (width * max_dimension) / height;
            height = max_dimension;
        }
    }
    if (width < 1 || height < 1) return NULL;
    struct SwsContext *scale = sws_getContext(
        frame->width, frame->height, (enum AVPixelFormat)frame->format,
        width, height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale) return NULL;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 24, SDL_PIXELFORMAT_RGB24);
    if (!surface) {
        sws_freeContext(scale);
        return NULL;
    }
    uint8_t *destination[] = { (uint8_t *)surface->pixels, NULL, NULL, NULL };
    int destination_stride[] = { surface->pitch, 0, 0, 0 };
    int rows = sws_scale(scale, (const uint8_t *const *)frame->data, frame->linesize,
                         0, frame->height, destination, destination_stride);
    sws_freeContext(scale);
    if (rows != height) {
        SDL_FreeSurface(surface);
        return NULL;
    }
    return surface;
}

SDL_Surface *vfh_media_decode_poster(const char *path, int max_dimension) {
    AVFormatContext *format = NULL;
    AVCodecContext *codec_context = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    SDL_Surface *poster = NULL;
    AVCodec *codec = NULL;
    int video_stream = -1;

    if (!path || avformat_open_input(&format, path, NULL, NULL) < 0) goto done;
    if (avformat_find_stream_info(format, NULL) < 0) goto done;
    video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video_stream < 0 || !codec) goto done;
    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context ||
        avcodec_parameters_to_context(codec_context,
                                      format->streams[video_stream]->codecpar) < 0 ||
        avcodec_open2(codec_context, codec, NULL) < 0) goto done;
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !packet) goto done;

    /* Most files expose a keyframe near the start. Bound packet reads so one
       damaged file cannot monopolise the thumbnail worker. */
    for (int reads = 0; reads < 160 && !poster; reads++) {
        if (av_read_frame(format, packet) < 0) break;
        if (packet->stream_index == video_stream &&
            avcodec_send_packet(codec_context, packet) >= 0 &&
            avcodec_receive_frame(codec_context, frame) >= 0) {
            poster = vfh_surface_from_frame(frame, max_dimension);
            av_frame_unref(frame);
        }
        av_packet_unref(packet);
    }

done:
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec_context);
    avformat_close_input(&format);
    return poster;
}
