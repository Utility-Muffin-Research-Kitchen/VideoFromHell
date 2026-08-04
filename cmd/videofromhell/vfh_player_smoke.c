/* Test-only MLP1 acceptance runner. It proves the real player selects MPP by
   requiring at least one NV12 frame; it is never included in the Pak. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/pixfmt.h>

#include "vfh_player.h"

static double vfh_smoke_now(void) {
    struct timespec value = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <video>\n", argv[0]);
        return 64;
    }
    vfh_player *player = vfh_player_create();
    char error[256] = {0};
    if (!player || !vfh_player_open(player, argv[1], error, (int)sizeof(error))) {
        fprintf(stderr, "vfh-player-smoke: %s\n", error[0] ? error : "open failed");
        vfh_player_destroy(player);
        return 1;
    }

    unsigned frames = 0;
    unsigned nv12_frames = 0;
    const double deadline = vfh_smoke_now() + 10.0;
    while (vfh_smoke_now() < deadline) {
        AVFrame *frame = NULL;
        if (vfh_player_take_due_video_frame(player, &frame, NULL)) {
            frames++;
            if (frame && frame->format == AV_PIX_FMT_NV12) nv12_frames++;
            av_frame_free(&frame);
        } else if (vfh_player_is_finished(player)) {
            break;
        } else {
            usleep(5000);
        }
    }
    printf("vfh-player-smoke: frames=%u nv12=%u position=%.3f duration=%.3f finished=%d\n",
           frames, nv12_frames, vfh_player_position(player),
           vfh_player_duration(player), vfh_player_is_finished(player));
    vfh_player_destroy(player);
    return frames && nv12_frames ? 0 : 1;
}
