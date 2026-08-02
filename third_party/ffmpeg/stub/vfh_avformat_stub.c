/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
#include <libavformat/avformat.h>

int avformat_open_input(AVFormatContext **ps, const char *url,
                        ff_const59 AVInputFormat *fmt, AVDictionary **options) {
    (void)url;
    (void)fmt;
    (void)options;
    if (ps) *ps = NULL;
    return -1;
}

int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options) {
    (void)ic;
    (void)options;
    return -1;
}

int av_find_best_stream(AVFormatContext *ic, enum AVMediaType type,
                        int wanted_stream_nb, int related_stream,
                        AVCodec **decoder_ret, int flags) {
    (void)ic;
    (void)type;
    (void)wanted_stream_nb;
    (void)related_stream;
    (void)flags;
    if (decoder_ret) *decoder_ret = NULL;
    return -1;
}

int av_read_frame(AVFormatContext *s, AVPacket *pkt) {
    (void)s;
    (void)pkt;
    return -1;
}

int av_seek_frame(AVFormatContext *s, int stream_index, int64_t timestamp,
                  int flags) {
    (void)s;
    (void)stream_index;
    (void)timestamp;
    (void)flags;
    return -1;
}

void avformat_close_input(AVFormatContext **s) {
    if (s) *s = NULL;
}
