#include "vfh_mpp.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>

#include "vfh_mpp_abi.h"

#define VFH_MPP_CHUNK_SIZE 4096u
#define VFH_MPP_MAX_RETRIES 1000
#define VFH_MPP_FMT_YUV420SP 0u

typedef struct vfh_mpp_frame_item {
    AVFrame *frame;
    int64_t pts;
    struct vfh_mpp_frame_item *next;
} vfh_mpp_frame_item;

struct vfh_mpp_decoder {
    MppCtx context;
    MppApi *api;
    MppPacket packet;
    AVBSFContext *bsf;
    vfh_mpp_frame_item *frames_head;
    vfh_mpp_frame_item *frames_tail;
    bool eos;
    bool failed;
};

static void vfh_mpp_clear_frames(vfh_mpp_decoder *decoder) {
    while (decoder && decoder->frames_head) {
        vfh_mpp_frame_item *item = decoder->frames_head;
        decoder->frames_head = item->next;
        av_frame_free(&item->frame);
        free(item);
    }
    if (decoder) decoder->frames_tail = NULL;
}

static bool vfh_mpp_append_frame(vfh_mpp_decoder *decoder, AVFrame *frame,
                                 int64_t pts) {
    vfh_mpp_frame_item *item = calloc(1, sizeof(*item));
    if (!item) {
        av_frame_free(&frame);
        return false;
    }
    item->frame = frame;
    item->pts = pts;
    if (decoder->frames_tail) decoder->frames_tail->next = item;
    else decoder->frames_head = item;
    decoder->frames_tail = item;
    return true;
}

static bool vfh_mpp_copy_frame(vfh_mpp_decoder *decoder, MppFrame source) {
    const unsigned width = mpp_frame_get_width(source);
    const unsigned height = mpp_frame_get_height(source);
    unsigned hor_stride = mpp_frame_get_hor_stride(source);
    unsigned ver_stride = mpp_frame_get_ver_stride(source);
    const unsigned format = mpp_frame_get_fmt(source);
    MppBuffer buffer = mpp_frame_get_buffer(source);
    if (!hor_stride) hor_stride = width;
    if (!ver_stride) ver_stride = height;
    if (!width || !height || hor_stride < width || ver_stride < height ||
        format != VFH_MPP_FMT_YUV420SP ||
        (size_t)hor_stride > SIZE_MAX / (size_t)ver_stride) {
        fprintf(stderr, "videofromhell: unsupported MPP frame %ux%u stride %ux%u fmt=0x%x\n",
                width, height, hor_stride, ver_stride, format);
        return false;
    }
    const size_t luma_bytes = (size_t)hor_stride * (size_t)ver_stride;
    if (!buffer) {
        fprintf(stderr, "videofromhell: MPP frame has no CPU-visible buffer\n");
        return false;
    }
    const uint8_t *input = mpp_buffer_get_ptr_with_caller(buffer, "videofromhell");
    if (!input) {
        fprintf(stderr, "videofromhell: MPP buffer cannot be mapped\n");
        return false;
    }

    AVFrame *output = av_frame_alloc();
    if (!output) return false;
    output->format = AV_PIX_FMT_NV12;
    output->width = (int)width;
    output->height = (int)height;
    output->pts = mpp_frame_get_pts(source);
    if (av_frame_get_buffer(output, 32) < 0) {
        av_frame_free(&output);
        return false;
    }
    for (unsigned row = 0; row < height; row++)
        memcpy(output->data[0] + (size_t)row * output->linesize[0],
               input + (size_t)row * hor_stride, width);
    const uint8_t *chroma = input + luma_bytes;
    const unsigned chroma_rows = (height + 1u) / 2u;
    for (unsigned row = 0; row < chroma_rows; row++)
        memcpy(output->data[1] + (size_t)row * output->linesize[1],
               chroma + (size_t)row * hor_stride, width);
    if (!vfh_mpp_append_frame(decoder, output, output->pts)) return false;
    return true;
}

