/*
 * Phase 5 hardware-decode gate.  This is a test binary, not part of the Pak.
 * It demuxes H.264/HEVC with the vendor FFmpeg, converts MP4 packets to
 * Annex-B, then measures MPP decode and an NV12-sized CPU copy.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#include "vfh_mpp_abi.h"

#define VFH_PROBE_MAX_RETRIES 10000
#define VFH_PROBE_CHUNK_SIZE 4096u

typedef struct {
    MppCtx context;
    MppApi *api;
    MppPacket packet;
    uint8_t *copy_buffer;
    size_t copy_capacity;
    uint8_t copy_checksum;
    uint64_t packets;
    uint64_t frames;
    uint64_t copied_bytes;
    double copy_seconds;
    unsigned width;
    unsigned height;
    unsigned hor_stride;
    unsigned ver_stride;
    unsigned format;
    bool eos;
} vfh_probe;

typedef struct {
    vfh_probe *probe;
    AVFormatContext *format;
    AVBSFContext *bsf;
    int video_stream;
    int result;
} vfh_probe_worker;

static double vfh_probe_now(void) {
    struct timespec value = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static unsigned long vfh_probe_peak_rss_kib(void) {
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) return 0;
    char line[256];
    unsigned long result = 0;
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            result = strtoul(line + 6, NULL, 10);
            break;
        }
    }
    fclose(status);
    return result;
}

static bool vfh_probe_reserve_copy(vfh_probe *probe, size_t bytes) {
    if (bytes <= probe->copy_capacity) return true;
    void *replacement = realloc(probe->copy_buffer, bytes);
    if (!replacement) return false;
    probe->copy_buffer = replacement;
    probe->copy_capacity = bytes;
    return true;
}

static int vfh_probe_consume_frame(vfh_probe *probe, MppFrame frame) {
    MPP_RET result = MPP_OK;
    if (mpp_frame_get_info_change(frame)) {
        result = probe->api->control(probe->context,
                                     VFH_MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        if (result != MPP_OK) {
            fprintf(stderr, "vfh-probe: info-change acknowledgement failed: %d\n", result);
            mpp_frame_deinit(&frame);
            return 1;
        }
    } else {
        MppBuffer buffer = mpp_frame_get_buffer(frame);
        if (buffer) {
            unsigned width = mpp_frame_get_width(frame);
            unsigned height = mpp_frame_get_height(frame);
            unsigned hor_stride = mpp_frame_get_hor_stride(frame);
            unsigned ver_stride = mpp_frame_get_ver_stride(frame);
            if (!hor_stride) hor_stride = width;
            if (!ver_stride) ver_stride = height;
            size_t pixels = (size_t)hor_stride * (size_t)ver_stride;
            if (!hor_stride || !ver_stride || pixels > SIZE_MAX / 3u * 2u) {
                fprintf(stderr, "vfh-probe: invalid MPP frame stride %ux%u\n",
                        hor_stride, ver_stride);
                mpp_frame_deinit(&frame);
                return 1;
            }
            size_t bytes = pixels + pixels / 2u;
            void *source = mpp_buffer_get_ptr_with_caller(buffer, "vfh-probe");
            if (!source || !vfh_probe_reserve_copy(probe, bytes)) {
                fprintf(stderr, "vfh-probe: could not map or reserve %zu-byte MPP frame\n", bytes);
                mpp_frame_deinit(&frame);
                return 1;
            }
            double copy_start = vfh_probe_now();
            memcpy(probe->copy_buffer, source, bytes);
            probe->copy_seconds += vfh_probe_now() - copy_start;
            probe->copy_checksum ^= probe->copy_buffer[bytes / 2u];
            probe->copied_bytes += bytes;
            probe->frames++;
            probe->width = width;
            probe->height = height;
            probe->hor_stride = hor_stride;
            probe->ver_stride = ver_stride;
            probe->format = mpp_frame_get_fmt(frame);
        }
    }
    if (mpp_frame_get_eos(frame)) probe->eos = true;
    mpp_frame_deinit(&frame);
    return 0;
}

static int vfh_probe_drain(vfh_probe *probe, bool *made_progress) {
    if (made_progress) *made_progress = false;
    for (;;) {
        MppFrame frame = NULL;
        MPP_RET result = probe->api->decode_get_frame(probe->context, &frame);
        if (result == MPP_ERR_TIMEOUT || result == MPP_NOK) return 0;
        if (result != MPP_OK) {
            fprintf(stderr, "vfh-probe: decode_get_frame failed: %d\n", result);
            return 1;
        }
        if (!frame) return 0;
        if (made_progress) *made_progress = true;
        if (vfh_probe_consume_frame(probe, frame) != 0) return 1;
    }
}

static int vfh_probe_submit_chunk(vfh_probe *probe, const uint8_t *data,
                                  size_t bytes, bool eos) {
    if (!probe->packet) {
        fprintf(stderr, "vfh-probe: reusable MPP packet is unavailable\n");
        return 1;
    }
    MPP_RET result = MPP_OK;
    mpp_packet_set_data(probe->packet, (void *)data);
    mpp_packet_set_size(probe->packet, bytes);
    mpp_packet_set_pos(probe->packet, (void *)data);
    mpp_packet_set_length(probe->packet, bytes);
    if (eos) {
        result = mpp_packet_set_eos(probe->packet);
        if (result != MPP_OK) {
            fprintf(stderr, "vfh-probe: mpp_packet_set_eos failed: %d\n", result);
            return 1;
        }
    }

    for (int attempt = 0; attempt < VFH_PROBE_MAX_RETRIES; attempt++) {
        result = probe->api->decode_put_packet(probe->context, probe->packet);
        if (result == MPP_OK) return 0;
        bool progress = false;
        if (vfh_probe_drain(probe, &progress) != 0) break;
        if (!progress) usleep(1000);
    }
    fprintf(stderr, "vfh-probe: decode_put_packet did not accept %zu-byte input: %d\n",
            bytes, result);
    return 1;
}

static int vfh_probe_submit(vfh_probe *probe, const AVPacket *packet) {
    if (!packet) return vfh_probe_submit_chunk(probe, NULL, 0, true);
    for (size_t offset = 0; offset < (size_t)packet->size;) {
        size_t chunk = (size_t)packet->size - offset;
        if (chunk > VFH_PROBE_CHUNK_SIZE) chunk = VFH_PROBE_CHUNK_SIZE;
        if (vfh_probe_submit_chunk(probe, packet->data + offset, chunk, false) != 0)
            return 1;
        offset += chunk;
    }
    probe->packets++;
    return 0;
}

static int vfh_probe_drain_bsf(vfh_probe *probe, AVBSFContext *bsf,
                               AVPacket *filtered) {
    for (;;) {
        int result = av_bsf_receive_packet(bsf, filtered);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
        if (result < 0) {
            fprintf(stderr, "vfh-probe: bitstream filter receive failed: %d\n", result);
            return 1;
        }
        int submitted = vfh_probe_submit(probe, filtered);
        av_packet_unref(filtered);
        if (submitted != 0) return submitted;
    }
}

static int vfh_probe_send_bsf(vfh_probe *probe, AVBSFContext *bsf,
                              AVPacket *input, AVPacket *filtered) {
    int result = 0;
    for (int attempt = 0; attempt < VFH_PROBE_MAX_RETRIES; attempt++) {
        result = av_bsf_send_packet(bsf, input);
        if (result != AVERROR(EAGAIN)) break;
        if (vfh_probe_drain_bsf(probe, bsf, filtered) != 0) return 1;
    }
    if (result < 0) {
        fprintf(stderr, "vfh-probe: bitstream filter send failed: %d\n", result);
        return 1;
    }
    return vfh_probe_drain_bsf(probe, bsf, filtered);
}

static void *vfh_probe_worker_main(void *opaque) {
    vfh_probe_worker *worker = opaque;
    AVPacket *input = av_packet_alloc();
    AVPacket *filtered = av_packet_alloc();
    worker->result = 1;
    if (!input || !filtered) {
        fprintf(stderr, "vfh-probe: packet allocation failed\n");
        goto done;
    }

    int read_result;
    while ((read_result = av_read_frame(worker->format, input)) >= 0) {
        if (input->stream_index == worker->video_stream) {
            if (vfh_probe_send_bsf(worker->probe, worker->bsf, input, filtered) != 0)
                goto done;
        } else {
            av_packet_unref(input);
        }
    }
    if (read_result != AVERROR_EOF) {
        fprintf(stderr, "vfh-probe: demux failed: %d\n", read_result);
        goto done;
    }
    if (vfh_probe_send_bsf(worker->probe, worker->bsf, NULL, filtered) != 0 ||
        vfh_probe_submit(worker->probe, NULL) != 0)
        goto done;

    for (int attempt = 0; !worker->probe->eos && attempt < VFH_PROBE_MAX_RETRIES; attempt++) {
        bool progress = false;
        if (vfh_probe_drain(worker->probe, &progress) != 0) goto done;
        if (!progress) usleep(1000);
    }
    if (!worker->probe->eos || !worker->probe->frames) {
        fprintf(stderr, "vfh-probe: MPP produced no complete decoded stream\n");
        goto done;
    }
    worker->result = 0;

done:
    av_packet_free(&filtered);
    av_packet_free(&input);
    return NULL;
}

static const char *vfh_probe_codec_name(enum AVCodecID codec) {
    return codec == AV_CODEC_ID_H264 ? "h264" : "hevc";
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <h264-or-hevc-video>\n", argv[0]);
        return 64;
    }

    AVFormatContext *format = NULL;
    AVBSFContext *bsf = NULL;
    vfh_probe probe = {0};
    int exit_code = 1;
    av_log_set_level(AV_LOG_ERROR);

    if (avformat_open_input(&format, argv[1], NULL, NULL) < 0 ||
        avformat_find_stream_info(format, NULL) < 0) {
        fprintf(stderr, "vfh-probe: could not open %s\n", argv[1]);
        goto cleanup;
    }
    int video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream < 0) {
        fprintf(stderr, "vfh-probe: no video stream in %s\n", argv[1]);
        goto cleanup;
    }
    AVStream *stream = format->streams[video_stream];
    enum AVCodecID codec = stream->codecpar->codec_id;
    if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) {
        fprintf(stderr, "vfh-probe: only H.264 and HEVC are supported\n");
        goto cleanup;
    }
    const char *filter_name = codec == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb";
    const AVBitStreamFilter *filter = av_bsf_get_by_name(filter_name);
    if (!filter || av_bsf_alloc(filter, &bsf) < 0 ||
        avcodec_parameters_copy(bsf->par_in, stream->codecpar) < 0) {
        fprintf(stderr, "vfh-probe: %s bitstream filter is unavailable\n", filter_name);
        goto cleanup;
    }
    bsf->time_base_in = stream->time_base;
    if (av_bsf_init(bsf) < 0) {
        fprintf(stderr, "vfh-probe: could not initialise %s\n", filter_name);
        goto cleanup;
    }

    MppCodingType mpp_codec = codec == AV_CODEC_ID_H264
                             ? MPP_VIDEO_CodingAVC : MPP_VIDEO_CodingHEVC;
    if (mpp_packet_init(&probe.packet, NULL, 0) != MPP_OK || !probe.packet ||
        mpp_check_support_format(MPP_CTX_DEC, mpp_codec) != MPP_OK ||
        mpp_create(&probe.context, &probe.api) != MPP_OK || !probe.api ||
        probe.api->size < sizeof(*probe.api) ||
        mpp_init(probe.context, MPP_CTX_DEC, mpp_codec) != MPP_OK) {
        fprintf(stderr, "vfh-probe: MPP cannot initialise %s decode\n",
                vfh_probe_codec_name(codec));
        goto cleanup;
    }
    fprintf(stderr, "vfh-probe: MPP %s, api=%u bytes\n", get_mpp_version(), probe.api->size);
    MppDecCfg config = NULL;
    if (mpp_dec_cfg_init(&config) != MPP_OK ||
        probe.api->control(probe.context, VFH_MPP_DEC_GET_CFG, config) != MPP_OK ||
        mpp_dec_cfg_set_u32(config, "base:split_parse", 1) != MPP_OK ||
        probe.api->control(probe.context, VFH_MPP_DEC_SET_CFG, config) != MPP_OK) {
        fprintf(stderr, "vfh-probe: could not enable the MPP split parser\n");
        if (config) mpp_dec_cfg_deinit(config);
        goto cleanup;
    }
    mpp_dec_cfg_deinit(config);

    vfh_probe_worker worker = {
        .probe = &probe,
        .format = format,
        .bsf = bsf,
        .video_stream = video_stream,
        .result = 1,
    };
    pthread_t worker_thread;
    if (pthread_create(&worker_thread, NULL, vfh_probe_worker_main, &worker) != 0) {
        fprintf(stderr, "vfh-probe: could not create decode worker\n");
        goto cleanup;
    }
    double decode_start = vfh_probe_now();
    pthread_join(worker_thread, NULL);
    double decode_seconds = vfh_probe_now() - decode_start;
    if (worker.result != 0) goto cleanup;

    printf("vfh-probe: mpp=%s codec=%s source=%ux%u stride=%ux%u fmt=0x%x "
           "packets=%" PRIu64 " frames=%" PRIu64 " elapsed=%.3fs fps=%.2f "
           "copy_avg=%.3fms copied=%" PRIu64 " peak_rss=%luKiB checksum=%u\n",
           get_mpp_version(), vfh_probe_codec_name(codec), probe.width, probe.height,
           probe.hor_stride, probe.ver_stride, probe.format, probe.packets, probe.frames,
           decode_seconds, (double)probe.frames / decode_seconds,
           probe.copy_seconds * 1000.0 / (double)probe.frames, probe.copied_bytes,
           vfh_probe_peak_rss_kib(), (unsigned)probe.copy_checksum);
    exit_code = 0;

cleanup:
    av_bsf_free(&bsf);
    avformat_close_input(&format);
    if (probe.packet) mpp_packet_deinit(&probe.packet);
    if (probe.context) mpp_destroy(probe.context);
    free(probe.copy_buffer);
    return exit_code;
}
