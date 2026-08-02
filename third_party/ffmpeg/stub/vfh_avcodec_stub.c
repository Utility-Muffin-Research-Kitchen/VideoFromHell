/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
void *avcodec_find_decoder(void) { return 0; }
void *avcodec_alloc_context3(void) { return 0; }
int avcodec_parameters_to_context(void) { return -1; }
int avcodec_open2(void) { return -1; }
int avcodec_send_packet(void) { return -1; }
int avcodec_receive_frame(void) { return -1; }
void avcodec_free_context(void) {}
void avcodec_flush_buffers(void) {}
void *av_packet_alloc(void) { return 0; }
void av_packet_free(void) {}
void av_packet_unref(void) {}
void *av_packet_clone(void) { return 0; }
int avcodec_parameters_copy(void) { return -1; }
void *av_bsf_get_by_name(void) { return 0; }
int av_bsf_alloc(void) { return -1; }
int av_bsf_init(void) { return -1; }
int av_bsf_send_packet(void) { return -1; }
int av_bsf_receive_packet(void) { return -1; }
void av_bsf_flush(void) {}
void av_bsf_free(void) {}
