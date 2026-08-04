#ifndef VFH_MEDIA_H
#define VFH_MEDIA_H

#include <stdbool.h>

#include <SDL.h>

#define VFH_MEDIA_TITLE_MAX 256
#define VFH_MEDIA_LABEL_MAX 64

typedef struct {
    double duration;
    int year;
    bool has_embedded_art;
    char title[VFH_MEDIA_TITLE_MAX];
    char container[VFH_MEDIA_LABEL_MAX];
    char video_codec[VFH_MEDIA_LABEL_MAX];
    char audio_codec[VFH_MEDIA_LABEL_MAX];
} vfh_media_metadata;

bool vfh_media_file_supported(const char *name);
/* Container-only probe for the library worker: it opens no decoders and does
 * not touch SDL/renderer state. */
bool vfh_media_probe_metadata(const char *path, vfh_media_metadata *out_metadata);
bool vfh_media_probe_duration(const char *path, double *out_seconds);
void vfh_media_format_duration(double seconds, char *out, int out_size);

typedef enum {
    VFH_MEDIA_POSTER_READY,
    VFH_MEDIA_POSTER_NOT_FOUND,
    VFH_MEDIA_POSTER_ERROR,
} vfh_media_poster_status;

/* Prefer embedded artwork, otherwise decode near 10% of the video and make a
   single 25% retry when that candidate is effectively black.  The caller owns
   the returned surface.  This function does not touch an SDL renderer. */
SDL_Surface *vfh_media_decode_poster(const char *path, int max_dimension,
                                     vfh_media_poster_status *out_status);

#endif
