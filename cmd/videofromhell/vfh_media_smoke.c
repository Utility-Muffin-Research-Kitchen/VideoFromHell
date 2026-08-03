#include "vfh_media.h"

#include <stdio.h>

/* Target-only acceptance helper. It deliberately exercises VFH's own lazy
 * poster path rather than shelling out to ffmpeg, so device failures can be
 * diagnosed separately from MPP playback. */
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <video>\n", argv[0]);
        return 2;
    }
    vfh_media_metadata metadata;
    if (!vfh_media_probe_metadata(argv[1], &metadata)) {
        fprintf(stderr, "vfh-media-smoke: metadata probe failed: %s\n", argv[1]);
        return 1;
    }
    vfh_media_poster_status status = VFH_MEDIA_POSTER_ERROR;
    SDL_Surface *poster = vfh_media_decode_poster(argv[1], 260, &status);
    if (!poster) {
        fprintf(stderr, "vfh-media-smoke: poster failed: status=%d path=%s\n",
                (int)status, argv[1]);
        return 1;
    }
    printf("vfh-media-smoke: poster=%dx%d duration=%.3f video=%s audio=%s\n",
           poster->w, poster->h, metadata.duration, metadata.video_codec,
           metadata.audio_codec);
    SDL_FreeSurface(poster);
    return 0;
}
