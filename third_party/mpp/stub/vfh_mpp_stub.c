/* Link-time Rockchip MPP ABI declarations only; never packaged or executed. */
int mpp_create(void) { return -1; }
int mpp_init(void) { return -1; }
int mpp_destroy(void) { return -1; }
int mpp_check_support_format(void) { return -1; }
const char *get_mpp_version(void) { return "stub"; }
int mpp_packet_init(void) { return -1; }
int mpp_packet_deinit(void) { return -1; }
int mpp_packet_set_eos(void) { return -1; }
int mpp_packet_clr_eos(void) { return -1; }
void mpp_packet_set_data(void) {}
void mpp_packet_set_size(void) {}
void mpp_packet_set_pos(void) {}
void mpp_packet_set_length(void) {}
void mpp_packet_set_pts(void) {}
unsigned long mpp_packet_get_length(void) { return 0; }
int mpp_dec_cfg_init(void) { return -1; }
int mpp_dec_cfg_deinit(void) { return -1; }
int mpp_dec_cfg_set_u32(void) { return -1; }
int mpp_frame_deinit(void) { return -1; }
unsigned int mpp_frame_get_width(void) { return 0; }
unsigned int mpp_frame_get_height(void) { return 0; }
unsigned int mpp_frame_get_hor_stride(void) { return 0; }
unsigned int mpp_frame_get_ver_stride(void) { return 0; }
unsigned int mpp_frame_get_eos(void) { return 0; }
unsigned int mpp_frame_get_info_change(void) { return 0; }
unsigned int mpp_frame_get_fmt(void) { return 0; }
long long mpp_frame_get_pts(void) { return 0; }
void *mpp_frame_get_buffer(void) { return 0; }
void *mpp_buffer_get_ptr_with_caller(void) { return 0; }
