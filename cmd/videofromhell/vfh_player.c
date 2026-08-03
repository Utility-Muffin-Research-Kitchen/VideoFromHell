#include "vfh_player.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <SDL.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "vfh_mpp.h"

#define VFH_PACKET_QUEUE_BYTES (2u * 1024u * 1024u)
#define VFH_PACKET_QUEUE_ITEMS 96
#define VFH_FRAME_QUEUE_ITEMS 6

typedef struct vfh_packet_item {
    AVPacket *packet;
    unsigned long generation;
    bool eof;
    struct vfh_packet_item *next;
} vfh_packet_item;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    vfh_packet_item *head;
    vfh_packet_item *tail;
    size_t bytes;
    int count;
    bool closed;
} vfh_packet_queue;

typedef struct vfh_frame_item {
    AVFrame *frame;
    double pts;
    unsigned long generation;
    struct vfh_frame_item *next;
} vfh_frame_item;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    vfh_frame_item *head;
    vfh_frame_item *tail;
    int count;
    bool closed;
} vfh_frame_queue;

struct vfh_player {
    AVFormatContext *format;
    AVCodecContext *video_codec;
    AVCodecContext *audio_codec;
    vfh_mpp_decoder *mpp;
    SwrContext *resampler;
    int video_stream;
    int audio_stream;
    AVRational video_time_base;
    AVRational audio_time_base;
    unsigned audio_channels;
    unsigned audio_rate;
    double duration;

    vfh_packet_queue video_packets;
    vfh_packet_queue audio_packets;
    vfh_frame_queue video_frames;

    pthread_t demux_thread;
    pthread_t video_thread;
    pthread_t audio_thread;
    bool demux_thread_started;
    bool video_thread_started;
    bool audio_thread_started;
    snd_pcm_t *pcm;
    bool current_bluetooth;
    atomic_bool desired_bluetooth;
    atomic_bool audio_reopen_pending;
    pthread_mutex_t audio_notice_mutex;
    char audio_notice[128];

    atomic_bool stop;
    atomic_bool paused;
    atomic_bool demux_eof;
    atomic_bool video_done;
    atomic_bool audio_done;
    atomic_ulong generation;

    pthread_mutex_t state_mutex;
    double clock_seconds;
    bool clock_valid;
    uint32_t wall_anchor_ms;
    double wall_anchor_seconds;
    double paused_seconds;

    pthread_mutex_t seek_mutex;
    bool seek_pending;
    double seek_target;
};

static bool vfh_reopen_audio_output(vfh_player *player);

static void vfh_packet_item_free(vfh_packet_item *item) {
    if (!item) return;
    av_packet_free(&item->packet);
    free(item);
}

static void vfh_packet_queue_init(vfh_packet_queue *queue) {
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

static void vfh_packet_queue_clear(vfh_packet_queue *queue) {
    pthread_mutex_lock(&queue->mutex);
    vfh_packet_item *item = queue->head;
    queue->head = queue->tail = NULL;
    queue->bytes = 0;
    queue->count = 0;
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    while (item) {
        vfh_packet_item *next = item->next;
        vfh_packet_item_free(item);
        item = next;
    }
}

static void vfh_packet_queue_close(vfh_packet_queue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

static void vfh_packet_queue_destroy(vfh_packet_queue *queue) {
    vfh_packet_queue_clear(queue);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->mutex);
}

static bool vfh_packet_queue_put(vfh_packet_queue *queue, AVPacket *packet,
                                 unsigned long generation, bool eof) {
    size_t bytes = packet && packet->size > 0 ? (size_t)packet->size : 0;
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed &&
           (queue->count >= VFH_PACKET_QUEUE_ITEMS ||
            (queue->count > 0 && queue->bytes + bytes > VFH_PACKET_QUEUE_BYTES)))
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    if (queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        av_packet_free(&packet);
        return false;
    }
    vfh_packet_item *item = calloc(1, sizeof(*item));
    if (!item) {
        pthread_mutex_unlock(&queue->mutex);
        av_packet_free(&packet);
        return false;
    }
    item->packet = packet;
    item->generation = generation;
    item->eof = eof;
    if (queue->tail) queue->tail->next = item;
    else queue->head = item;
    queue->tail = item;
    queue->bytes += bytes;
    queue->count++;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static bool vfh_packet_queue_get(vfh_packet_queue *queue, vfh_packet_item **out) {
    *out = NULL;
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed && !queue->head)
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    if (!queue->head) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    vfh_packet_item *item = queue->head;
    queue->head = item->next;
    if (!queue->head) queue->tail = NULL;
    queue->bytes -= item->packet && item->packet->size > 0 ? (size_t)item->packet->size : 0;
    queue->count--;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    item->next = NULL;
    *out = item;
    return true;
}

