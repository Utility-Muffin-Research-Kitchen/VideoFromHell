/*
 * Link-only placeholder for an FFmpeg SONAME stub. It carries no FFmpeg code
 * and exports no FFmpeg API. Later decoder sources must add the exact symbols
 * they use before linking against the corresponding stub.
 */
static const char vfh_link_stub_marker[] = "Video From Hell link stub";

const char *vfh_ffmpeg_link_stub_marker(void) {
    return vfh_link_stub_marker;
}
