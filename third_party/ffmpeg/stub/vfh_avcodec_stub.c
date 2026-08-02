/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>

AVCodec *avcodec_find_decoder(enum AVCodecID id) {
    (void)id;
    return NULL;
}

AVCodecContext *avcodec_alloc_context3(const AVCodec *codec) {
    (void)codec;
    return NULL;
}

int avcodec_parameters_to_context(AVCodecContext *codec,
                                  const AVCodecParameters *par) {
    (void)codec;
    (void)par;
    return -1;
}

int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec,
                  AVDictionary **options) {
    (void)avctx;
    (void)codec;
    (void)options;
    return -1;
}

int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt) {
    (void)avctx;
    (void)avpkt;
    return -1;
}

int avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    (void)avctx;
    (void)frame;
    return -1;
}

void avcodec_free_context(AVCodecContext **avctx) {
    if (avctx) *avctx = NULL;
}

void avcodec_flush_buffers(AVCodecContext *avctx) {
    (void)avctx;
}

AVPacket *av_packet_alloc(void) {
    return NULL;
}

void av_packet_free(AVPacket **pkt) {
    if (pkt) *pkt = NULL;
}

void av_packet_unref(AVPacket *pkt) {
    (void)pkt;
}

AVPacket *av_packet_clone(const AVPacket *src) {
    (void)src;
    return NULL;
}

int avcodec_parameters_copy(AVCodecParameters *dst,
                            const AVCodecParameters *src) {
    (void)dst;
    (void)src;
    return -1;
}

const AVBitStreamFilter *av_bsf_get_by_name(const char *name) {
    (void)name;
    return NULL;
}

int av_bsf_alloc(const AVBitStreamFilter *filter, AVBSFContext **ctx) {
    (void)filter;
    if (ctx) *ctx = NULL;
    return -1;
}

int av_bsf_init(AVBSFContext *ctx) {
    (void)ctx;
    return -1;
}

int av_bsf_send_packet(AVBSFContext *ctx, AVPacket *pkt) {
    (void)ctx;
    (void)pkt;
    return -1;
}

int av_bsf_receive_packet(AVBSFContext *ctx, AVPacket *pkt) {
    (void)ctx;
    (void)pkt;
    return -1;
}

void av_bsf_flush(AVBSFContext *ctx) {
    (void)ctx;
}

void av_bsf_free(AVBSFContext **ctx) {
    if (ctx) *ctx = NULL;
}
