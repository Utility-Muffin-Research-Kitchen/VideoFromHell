#ifndef VFH_PLAYER_H
#define VFH_PLAYER_H

#include <stdbool.h>

#include <libavutil/frame.h>

typedef struct vfh_player vfh_player;

/* The player owns one demux context and three bounded worker lanes: demux,
   video decode, and audio decode/output. Renderer work remains with the caller. */
vfh_player *vfh_player_create(void);
void vfh_player_destroy(vfh_player *player);

bool vfh_player_open(vfh_player *player, const char *path, char *error, int error_size);
void vfh_player_close(vfh_player *player);

void vfh_player_set_paused(vfh_player *player, bool paused);
bool vfh_player_is_paused(const vfh_player *player);
void vfh_player_seek_relative(vfh_player *player, double seconds);

/* Returns the newest queued frame whose PTS is due according to the audio
   clock. Ownership transfers to the caller, which must av_frame_free(). */
bool vfh_player_take_due_video_frame(vfh_player *player, AVFrame **out_frame,
                                     double *out_pts);

bool vfh_player_is_finished(const vfh_player *player);
double vfh_player_position(const vfh_player *player);
double vfh_player_duration(const vfh_player *player);
bool vfh_player_has_audio(const vfh_player *player);

#endif