static bool vfh_mpp_drain(vfh_mpp_decoder *decoder) {
    for (;;) {
        MppFrame frame = NULL;
        MPP_RET result = decoder->api->decode_get_frame(decoder->context, &frame);
        if (result == MPP_ERR_TIMEOUT || result == MPP_NOK) return true;
        if (result != MPP_OK) {
            fprintf(stderr, "videofromhell: MPP decode_get_frame failed: %d\n", result);
            decoder->failed = true;
            return false;
        }
        if (!frame) return true;
        if (mpp_frame_get_info_change(frame)) {
            result = decoder->api->control(decoder->context,
                                           VFH_MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            if (result != MPP_OK) {
                fprintf(stderr, "videofromhell: MPP info-change acknowledgement failed: %d\n",
                        result);
                mpp_frame_deinit(&frame);
                decoder->failed = true;
                return false;
            }
        } else if (mpp_frame_get_buffer(frame) && !vfh_mpp_copy_frame(decoder, frame)) {
            mpp_frame_deinit(&frame);
            decoder->failed = true;
            return false;
        }
        if (mpp_frame_get_eos(frame)) decoder->eos = true;
        mpp_frame_deinit(&frame);
    }
}

static bool vfh_mpp_submit_chunk(vfh_mpp_decoder *decoder, const uint8_t *data,
                                 size_t size, int64_t pts, bool eos) {
    mpp_packet_set_data(decoder->packet, (void *)data);
    mpp_packet_set_size(decoder->packet, size);
    mpp_packet_set_pos(decoder->packet, (void *)data);
    mpp_packet_set_length(decoder->packet, size);
    mpp_packet_set_pts(decoder->packet, pts);
    if (eos && mpp_packet_set_eos(decoder->packet) != MPP_OK) {
        fprintf(stderr, "videofromhell: MPP could not signal EOS\n");
        decoder->failed = true;
        return false;
    }
    for (int attempt = 0; attempt < VFH_MPP_MAX_RETRIES; attempt++) {
        MPP_RET result = decoder->api->decode_put_packet(decoder->context, decoder->packet);
        if (result == MPP_OK) return true;
        if (result != MPP_ERR_BUFFER_FULL) {
            fprintf(stderr, "videofromhell: MPP decode_put_packet failed: %d\n", result);
            decoder->failed = true;
            return false;
        }
        if (!vfh_mpp_drain(decoder)) return false;
        usleep(1000);
    }
    fprintf(stderr, "videofromhell: MPP input queue stayed full\n");
    decoder->failed = true;
    return false;
}

static bool vfh_mpp_submit_filtered(vfh_mpp_decoder *decoder, const AVPacket *packet) {
    for (size_t offset = 0; offset < (size_t)packet->size;) {
        size_t chunk = (size_t)packet->size - offset;
        if (chunk > VFH_MPP_CHUNK_SIZE) chunk = VFH_MPP_CHUNK_SIZE;
        if (!vfh_mpp_submit_chunk(decoder, packet->data + offset, chunk,
                                  packet->pts, false)) return false;
        offset += chunk;
    }
    return true;
}

static bool vfh_mpp_drain_bsf(vfh_mpp_decoder *decoder) {
    AVPacket *filtered = av_packet_alloc();
    if (!filtered) return false;
    bool ok = true;
    for (;;) {
        int result = av_bsf_receive_packet(decoder->bsf, filtered);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0 || !vfh_mpp_submit_filtered(decoder, filtered)) {
            if (result < 0)
                fprintf(stderr, "videofromhell: MP4-to-Annex-B conversion failed: %d\n", result);
            ok = false;
            break;
        }
        av_packet_unref(filtered);
    }
    av_packet_free(&filtered);
    return ok;
}