static void vfh_frame_queue_init(vfh_frame_queue *queue) {
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

static void vfh_frame_item_free(vfh_frame_item *item) {
    if (!item) return;
    av_frame_free(&item->frame);
    free(item);
}

static void vfh_frame_queue_clear(vfh_frame_queue *queue) {
    pthread_mutex_lock(&queue->mutex);
    vfh_frame_item *item = queue->head;
    queue->head = queue->tail = NULL;
    queue->count = 0;
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    while (item) {
        vfh_frame_item *next = item->next;
        vfh_frame_item_free(item);
        item = next;
    }
}

static void vfh_frame_queue_close(vfh_frame_queue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

static void vfh_frame_queue_destroy(vfh_frame_queue *queue) {
    vfh_frame_queue_clear(queue);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->mutex);
}

static bool vfh_frame_queue_put(vfh_frame_queue *queue, AVFrame *frame,
                                double pts, unsigned long generation) {
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed && queue->count >= VFH_FRAME_QUEUE_ITEMS)
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    if (queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        av_frame_free(&frame);
        return false;
    }
    vfh_frame_item *item = calloc(1, sizeof(*item));
    if (!item) {
        pthread_mutex_unlock(&queue->mutex);
        av_frame_free(&frame);
        return false;
    }
    item->frame = frame;
    item->pts = pts;
    item->generation = generation;
    if (queue->tail) queue->tail->next = item;
    else queue->head = item;
    queue->tail = item;
    queue->count++;
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static bool vfh_frame_queue_empty(const vfh_frame_queue *queue) {
    vfh_frame_queue *mutable_queue = (vfh_frame_queue *)queue;
    pthread_mutex_lock(&mutable_queue->mutex);
    bool empty = mutable_queue->count == 0;
    pthread_mutex_unlock(&mutable_queue->mutex);
    return empty;
}

static double vfh_seconds_from_timestamp(int64_t timestamp, AVRational time_base,
                                         double fallback) {
    if (timestamp == AV_NOPTS_VALUE || time_base.den == 0) return fallback;
    return (double)timestamp * (double)time_base.num / (double)time_base.den;
}

static void vfh_set_clock(vfh_player *player, double seconds, bool valid) {
    pthread_mutex_lock(&player->state_mutex);
    player->clock_seconds = seconds < 0.0 ? 0.0 : seconds;
    player->clock_valid = valid;
    pthread_mutex_unlock(&player->state_mutex);
}

static double vfh_clock(vfh_player *player) {
    pthread_mutex_lock(&player->state_mutex);
    double seconds;
    if (atomic_load(&player->paused)) seconds = player->paused_seconds;
    else if (player->audio_stream >= 0 && player->clock_valid) seconds = player->clock_seconds;
    else seconds = player->wall_anchor_seconds +
                   (double)(SDL_GetTicks() - player->wall_anchor_ms) / 1000.0;
    pthread_mutex_unlock(&player->state_mutex);
    if (player->duration > 0.0 && seconds > player->duration) seconds = player->duration;
    return seconds < 0.0 ? 0.0 : seconds;
}

static void vfh_set_wall_anchor(vfh_player *player, double seconds) {
    pthread_mutex_lock(&player->state_mutex);
    player->wall_anchor_seconds = seconds;
    player->wall_anchor_ms = SDL_GetTicks();
    pthread_mutex_unlock(&player->state_mutex);
}

static void vfh_reset_for_seek(vfh_player *player, double seconds) {
    vfh_packet_queue_clear(&player->video_packets);
    vfh_packet_queue_clear(&player->audio_packets);
    vfh_frame_queue_clear(&player->video_frames);
    atomic_store(&player->demux_eof, false);
    atomic_store(&player->video_done, false);
    atomic_store(&player->audio_done, player->audio_stream < 0);
    vfh_set_clock(player, seconds, player->audio_stream < 0);
    vfh_set_wall_anchor(player, seconds);
}

static bool vfh_take_seek(vfh_player *player, double *target) {
    pthread_mutex_lock(&player->seek_mutex);
    if (!player->seek_pending) {
        pthread_mutex_unlock(&player->seek_mutex);
        return false;
    }
    *target = player->seek_target;
    player->seek_pending = false;
    pthread_mutex_unlock(&player->seek_mutex);
    return true;
}

static AVFrame *vfh_video_frame_yuv420(AVFrame *source) {
    if (source->format == AV_PIX_FMT_YUV420P || source->format == AV_PIX_FMT_YUVJ420P)
        return av_frame_clone(source);
    AVFrame *converted = av_frame_alloc();
    if (!converted) return NULL;
    converted->format = AV_PIX_FMT_YUV420P;
    converted->width = source->width;
    converted->height = source->height;
    if (av_frame_get_buffer(converted, 32) < 0) {
        av_frame_free(&converted);
        return NULL;
    }
    struct SwsContext *scale = sws_getContext(source->width, source->height,
                                               (enum AVPixelFormat)source->format,
                                               converted->width, converted->height,
                                               AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                               NULL, NULL, NULL);
    if (!scale) {
        av_frame_free(&converted);
        return NULL;
    }
    int rows = sws_scale(scale, (const uint8_t *const *)source->data, source->linesize,
                         0, source->height, converted->data, converted->linesize);
    sws_freeContext(scale);
    if (rows != converted->height) {
        av_frame_free(&converted);
        return NULL;
    }
    return converted;
}

static void vfh_video_receive(vfh_player *player, AVFrame *decoded,
                              unsigned long generation, double *last_pts) {
    while (!atomic_load(&player->stop)) {
        int result = avcodec_receive_frame(player->video_codec, decoded);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) break;
        double pts = vfh_seconds_from_timestamp(decoded->best_effort_timestamp,
                                                player->video_time_base, *last_pts + 1.0 / 30.0);
        *last_pts = pts;
        AVFrame *output = vfh_video_frame_yuv420(decoded);
        av_frame_unref(decoded);
        if (!output) continue;
        if (generation != atomic_load(&player->generation)) {
            av_frame_free(&output);
            continue;
        }
        if (!vfh_frame_queue_put(&player->video_frames, output, pts, generation)) break;
    }
}

