/*
 * Minimal public ABI declaration for the Video From Hell MPP probe.
 *
 * Derived from Rockchip MPP's Apache-2.0 public headers at 6dadc7e1
 * (2021-09-02).  Keep this small: production decoder code belongs behind the
 * Phase 5 throughput gate, and this header must not pull a vendor SDK into the
 * Pak.
 */
#ifndef VFH_MPP_ABI_H
#define VFH_MPP_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t MPP_RET;
typedef void *MppCtx;
typedef void *MppParam;
typedef void *MppFrame;
typedef void *MppPacket;
typedef void *MppBuffer;
typedef void *MppTask;
typedef void *MppDecCfg;

typedef enum {
    MPP_CTX_DEC = 0,
} MppCtxType;

typedef enum {
    MPP_VIDEO_CodingAVC = 7,
    MPP_VIDEO_CodingHEVC = 0x01000004,
} MppCodingType;

typedef uint32_t MpiCmd;

enum {
    MPP_OK = 0,
    MPP_NOK = -1,
    MPP_ERR_TIMEOUT = -8,
    MPP_ERR_BUFFER_FULL = -1012,
    VFH_MPP_DEC_SET_INFO_CHANGE_READY = 0x00310003,
    VFH_MPP_DEC_GET_CFG = 0x00310202,
    VFH_MPP_DEC_SET_CFG = 0x00310201,
};

typedef struct MppApi_t {
    uint32_t size;
    uint32_t version;
    MPP_RET (*decode)(MppCtx ctx, MppPacket packet, MppFrame *frame);
    MPP_RET (*decode_put_packet)(MppCtx ctx, MppPacket packet);
    MPP_RET (*decode_get_frame)(MppCtx ctx, MppFrame *frame);
    void (*reserved_data_flow[9])(void);
    MPP_RET (*reset)(MppCtx ctx);
    MPP_RET (*control)(MppCtx ctx, MpiCmd cmd, MppParam param);
} MppApi;

MPP_RET mpp_create(MppCtx *ctx, MppApi **mpi);
MPP_RET mpp_init(MppCtx ctx, MppCtxType type, MppCodingType coding);
MPP_RET mpp_destroy(MppCtx ctx);
MPP_RET mpp_check_support_format(MppCtxType type, MppCodingType coding);
const char *get_mpp_version(void);

MPP_RET mpp_packet_init(MppPacket *packet, void *data, size_t size);
MPP_RET mpp_packet_deinit(MppPacket *packet);
MPP_RET mpp_packet_set_eos(MppPacket packet);
MPP_RET mpp_packet_clr_eos(MppPacket packet);
void mpp_packet_set_data(MppPacket packet, void *data);
void mpp_packet_set_size(MppPacket packet, size_t size);
void mpp_packet_set_pos(MppPacket packet, void *position);
void mpp_packet_set_length(MppPacket packet, size_t size);
void mpp_packet_set_pts(MppPacket packet, int64_t pts);
size_t mpp_packet_get_length(MppPacket packet);
MPP_RET mpp_dec_cfg_init(MppDecCfg *cfg);
MPP_RET mpp_dec_cfg_deinit(MppDecCfg cfg);
MPP_RET mpp_dec_cfg_set_u32(MppDecCfg cfg, const char *name, uint32_t value);

MPP_RET mpp_frame_deinit(MppFrame *frame);
uint32_t mpp_frame_get_width(MppFrame frame);
uint32_t mpp_frame_get_height(MppFrame frame);
uint32_t mpp_frame_get_hor_stride(MppFrame frame);
uint32_t mpp_frame_get_ver_stride(MppFrame frame);
uint32_t mpp_frame_get_eos(MppFrame frame);
uint32_t mpp_frame_get_info_change(MppFrame frame);
uint32_t mpp_frame_get_fmt(MppFrame frame);
int64_t mpp_frame_get_pts(MppFrame frame);
MppBuffer mpp_frame_get_buffer(MppFrame frame);
void *mpp_buffer_get_ptr_with_caller(MppBuffer buffer, const char *caller);

#endif