bool vfh_mpp_decoder_open(vfh_mpp_decoder **out,
                          const AVCodecParameters *parameters,
                          AVRational time_base) {
    if (out) *out = NULL;
    if (!out || !parameters) return false;
    const char *filter_name = NULL;
    MppCodingType codec = 0;
    if (parameters->codec_id == AV_CODEC_ID_H264) {
        filter_name = "h264_mp4toannexb";
        codec = MPP_VIDEO_CodingAVC;
    } else if (parameters->codec_id == AV_CODEC_ID_HEVC) {
        filter_name = "hevc_mp4toannexb";
        codec = MPP_VIDEO_CodingHEVC;
    } else {
        return false;
    }
    if (mpp_check_support_format(MPP_CTX_DEC, codec) != MPP_OK) return false;

    vfh_mpp_decoder *decoder = calloc(1, sizeof(*decoder));
    const AVBitStreamFilter *filter = av_bsf_get_by_name(filter_name);
    if (!decoder || !filter || av_bsf_alloc(filter, &decoder->bsf) < 0 ||
        avcodec_parameters_copy(decoder->bsf->par_in, parameters) < 0) goto failed;
    decoder->bsf->time_base_in = time_base;
    if (av_bsf_init(decoder->bsf) < 0 ||
        mpp_packet_init(&decoder->packet, NULL, 0) != MPP_OK || !decoder->packet ||
        mpp_create(&decoder->context, &decoder->api) != MPP_OK || !decoder->api ||
        decoder->api->size < sizeof(*decoder->api) ||
        mpp_init(decoder->context, MPP_CTX_DEC, codec) != MPP_OK) goto failed;

    MppDecCfg config = NULL;
    if (mpp_dec_cfg_init(&config) != MPP_OK ||
        decoder->api->control(decoder->context, VFH_MPP_DEC_GET_CFG, config) != MPP_OK ||
        mpp_dec_cfg_set_u32(config, "base:split_parse", 1) != MPP_OK ||
        decoder->api->control(decoder->context, VFH_MPP_DEC_SET_CFG, config) != MPP_OK) {
        if (config) mpp_dec_cfg_deinit(config);
        goto failed;
    }
    mpp_dec_cfg_deinit(config);
    *out = decoder;
    return true;

failed:
    vfh_mpp_decoder_close(&decoder);
    return false;
}

void vfh_mpp_decoder_close(vfh_mpp_decoder **decoder_ptr) {
    if (!decoder_ptr || !*decoder_ptr) return;
    vfh_mpp_decoder *decoder = *decoder_ptr;
    vfh_mpp_clear_frames(decoder);
    av_bsf_free(&decoder->bsf);
    if (decoder->packet) mpp_packet_deinit(&decoder->packet);
    if (decoder->context) mpp_destroy(decoder->context);
    free(decoder);
    *decoder_ptr = NULL;
}

bool vfh_mpp_decoder_reset(vfh_mpp_decoder *decoder) {
    if (!decoder || !decoder->context || !decoder->packet) return false;
    if (decoder->api->reset(decoder->context) != MPP_OK ||
        mpp_packet_clr_eos(decoder->packet) != MPP_OK) {
        decoder->failed = true;
        return false;
    }
    av_bsf_flush(decoder->bsf);
    vfh_mpp_clear_frames(decoder);
    decoder->eos = false;
    decoder->failed = false;
    return true;
}

bool vfh_mpp_decoder_send_packet(vfh_mpp_decoder *decoder, AVPacket *packet) {
    if (!decoder || decoder->failed) return false;
    int result = 0;
    for (int attempt = 0; attempt < VFH_MPP_MAX_RETRIES; attempt++) {
        result = av_bsf_send_packet(decoder->bsf, packet);
        if (result != AVERROR(EAGAIN)) break;
        if (!vfh_mpp_drain_bsf(decoder)) return false;
    }
    if (result < 0 || !vfh_mpp_drain_bsf(decoder)) {
        if (result < 0)
            fprintf(stderr, "videofromhell: MP4-to-Annex-B input failed: %d\n", result);
        decoder->failed = true;
        return false;
    }
    if (!packet)
        return vfh_mpp_submit_chunk(decoder, NULL, 0, AV_NOPTS_VALUE, true);
    return true;
}

int vfh_mpp_decoder_receive_frame(vfh_mpp_decoder *decoder, AVFrame **out_frame,
                                  int64_t *out_pts) {
    if (out_frame) *out_frame = NULL;
    if (out_pts) *out_pts = AV_NOPTS_VALUE;
    if (!decoder || !out_frame) return -1;
    if (!vfh_mpp_drain(decoder)) return -1;
    vfh_mpp_frame_item *item = decoder->frames_head;
    if (!item) return 0;
    decoder->frames_head = item->next;
    if (!decoder->frames_head) decoder->frames_tail = NULL;
    *out_frame = item->frame;
    if (out_pts) *out_pts = item->pts;
    free(item);
    return 1;
}

bool vfh_mpp_decoder_reached_eos(const vfh_mpp_decoder *decoder) {
    return decoder && decoder->eos;
}
