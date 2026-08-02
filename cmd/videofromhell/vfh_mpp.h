#ifndef VFH_MPP_H
#define VFH_MPP_H

#include <stdbool.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>

typedef struct vfh_mpp_decoder vfh_mpp_decoder;

/* The direct-MPP decoder is limited deliberately to H.264/HEVC NV12 output.
   The caller owns every AVFrame returned by receive_frame(). */
bool vfh_mpp_decoder_open(vfh_mpp_decoder **out,
                          const AVCodecParameters *parameters,
                          AVRational time_base);
void vfh_mpp_decoder_close(vfh_mpp_decoder **decoder);
bool vfh_mpp_decoder_reset(vfh_mpp_decoder *decoder);

/* Pass NULL to flush the FFmpeg bitstream filter and signal MPP EOS. */
bool vfh_mpp_decoder_send_packet(vfh_mpp_decoder *decoder, AVPacket *packet);

/* 1 = frame returned, 0 = no frame ready, -1 = decoder error. */
int vfh_mpp_decoder_receive_frame(vfh_mpp_decoder *decoder, AVFrame **out_frame,
                                  int64_t *out_pts);
bool vfh_mpp_decoder_reached_eos(const vfh_mpp_decoder *decoder);

#endif