static bool vfh_video_receive_mpp(vfh_player *player, unsigned long generation,
                                  double *last_pts) {
    for (;;) {
        AVFrame *output = NULL;
        int64_t packet_pts = AV_NOPTS_VALUE;
        int result = vfh_mpp_decoder_receive_frame(player->mpp, &output, &packet_pts);
        if (result < 0) return false;
        if (result == 0) return true;
        double pts = vfh_seconds_from_timestamp(packet_pts, player->video_time_base,
                                                *last_pts + 1.0 / 30.0);
        *last_pts = pts;
        if (generation != atomic_load(&player->generation)) {
            av_frame_free(&output);
            continue;
        }
        if (!vfh_frame_queue_put(&player->video_frames, output, pts, generation))
            return false;
    }
}

static void *vfh_mpp_video_thread_main(vfh_player *player) {
    unsigned long local_generation = atomic_load(&player->generation);
    double last_pts = 0.0;
    bool eos_sent = false;
    while (!atomic_load(&player->stop)) {
        if (atomic_load(&player->paused)) {
            usleep(10000);
            continue;
        }
        unsigned long current = atomic_load(&player->generation);
        if (current != local_generation) {
            local_generation = current;
            last_pts = vfh_player_position(player);
            eos_sent = false;
            if (!vfh_mpp_decoder_reset(player->mpp)) break;
        }
        if (eos_sent) {
            if (!vfh_video_receive_mpp(player, local_generation, &last_pts)) break;
            if (vfh_mpp_decoder_reached_eos(player->mpp)) {
                atomic_store(&player->video_done, true);
                usleep(10000);
            } else {
                usleep(1000);
            }
            continue;
        }

        vfh_packet_item *item = NULL;
        if (!vfh_packet_queue_get(&player->video_packets, &item)) break;
        if (item->generation != local_generation) {
            vfh_packet_item_free(item);
            continue;
        }
        bool accepted = vfh_mpp_decoder_send_packet(player->mpp,
                                                    item->eof ? NULL : item->packet);
        if (item->eof) eos_sent = accepted;
        vfh_packet_item_free(item);
        if (!accepted || !vfh_video_receive_mpp(player, local_generation, &last_pts)) break;
    }
    atomic_store(&player->video_done, true);
    return NULL;
}

