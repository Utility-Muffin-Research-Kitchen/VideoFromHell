/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
void *av_frame_alloc(void) { return 0; }
void av_frame_free(void) {}
void av_frame_unref(void) {}
void *av_frame_clone(void) { return 0; }
int av_frame_get_buffer(void) { return -1; }
long long av_get_default_channel_layout(void) { return 0; }
void av_log_set_level(void) {}
