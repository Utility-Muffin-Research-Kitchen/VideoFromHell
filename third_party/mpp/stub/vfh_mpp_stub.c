/* Link-time Rockchip MPP ABI declarations only; never packaged or executed. */
#include "../include/vfh_mpp_abi.h"

MPP_RET mpp_create(MppCtx *ctx, MppApi **mpi) {
    if (ctx) *ctx = NULL;
    if (mpi) *mpi = NULL;
    return MPP_NOK;
}

MPP_RET mpp_init(MppCtx ctx, MppCtxType type, MppCodingType coding) {
    (void)ctx;
    (void)type;
    (void)coding;
    return MPP_NOK;
}

MPP_RET mpp_destroy(MppCtx ctx) {
    (void)ctx;
    return MPP_NOK;
}

MPP_RET mpp_check_support_format(MppCtxType type, MppCodingType coding) {
    (void)type;
    (void)coding;
    return MPP_NOK;
}

const char *get_mpp_version(void) { return "stub"; }

MPP_RET mpp_packet_init(MppPacket *packet, void *data, size_t size) {
    (void)data;
    (void)size;
    if (packet) *packet = NULL;
    return MPP_NOK;
}

MPP_RET mpp_packet_deinit(MppPacket *packet) {
    if (packet) *packet = NULL;
    return MPP_NOK;
}

MPP_RET mpp_packet_set_eos(MppPacket packet) {
    (void)packet;
    return MPP_NOK;
}

MPP_RET mpp_packet_clr_eos(MppPacket packet) {
    (void)packet;
    return MPP_NOK;
}

void mpp_packet_set_data(MppPacket packet, void *data) {
    (void)packet;
    (void)data;
}

void mpp_packet_set_size(MppPacket packet, size_t size) {
    (void)packet;
    (void)size;
}

void mpp_packet_set_pos(MppPacket packet, void *position) {
    (void)packet;
    (void)position;
}

void mpp_packet_set_length(MppPacket packet, size_t size) {
    (void)packet;
    (void)size;
}

void mpp_packet_set_pts(MppPacket packet, int64_t pts) {
    (void)packet;
    (void)pts;
}

size_t mpp_packet_get_length(MppPacket packet) {
    (void)packet;
    return 0;
}

MPP_RET mpp_dec_cfg_init(MppDecCfg *cfg) {
    if (cfg) *cfg = NULL;
    return MPP_NOK;
}

MPP_RET mpp_dec_cfg_deinit(MppDecCfg cfg) {
    (void)cfg;
    return MPP_NOK;
}

MPP_RET mpp_dec_cfg_set_u32(MppDecCfg cfg, const char *name,
                            uint32_t value) {
    (void)cfg;
    (void)name;
    (void)value;
    return MPP_NOK;
}

MPP_RET mpp_frame_deinit(MppFrame *frame) {
    if (frame) *frame = NULL;
    return MPP_NOK;
}

uint32_t mpp_frame_get_width(MppFrame frame) {
    (void)frame;
    return 0;
}

uint32_t mpp_frame_get_height(MppFrame frame) {
    (void)frame;
    return 0;
}

uint32_t mpp_frame_get_hor_stride(MppFrame frame) {
    (void)frame;
    return 0;
}

uint32_t mpp_frame_get_ver_stride(MppFrame frame) {
    (void)frame;
    return 0;
}

uint32_t mpp_frame_get_eos(MppFrame frame) {
    (void)frame;
    return 0;
}

uint32_t mpp_frame_get_info_change(MppFrame frame) {
    (void)frame;
    return 0;
}

uint32_t mpp_frame_get_fmt(MppFrame frame) {
    (void)frame;
    return 0;
}

int64_t mpp_frame_get_pts(MppFrame frame) {
    (void)frame;
    return 0;
}

MppBuffer mpp_frame_get_buffer(MppFrame frame) {
    (void)frame;
    return NULL;
}

void *mpp_buffer_get_ptr_with_caller(MppBuffer buffer, const char *caller) {
    (void)buffer;
    (void)caller;
    return NULL;
}