static void *vfh_video_thread_main(void *opaque) {
    vfh_player *player = opaque;
    if (player->mpp) return vfh_mpp_video_thread_main(player);
    AVFrame *decoded = av_frame_alloc();
    if (!decoded) {
        atomic_store(&player->video_done, true);
        return NULL;
    }
    unsigned long local_generation = atomic_load(&player->generation);
    double last_pts = 0.0;
    while (!atomic_load(&player->stop)) {
        if (atomic_load(&player->paused)) {
            usleep(10000);
            continue;
        }
        unsigned long current = atomic_load(&player->generation);
        if (current != local_generation) {
            local_generation = current;
            last_pts = vfh_player_position(player);
            avcodec_flush_buffers(player->video_codec);
        }
        vfh_packet_item *item = NULL;
        if (!vfh_packet_queue_get(&player->video_packets, &item)) break;
        if (item->generation != local_generation) {
            vfh_packet_item_free(item);
            continue;
        }
        if (item->eof) {
            avcodec_send_packet(player->video_codec, NULL);
            vfh_video_receive(player, decoded, local_generation, &last_pts);
            atomic_store(&player->video_done, true);
            vfh_packet_item_free(item);
            continue;
        }
        int sent = avcodec_send_packet(player->video_codec, item->packet);
        vfh_packet_item_free(item);
        if (sent >= 0) vfh_video_receive(player, decoded, local_generation, &last_pts);
    }
    av_frame_free(&decoded);
    return NULL;
}

static bool vfh_audio_write(vfh_player *player, const int16_t *samples,
                            int frames, double frame_pts, unsigned long generation) {
    int offset = 0;
    while (offset < frames && !atomic_load(&player->stop)) {
        if (generation != atomic_load(&player->generation)) return false;
        (void)vfh_reopen_audio_output(player);
        if (!player->pcm) return false;
        snd_pcm_sframes_t written = snd_pcm_writei(player->pcm,
                                                    samples + (size_t)offset * player->audio_channels,
                                                    (snd_pcm_uframes_t)(frames - offset));
        if (written < 0) {
            written = snd_pcm_recover(player->pcm, (int)written, 1);
            if (written < 0) return false;
            continue;
        }
        offset += (int)written;
        snd_pcm_sframes_t delay = 0;
        double end = frame_pts + (double)offset / (double)player->audio_rate;
        if (snd_pcm_delay(player->pcm, &delay) == 0)
            end -= (double)delay / (double)player->audio_rate;
        vfh_set_clock(player, end, true);
    }
    return offset == frames;
}

static void vfh_audio_receive(vfh_player *player, AVFrame *decoded,
                              unsigned long generation, double *last_pts) {
    while (!atomic_load(&player->stop)) {
        int result = avcodec_receive_frame(player->audio_codec, decoded);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) break;
        double pts = vfh_seconds_from_timestamp(decoded->best_effort_timestamp,
                                                player->audio_time_base, *last_pts);
        int capacity = decoded->nb_samples + 64;
        int16_t *samples = calloc((size_t)capacity * player->audio_channels, sizeof(*samples));
        uint8_t *output_planes[] = { (uint8_t *)samples };
        int output = samples ? swr_convert(player->resampler, output_planes, capacity,
                                           (const uint8_t **)decoded->extended_data,
                                           decoded->nb_samples) : 0;
        av_frame_unref(decoded);
        if (output > 0 && generation == atomic_load(&player->generation)) {
            (void)vfh_audio_write(player, samples, output, pts, generation);
            *last_pts = pts + (double)output / (double)player->audio_rate;
        }
        free(samples);
    }
}

static void *vfh_audio_thread_main(void *opaque) {
    vfh_player *player = opaque;
    AVFrame *decoded = av_frame_alloc();
    if (!decoded) {
        atomic_store(&player->audio_done, true);
        return NULL;
    }
    unsigned long local_generation = atomic_load(&player->generation);
    double last_pts = 0.0;
    bool pcm_paused = false;
    while (!atomic_load(&player->stop)) {
        if (atomic_load(&player->paused)) {
            bool reopened = vfh_reopen_audio_output(player);
            if (!pcm_paused) {
                snd_pcm_drop(player->pcm);
                pcm_paused = true;
            } else if (reopened) {
                /* The replacement starts prepared; keep pause semantics across
                   a live route change rather than emitting a brief audio burst. */
                snd_pcm_drop(player->pcm);
            }
            usleep(10000);
            continue;
        }
        if (pcm_paused) {
            snd_pcm_prepare(player->pcm);
            pcm_paused = false;
        }
        (void)vfh_reopen_audio_output(player);
        unsigned long current = atomic_load(&player->generation);
        if (current != local_generation) {
            local_generation = current;
            last_pts = vfh_player_position(player);
            avcodec_flush_buffers(player->audio_codec);
            snd_pcm_drop(player->pcm);
            snd_pcm_prepare(player->pcm);
        }
        vfh_packet_item *item = NULL;
        if (!vfh_packet_queue_get(&player->audio_packets, &item)) break;
        if (item->generation != local_generation) {
            vfh_packet_item_free(item);
            continue;
        }
        if (item->eof) {
            avcodec_send_packet(player->audio_codec, NULL);
            vfh_audio_receive(player, decoded, local_generation, &last_pts);
            snd_pcm_drain(player->pcm);
            vfh_set_clock(player, last_pts, true);
            atomic_store(&player->audio_done, true);
            vfh_packet_item_free(item);
            continue;
        }
        int sent = avcodec_send_packet(player->audio_codec, item->packet);
        vfh_packet_item_free(item);
        if (sent >= 0) vfh_audio_receive(player, decoded, local_generation, &last_pts);
    }
    av_frame_free(&decoded);
    return NULL;
}

