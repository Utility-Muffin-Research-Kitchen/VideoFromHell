/* Link-time FFmpeg 4.4 ABI declarations only; never packaged or executed. */
void *swr_alloc_set_opts(void) { return 0; }
int swr_init(void) { return -1; }
void swr_free(void) {}
int swr_convert(void) { return -1; }
