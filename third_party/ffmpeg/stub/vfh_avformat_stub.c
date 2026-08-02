/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
int avformat_open_input(void) { return -1; }
int avformat_find_stream_info(void) { return -1; }
int av_find_best_stream(void) { return -1; }
int av_read_frame(void) { return -1; }
int av_seek_frame(void) { return -1; }
void avformat_close_input(void) {}