static void vfh_demux_push_eof(vfh_player *player, unsigned long generation) {
    (void)vfh_packet_queue_put(&player->video_packets, NULL, generation, true);
    if (player->audio_stream >= 0)
        (void)vfh_packet_queue_put(&player->audio_packets, NULL, generation, true);
}

static void *vfh_demux_thread_main(void *opaque) {
    vfh_player *player = opaque;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return NULL;
    unsigned long local_generation = atomic_load(&player->generation);
    bool eof_sent = false;
    while (!atomic_load(&player->stop)) {
        double seek_target = 0.0;
        if (vfh_take_seek(player, &seek_target)) {
            (void)av_seek_frame(player->format, -1,
                                (int64_t)(seek_target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
            local_generation = atomic_load(&player->generation);
            eof_sent = false;
            continue;
        }
        if (atomic_load(&player->paused)) {
            usleep(10000);
            continue;
        }
        if (eof_sent) {
            usleep(10000);
            continue;
        }
        int read = av_read_frame(player->format, packet);
        if (read < 0) {
            vfh_demux_push_eof(player, local_generation);
            atomic_store(&player->demux_eof, true);
            eof_sent = true;
            continue;
        }
        vfh_packet_queue *queue = NULL;
        if (packet->stream_index == player->video_stream) queue = &player->video_packets;
        else if (packet->stream_index == player->audio_stream) queue = &player->audio_packets;
        if (queue) {
            AVPacket *copy = av_packet_clone(packet);
            if (copy) (void)vfh_packet_queue_put(queue, copy, local_generation, false);
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return NULL;
}

static bool vfh_open_decoder(AVFormatContext *format, int stream_index,
                             AVCodecContext **out_context) {
    *out_context = NULL;
    if (stream_index < 0) return true;
    AVCodec *codec = avcodec_find_decoder(format->streams[stream_index]->codecpar->codec_id);
    if (!codec) return false;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context ||
        avcodec_parameters_to_context(context, format->streams[stream_index]->codecpar) < 0 ||
        avcodec_open2(context, codec, NULL) < 0) {
        avcodec_free_context(&context);
        return false;
    }
    *out_context = context;
    return true;
}

static bool vfh_output_is_bluetooth(const char *output) {
    return output && strcasecmp(output, "BLUETOOTH") == 0;
}

static bool vfh_open_pcm(vfh_player *player, bool bluetooth, snd_pcm_t **out_pcm) {
    *out_pcm = NULL;
    const char *device = bluetooth ? "bluealsa" : "default";
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0) < 0) return false;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                           player->audio_channels, player->audio_rate, 1, 200000) < 0) {
        snd_pcm_close(pcm);
        return false;
    }
    *out_pcm = pcm;
    return true;
}

static void vfh_set_audio_notice(vfh_player *player, const char *notice) {
    pthread_mutex_lock(&player->audio_notice_mutex);
    snprintf(player->audio_notice, sizeof(player->audio_notice), "%s", notice ? notice : "");
    pthread_mutex_unlock(&player->audio_notice_mutex);
}

/* The audio thread owns player->pcm while playback is active.  It is therefore
 * the only context allowed to replace it, avoiding races with snd_pcm_writei
 * while the UI applies live Jawaka status updates. */
static bool vfh_reopen_audio_output(vfh_player *player) {
    if (!atomic_exchange(&player->audio_reopen_pending, false)) return false;
    bool desired_bluetooth = atomic_load(&player->desired_bluetooth);
    if (desired_bluetooth == player->current_bluetooth) return false;

    snd_pcm_t *replacement = NULL;
    bool actual_bluetooth = desired_bluetooth;
    if (!vfh_open_pcm(player, desired_bluetooth, &replacement)) {
        actual_bluetooth = false;
        if (!desired_bluetooth || !vfh_open_pcm(player, false, &replacement)) {
            vfh_set_audio_notice(player, desired_bluetooth
                                 ? "Bluetooth output is unavailable; keeping the current output."
                                 : "System audio output is unavailable; keeping the current output.");
            return false;
        }
        vfh_set_audio_notice(player, "Bluetooth output is unavailable; using system output.");
    }
    if (player->pcm) {
        snd_pcm_drop(player->pcm);
        snd_pcm_close(player->pcm);
    }
    player->pcm = replacement;
    player->current_bluetooth = actual_bluetooth;
    return true;
}

