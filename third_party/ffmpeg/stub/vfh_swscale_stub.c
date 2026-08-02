/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
#include <libswscale/swscale.h>

struct SwsContext *sws_getContext(int srcW, int srcH,
                                  enum AVPixelFormat srcFormat,
                                  int dstW, int dstH,
                                  enum AVPixelFormat dstFormat, int flags,
                                  SwsFilter *srcFilter, SwsFilter *dstFilter,
                                  const double *param) {
    (void)srcW;
    (void)srcH;
    (void)srcFormat;
    (void)dstW;
    (void)dstH;
    (void)dstFormat;
    (void)flags;
    (void)srcFilter;
    (void)dstFilter;
    (void)param;
    return NULL;
}

int sws_scale(struct SwsContext *c, const uint8_t *const srcSlice[],
              const int srcStride[], int srcSliceY, int srcSliceH,
              uint8_t *const dst[], const int dstStride[]) {
    (void)c;
    (void)srcSlice;
    (void)srcStride;
    (void)srcSliceY;
    (void)srcSliceH;
    (void)dst;
    (void)dstStride;
    return -1;
}

void sws_freeContext(struct SwsContext *swsContext) {
    (void)swsContext;
}
