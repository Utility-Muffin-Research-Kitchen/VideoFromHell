#ifndef VFH_PLAYER_H
#define VFH_PLAYER_H

#include <stdbool.h>

#include <libavutil/frame.h>

typedef struct vfh_player vfh_player;

enum { VFH_PLAYER_MEDIA_LABEL_MAX = 64, VFH_PLAYER_CHAPTER_TITLE_MAX = 128 };

/* Snapshot of immutable stream metadata from the currently open container. */
typedef struct {
    char container[VFH_PLAYER_MEDIA_LABEL_MAX];
    char video_codec[VFH_PLAYER_MEDIA_LABEL_MAX];
    char audio_codec[VFH_PLAYER_MEDIA_LABEL_MAX];
    int video_width;
    int video_height;
} vfh_player_media_info;

typedef struct {
    double start_seconds;
    char title[VFH_PLAYER_CHAPTER_TITLE_MAX];
} vfh_player_chapter;

/* The player owns one demux context and three bounded worker lanes: demux,
   video decode, and audio decode/output. Renderer work remains with the caller. */
vfh_player *vfh_player_create(void);
void vfh_player_destroy(vfh_player *player);

bool vfh_player_open(vfh_player *player, const char *path, char *error, int error_size);
void vfh_player_close(vfh_player *player);

void vfh_player_set_paused(vfh_player *player, bool paused);
bool vfh_player_is_paused(const vfh_player *player);
void vfh_player_seek_relative(vfh_player *player, double seconds);

/* Apply Jawaka's live route. The audio worker reopens only the ALSA PCM, never
 * the demuxer or video decoder. A recoverable route fallback is returned once. */
void vfh_player_set_audio_output(vfh_player *player, const char *output);
bool vfh_player_take_audio_notice(vfh_player *player, char *out, int out_size);

/* Returns the newest queued frame whose PTS is due according to the audio
   clock. Ownership transfers to the caller, which must av_frame_free(). */
bool vfh_player_take_due_video_frame(vfh_player *player, AVFrame **out_frame,
                                     double *out_pts);

bool vfh_player_is_finished(const vfh_player *player);
double vfh_player_position(const vfh_player *player);
double vfh_player_duration(const vfh_player *player);
bool vfh_player_has_audio(const vfh_player *player);
bool vfh_player_get_media_info(const vfh_player *player, vfh_player_media_info *out_info);
int vfh_player_chapter_count(const vfh_player *player);
bool vfh_player_chapter_get(const vfh_player *player, int index, vfh_player_chapter *out_chapter);

#endif