static bool vfh_open_audio_output(vfh_player *player) {
    if (player->audio_stream < 0) return true;
    player->audio_channels = (unsigned)player->audio_codec->channels;
    player->audio_rate = (unsigned)player->audio_codec->sample_rate;
    if (!player->audio_channels || !player->audio_rate) return false;
    int64_t layout = player->audio_codec->channel_layout
                   ? (int64_t)player->audio_codec->channel_layout
                   : av_get_default_channel_layout(player->audio_codec->channels);
    player->resampler = swr_alloc_set_opts(NULL, layout, AV_SAMPLE_FMT_S16,
                                           (int)player->audio_rate, layout,
                                           player->audio_codec->sample_fmt,
                                           (int)player->audio_rate, 0, NULL);
    if (!player->resampler || swr_init(player->resampler) < 0) return false;
    bool desired_bluetooth = atomic_load(&player->desired_bluetooth);
    if (!vfh_open_pcm(player, desired_bluetooth, &player->pcm)) {
        if (!desired_bluetooth || !vfh_open_pcm(player, false, &player->pcm)) return false;
        vfh_set_audio_notice(player, "Bluetooth output is unavailable; using system output.");
        player->current_bluetooth = false;
    } else {
        player->current_bluetooth = desired_bluetooth;
    }
    return true;
}

vfh_player *vfh_player_create(void) {
    vfh_player *player = calloc(1, sizeof(*player));
    if (!player) return NULL;
    player->video_stream = -1;
    player->audio_stream = -1;
    atomic_init(&player->stop, false);
    atomic_init(&player->paused, false);
    atomic_init(&player->demux_eof, false);
    atomic_init(&player->video_done, false);
    atomic_init(&player->audio_done, true);
    atomic_init(&player->generation, 1);
    const char *output = getenv("JAWAKA_AUDIO_OUTPUT");
    if (!output || !output[0]) output = getenv("UMRK_AUDIO_OUTPUT");
    atomic_init(&player->desired_bluetooth, vfh_output_is_bluetooth(output));
    atomic_init(&player->audio_reopen_pending, false);
    pthread_mutex_init(&player->state_mutex, NULL);
    pthread_mutex_init(&player->seek_mutex, NULL);
    pthread_mutex_init(&player->audio_notice_mutex, NULL);
    vfh_packet_queue_init(&player->video_packets);
    vfh_packet_queue_init(&player->audio_packets);
    vfh_frame_queue_init(&player->video_frames);
    return player;
}

void vfh_player_close(vfh_player *player) {
    if (!player) return;
    atomic_store(&player->stop, true);
    vfh_packet_queue_close(&player->video_packets);
    vfh_packet_queue_close(&player->audio_packets);
    vfh_frame_queue_close(&player->video_frames);
    if (player->demux_thread_started) pthread_join(player->demux_thread, NULL);
    if (player->video_thread_started) pthread_join(player->video_thread, NULL);
    if (player->audio_thread_started) pthread_join(player->audio_thread, NULL);
    player->demux_thread_started = player->video_thread_started = player->audio_thread_started = false;
    if (player->pcm) {
        snd_pcm_drop(player->pcm);
        snd_pcm_close(player->pcm);
        player->pcm = NULL;
    }
    swr_free(&player->resampler);
    avcodec_free_context(&player->audio_codec);
    avcodec_free_context(&player->video_codec);
    vfh_mpp_decoder_close(&player->mpp);
    avformat_close_input(&player->format);
}

void vfh_player_destroy(vfh_player *player) {
    if (!player) return;
    vfh_player_close(player);
    vfh_packet_queue_destroy(&player->video_packets);
    vfh_packet_queue_destroy(&player->audio_packets);
    vfh_frame_queue_destroy(&player->video_frames);
    pthread_mutex_destroy(&player->seek_mutex);
    pthread_mutex_destroy(&player->state_mutex);
    pthread_mutex_destroy(&player->audio_notice_mutex);
    free(player);
}

