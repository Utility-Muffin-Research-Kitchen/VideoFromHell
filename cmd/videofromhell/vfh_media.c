#include "vfh_media.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL_image.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
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

static void vfh_media_copy_metadata(AVDictionary *metadata, const char *key,
                                    char *out, size_t out_size) {
    if (!out || out_size == 0 || out[0]) return;
    AVDictionaryEntry *entry = metadata ? av_dict_get(metadata, key, NULL, 0) : NULL;
    if (entry && entry->value && entry->value[0])
        snprintf(out, out_size, "%s", entry->value);
}

static int vfh_media_year_from_metadata(AVDictionary *metadata) {
    static const char *const keys[] = { "year", "date" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        AVDictionaryEntry *entry = metadata ? av_dict_get(metadata, keys[i], NULL, 0) : NULL;
        const char *value = entry ? entry->value : NULL;
        if (!value) continue;
        for (const char *cursor = value; cursor[0] && cursor[1] && cursor[2] && cursor[3]; cursor++) {
            if (!isdigit((unsigned char)cursor[0]) || !isdigit((unsigned char)cursor[1]) ||
                !isdigit((unsigned char)cursor[2]) || !isdigit((unsigned char)cursor[3])) continue;
            int year = (cursor[0] - '0') * 1000 + (cursor[1] - '0') * 100 +
                       (cursor[2] - '0') * 10 + (cursor[3] - '0');
            if (year >= 1800 && year <= 3000) return year;
        }
    }
    return 0;
}

static void vfh_media_copy_codec_name(const AVCodecParameters *parameters,
                                      char *out, size_t out_size) {
    if (!parameters || !out || out_size == 0) return;
    AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (codec && codec->name) snprintf(out, out_size, "%s", codec->name);
}

bool vfh_media_probe_metadata(const char *path, vfh_media_metadata *out_metadata) {
    if (out_metadata) memset(out_metadata, 0, sizeof(*out_metadata));
    AVFormatContext *format = NULL;
    if (!path || avformat_open_input(&format, path, NULL, NULL) < 0) return false;
    bool ok = avformat_find_stream_info(format, NULL) >= 0;
    if (ok && out_metadata) {
        if (format->duration != AV_NOPTS_VALUE && format->duration > 0)
            out_metadata->duration = (double)format->duration / AV_TIME_BASE;
        if (format->iformat && format->iformat->name)
            snprintf(out_metadata->container, sizeof(out_metadata->container), "%s",
                     format->iformat->name);
        vfh_media_copy_metadata(format->metadata, "title", out_metadata->title,
                                sizeof(out_metadata->title));
        out_metadata->year = vfh_media_year_from_metadata(format->metadata);
        for (unsigned i = 0; i < format->nb_streams; i++) {
            AVStream *stream = format->streams[i];
            if (!stream) continue;
            if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC)
                out_metadata->has_embedded_art = true;
            if (!out_metadata->title[0])
                vfh_media_copy_metadata(stream->metadata, "title", out_metadata->title,
                                        sizeof(out_metadata->title));
            if (!out_metadata->year)
                out_metadata->year = vfh_media_year_from_metadata(stream->metadata);
        }
        int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        int audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (video >= 0 && (unsigned)video < format->nb_streams)
            vfh_media_copy_codec_name(format->streams[video]->codecpar,
                                      out_metadata->video_codec,
                                      sizeof(out_metadata->video_codec));
        if (audio >= 0 && (unsigned)audio < format->nb_streams)
            vfh_media_copy_codec_name(format->streams[audio]->codecpar,
                                      out_metadata->audio_codec,
                                      sizeof(out_metadata->audio_codec));
    }
    avformat_close_input(&format);
    return ok;
}

bool vfh_media_probe_duration(const char *path, double *out_seconds) {
    if (out_seconds) *out_seconds = 0.0;
    vfh_media_metadata metadata;
    if (!vfh_media_probe_metadata(path, &metadata) || metadata.duration <= 0.0) return false;
    if (out_seconds) *out_seconds = metadata.duration;
    return true;
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

static bool vfh_media_surface_is_dark(const SDL_Surface *surface) {
    if (!surface || !surface->pixels || surface->w < 1 || surface->h < 1) return true;
    SDL_Surface *mutable_surface = (SDL_Surface *)surface;
    if (SDL_LockSurface(mutable_surface) != 0) return false;
    int step_x = surface->w / 32;
    int step_y = surface->h / 32;
    if (step_x < 1) step_x = 1;
    if (step_y < 1) step_y = 1;
    uint64_t sum = 0, sum_squared = 0, count = 0, bright = 0;
    for (int y = 0; y < surface->h; y += step_y) {
        const uint8_t *row = (const uint8_t *)surface->pixels + y * surface->pitch;
        for (int x = 0; x < surface->w; x += step_x) {
            const uint8_t *pixel = row + x * 3;  /* RGB24 created above. */
            unsigned luma = (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2]) >> 8;
            sum += luma;
            sum_squared += (uint64_t)luma * luma;
            if (luma >= 64) bright++;
            count++;
        }
    }
    SDL_UnlockSurface(mutable_surface);
    if (!count) return true;
    uint64_t mean = sum / count;
    uint64_t variance = sum_squared / count - mean * mean;
    /* Do not reject a deliberately dark scene containing titles or detail;
       this only filters the low-luma, low-variance leader frames. */
    return mean < 32 && variance < 512 && bright * 100 < count * 2;
}

