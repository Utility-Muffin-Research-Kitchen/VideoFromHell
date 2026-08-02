/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>

AVFrame *av_frame_alloc(void) {
    return NULL;
}

void av_frame_free(AVFrame **frame) {
    if (frame) *frame = NULL;
}

void av_frame_unref(AVFrame *frame) {
    (void)frame;
}

AVFrame *av_frame_clone(const AVFrame *src) {
    (void)src;
    return NULL;
}

int av_frame_get_buffer(AVFrame *frame, int align) {
    (void)frame;
    (void)align;
    return -1;
}

int64_t av_get_default_channel_layout(int nb_channels) {
    (void)nb_channels;
    return 0;
}

void av_log_set_level(int level) {
    (void)level;
}