bool vfh_player_open(vfh_player *player, const char *path, char *error, int error_size) {
    if (error && error_size > 0) error[0] = '\0';
    if (!player || !path || !path[0]) return false;
    av_log_set_level(AV_LOG_QUIET);
    if (avformat_open_input(&player->format, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(player->format, NULL) < 0) goto failed;
    player->video_stream = av_find_best_stream(player->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    player->audio_stream = av_find_best_stream(player->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (player->video_stream < 0 ||
        (!vfh_mpp_decoder_open(&player->mpp,
                               player->format->streams[player->video_stream]->codecpar,
                               player->format->streams[player->video_stream]->time_base) &&
         !vfh_open_decoder(player->format, player->video_stream, &player->video_codec))) goto failed;
    if (player->mpp)
        fprintf(stderr, "videofromhell: using direct MPP video decode\n");
    player->video_time_base = player->format->streams[player->video_stream]->time_base;
    if (player->audio_stream >= 0)
        player->audio_time_base = player->format->streams[player->audio_stream]->time_base;
    if (player->format->duration > 0 && player->format->duration != AV_NOPTS_VALUE)
        player->duration = (double)player->format->duration / AV_TIME_BASE;
    /* Audio is best-effort: an unsupported audio codec or a busy ALSA device
       must not make an otherwise-playable video refuse to open. Drop the audio
       stream and run video-only off the wall clock instead. */
    if (player->audio_stream >= 0 &&
        (!vfh_open_decoder(player->format, player->audio_stream, &player->audio_codec) ||
         !vfh_open_audio_output(player))) {
        fprintf(stderr, "videofromhell: audio unavailable; playing video only\n");
        if (player->pcm) { snd_pcm_close(player->pcm); player->pcm = NULL; }
        swr_free(&player->resampler);
        avcodec_free_context(&player->audio_codec);
        player->audio_stream = -1;
    }
    atomic_store(&player->audio_done, player->audio_stream < 0);
    vfh_set_wall_anchor(player, 0.0);
    vfh_set_clock(player, 0.0, player->audio_stream < 0);
    if (pthread_create(&player->video_thread, NULL, vfh_video_thread_main, player) != 0) goto failed;
    player->video_thread_started = true;
    if (player->audio_stream >= 0) {
        if (pthread_create(&player->audio_thread, NULL, vfh_audio_thread_main, player) != 0) goto failed;
        player->audio_thread_started = true;
    }
    if (pthread_create(&player->demux_thread, NULL, vfh_demux_thread_main, player) != 0) goto failed;
    player->demux_thread_started = true;
    return true;

failed:
    if (error && error_size > 0)
        snprintf(error, (size_t)error_size, "Unable to open playable audio/video streams.");
    vfh_player_close(player);
    return false;
}

void vfh_player_set_paused(vfh_player *player, bool paused) {
    if (!player || atomic_load(&player->stop)) return;
    if (paused) {
        double position = vfh_clock(player);
        pthread_mutex_lock(&player->state_mutex);
        player->paused_seconds = position;
        pthread_mutex_unlock(&player->state_mutex);
        atomic_store(&player->paused, true);
    } else {
        atomic_store(&player->paused, false);
        vfh_set_wall_anchor(player, vfh_clock(player));
    }
}

bool vfh_player_is_paused(const vfh_player *player) {
    return player && atomic_load(&player->paused);
}

void vfh_player_set_audio_output(vfh_player *player, const char *output) {
    if (!player) return;
    bool bluetooth = vfh_output_is_bluetooth(output);
    if (atomic_exchange(&player->desired_bluetooth, bluetooth) != bluetooth)
        atomic_store(&player->audio_reopen_pending, true);
}

bool vfh_player_take_audio_notice(vfh_player *player, char *out, int out_size) {
    if (!player || !out || out_size < 1) return false;
    pthread_mutex_lock(&player->audio_notice_mutex);
    bool has_notice = player->audio_notice[0] != '\0';
    if (has_notice) {
        snprintf(out, (size_t)out_size, "%s", player->audio_notice);
        player->audio_notice[0] = '\0';
    } else {
        out[0] = '\0';
    }
    pthread_mutex_unlock(&player->audio_notice_mutex);
    return has_notice;
}

void vfh_player_seek_relative(vfh_player *player, double seconds) {
    if (!player || atomic_load(&player->stop)) return;
    double target = vfh_clock(player) + seconds;
    if (target < 0.0) target = 0.0;
    if (player->duration > 0.0 && target > player->duration) target = player->duration;
    pthread_mutex_lock(&player->seek_mutex);
    player->seek_target = target;
    player->seek_pending = true;
    pthread_mutex_unlock(&player->seek_mutex);
    atomic_fetch_add(&player->generation, 1);
    vfh_reset_for_seek(player, target);
}

bool vfh_player_take_due_video_frame(vfh_player *player, AVFrame **out_frame,
                                     double *out_pts) {
    if (out_frame) *out_frame = NULL;
    if (out_pts) *out_pts = 0.0;
    if (!player || !out_frame) return false;
    double clock = vfh_clock(player);
    unsigned long generation = atomic_load(&player->generation);
    AVFrame *chosen = NULL;
    double chosen_pts = 0.0;
    pthread_mutex_lock(&player->video_frames.mutex);
    while (player->video_frames.head) {
        vfh_frame_item *item = player->video_frames.head;
        if (item->generation != generation) {
            player->video_frames.head = item->next;
            if (!player->video_frames.head) player->video_frames.tail = NULL;
            player->video_frames.count--;
            pthread_cond_signal(&player->video_frames.not_full);
            pthread_mutex_unlock(&player->video_frames.mutex);
            vfh_frame_item_free(item);
            pthread_mutex_lock(&player->video_frames.mutex);
            continue;
        }
        if (item->pts > clock) break;
        player->video_frames.head = item->next;
        if (!player->video_frames.head) player->video_frames.tail = NULL;
        player->video_frames.count--;
        pthread_cond_signal(&player->video_frames.not_full);
        pthread_mutex_unlock(&player->video_frames.mutex);
        av_frame_free(&chosen);
        chosen = item->frame;
        chosen_pts = item->pts;
        free(item);
        pthread_mutex_lock(&player->video_frames.mutex);
    }
    pthread_mutex_unlock(&player->video_frames.mutex);
    if (!chosen) return false;
    *out_frame = chosen;
    if (out_pts) *out_pts = chosen_pts;
    return true;
}

bool vfh_player_is_finished(const vfh_player *player) {
    if (!player) return true;
    return atomic_load(&player->video_done) && atomic_load(&player->audio_done) &&
           vfh_frame_queue_empty(&player->video_frames);
}

double vfh_player_position(const vfh_player *player) {
    return player ? vfh_clock((vfh_player *)player) : 0.0;
}

double vfh_player_duration(const vfh_player *player) {
    return player ? player->duration : 0.0;
}

bool vfh_player_has_audio(const vfh_player *player) {
    return player && player->audio_stream >= 0;
}

static const char *vfh_player_codec_name(const AVCodecParameters *parameters) {
    if (!parameters) return "Unknown";
    AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    return codec && codec->name ? codec->name : "Unknown";
}

bool vfh_player_get_media_info(const vfh_player *player, vfh_player_media_info *out_info) {
    if (!out_info) return false;
    memset(out_info, 0, sizeof(*out_info));
    if (!player || !player->format || player->video_stream < 0) return false;

    const AVInputFormat *input = player->format->iformat;
    const AVCodecParameters *video = player->format->streams[player->video_stream]->codecpar;
    snprintf(out_info->container, sizeof(out_info->container), "%s",
             input && input->long_name ? input->long_name :
             input && input->name ? input->name : "Unknown");
    snprintf(out_info->video_codec, sizeof(out_info->video_codec), "%s",
             vfh_player_codec_name(video));
    out_info->video_width = video ? video->width : 0;
    out_info->video_height = video ? video->height : 0;
    if (player->audio_stream >= 0) {
        const AVCodecParameters *audio = player->format->streams[player->audio_stream]->codecpar;
        snprintf(out_info->audio_codec, sizeof(out_info->audio_codec), "%s",
                 vfh_player_codec_name(audio));
    } else {
        snprintf(out_info->audio_codec, sizeof(out_info->audio_codec), "%s", "None");
    }
    return true;
}

int vfh_player_chapter_count(const vfh_player *player) {
    if (!player || !player->format) return 0;
    return player->format->nb_chapters > 64 ? 64 : (int)player->format->nb_chapters;
}

bool vfh_player_chapter_get(const vfh_player *player, int index, vfh_player_chapter *out_chapter) {
    if (!out_chapter || index < 0 || index >= vfh_player_chapter_count(player)) return false;
    AVChapter *chapter = player->format->chapters[index];
    if (!chapter || chapter->time_base.den == 0) return false;
    memset(out_chapter, 0, sizeof(*out_chapter));
    out_chapter->start_seconds = (double)chapter->start * (double)chapter->time_base.num /
                                 (double)chapter->time_base.den;
    AVDictionaryEntry *title = av_dict_get(chapter->metadata, "title", NULL, 0);
    if (title && title->value && title->value[0])
        snprintf(out_chapter->title, sizeof(out_chapter->title), "%s", title->value);
    else
        snprintf(out_chapter->title, sizeof(out_chapter->title), "Chapter %d", index + 1);
    return true;
}