static SDL_Surface *vfh_media_decode_attached_picture(AVFormatContext *format,
                                                       int max_dimension) {
    if (!format) return NULL;
    for (unsigned i = 0; i < format->nb_streams; i++) {
        AVStream *stream = format->streams[i];
        if (!stream || !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
            !stream->codecpar) continue;
        AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : NULL;
        AVFrame *frame = context ? av_frame_alloc() : NULL;
        SDL_Surface *surface = NULL;
        if (context && frame &&
            avcodec_parameters_to_context(context, stream->codecpar) >= 0 &&
            avcodec_open2(context, codec, NULL) >= 0 &&
            avcodec_send_packet(context, &stream->attached_pic) >= 0 &&
            avcodec_receive_frame(context, frame) >= 0)
            surface = vfh_surface_from_frame(frame, max_dimension);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        if (surface) return surface;
    }
    return NULL;
}

static SDL_Surface *vfh_media_decode_frame_near(AVFormatContext *format,
                                                 AVCodecContext *codec_context,
                                                 AVFrame *frame, AVPacket *packet,
                                                 int video_stream, double fraction,
                                                 int max_dimension, int dark_frame_limit,
                                                 bool *out_saw_frame,
                                                 bool *out_saw_dark) {
    if (out_saw_frame) *out_saw_frame = false;
    if (out_saw_dark) *out_saw_dark = false;
    if (!format || !codec_context || !frame || !packet || video_stream < 0) return NULL;
    AVStream *stream = format->streams[video_stream];
    if (!stream || !stream->time_base.num || !stream->time_base.den) return NULL;
    if (fraction > 0.0 && format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        double timestamp = ((double)format->duration * fraction * stream->time_base.den) /
                           ((double)AV_TIME_BASE * stream->time_base.num);
        if (av_seek_frame(format, video_stream, (int64_t)timestamp, AVSEEK_FLAG_BACKWARD) < 0)
            return NULL;
        avcodec_flush_buffers(codec_context);
    }
    int dark_frames = 0;
    for (int reads = 0; reads < 240; reads++) {
        if (av_read_frame(format, packet) < 0) break;
        if (packet->stream_index == video_stream &&
            avcodec_send_packet(codec_context, packet) >= 0) {
            while (avcodec_receive_frame(codec_context, frame) >= 0) {
                if (out_saw_frame) *out_saw_frame = true;
                SDL_Surface *surface = vfh_surface_from_frame(frame, max_dimension);
                av_frame_unref(frame);
                if (!surface) continue;
                if (!vfh_media_surface_is_dark(surface)) {
                    av_packet_unref(packet);
                    return surface;
                }
                if (out_saw_dark) *out_saw_dark = true;
                SDL_FreeSurface(surface);
                if (++dark_frames >= dark_frame_limit) {
                    /* Never let a dark leader monopolise the thumbnail worker. */
                    av_packet_unref(packet);
                    return NULL;
                }
            }
        }
        av_packet_unref(packet);
    }
    return NULL;
}

SDL_Surface *vfh_media_decode_poster(const char *path, int max_dimension,
                                     vfh_media_poster_status *out_status) {
    AVFormatContext *format = NULL;
    AVCodecContext *codec_context = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    SDL_Surface *poster = NULL;
    AVCodec *codec = NULL;
    int video_stream = -1;

    if (out_status) *out_status = VFH_MEDIA_POSTER_ERROR;
    if (!path || avformat_open_input(&format, path, NULL, NULL) < 0) goto done;
    if (avformat_find_stream_info(format, NULL) < 0) goto done;
    poster = vfh_media_decode_attached_picture(format, max_dimension);
    if (poster) {
        if (out_status) *out_status = VFH_MEDIA_POSTER_READY;
        goto done;
    }
    video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video_stream < 0 || !codec) {
        if (out_status) *out_status = VFH_MEDIA_POSTER_NOT_FOUND;
        goto done;
    }
    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context ||
        avcodec_parameters_to_context(codec_context,
                                      format->streams[video_stream]->codecpar) < 0 ||
        avcodec_open2(codec_context, codec, NULL) < 0) goto done;
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !packet) goto done;

    bool saw_frame = false, saw_dark = false;
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        poster = vfh_media_decode_frame_near(format, codec_context, frame, packet,
                                                 video_stream, 0.10, max_dimension, 240,
                                             &saw_frame, &saw_dark);
        if (!poster && saw_dark)
            poster = vfh_media_decode_frame_near(format, codec_context, frame, packet,
                                                 video_stream, 0.25, max_dimension, 240,
                                                 &saw_frame, &saw_dark);
    } else {
        poster = vfh_media_decode_frame_near(format, codec_context, frame, packet,
                                             video_stream, 0.0, max_dimension, 60,
                                             &saw_frame, &saw_dark);
    }
    if (out_status)
        *out_status = poster ? VFH_MEDIA_POSTER_READY
                             : (saw_frame ? VFH_MEDIA_POSTER_NOT_FOUND
                                          : VFH_MEDIA_POSTER_ERROR);

done:
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec_context);
    avformat_close_input(&format);
    return poster;
}
