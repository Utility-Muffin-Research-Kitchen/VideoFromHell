/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
#include <libswresample/swresample.h>

struct SwrContext *swr_alloc_set_opts(struct SwrContext *s,
                                      int64_t out_ch_layout,
                                      enum AVSampleFormat out_sample_fmt,
                                      int out_sample_rate,
                                      int64_t in_ch_layout,
                                      enum AVSampleFormat in_sample_fmt,
                                      int in_sample_rate, int log_offset,
                                      void *log_ctx) {
    (void)s;
    (void)out_ch_layout;
    (void)out_sample_fmt;
    (void)out_sample_rate;
    (void)in_ch_layout;
    (void)in_sample_fmt;
    (void)in_sample_rate;
    (void)log_offset;
    (void)log_ctx;
    return NULL;
}

int swr_init(struct SwrContext *s) {
    (void)s;
    return -1;
}

void swr_free(struct SwrContext **s) {
    if (s) *s = NULL;
}

int swr_convert(struct SwrContext *s, uint8_t **out, int out_count,
                const uint8_t **in, int in_count) {
    (void)s;
    (void)out;
    (void)out_count;
    (void)in;
    (void)in_count;
    return -1;
}
