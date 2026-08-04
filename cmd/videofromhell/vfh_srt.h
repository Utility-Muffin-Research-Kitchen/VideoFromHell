/* External SubRip (.srt) subtitles.
 *
 * Sidecar only: `<video>.srt`, or a language-tagged `<video>.<lang>.srt`,
 * matched case-insensitively the way Disco Boy matches `cover.png`. Embedded
 * and bitmap subtitle streams stay out of scope -- they need a renderer, and
 * libass is far more than this app should carry.
 *
 * The whole cue list is parsed once when playback opens, then looked up by
 * playback time. A malformed file yields whatever cues did parse (possibly
 * none) and never blocks playback. */
#ifndef VFH_SRT_H
#define VFH_SRT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct vfh_srt vfh_srt;

/* Find and parse a sidecar for `video_path`. Returns NULL when there is no
   sidecar, or when it contained no usable cues. */
vfh_srt *vfh_srt_load_for(const char *video_path);

/* Parse SRT text directly. Exposed for tests. */
vfh_srt *vfh_srt_parse(const char *text);

void vfh_srt_free(vfh_srt *srt);

/* Cue text active at `seconds`, or NULL when no cue covers that moment.
   Lines within a cue are separated by '\n'. The pointer stays valid until
   vfh_srt_free(). */
const char *vfh_srt_text_at(const vfh_srt *srt, double seconds);

int vfh_srt_count(const vfh_srt *srt);
/* Absolute path of the loaded sidecar, or "" when parsed from memory. */
const char *vfh_srt_source(const vfh_srt *srt);

#endif /* VFH_SRT_H */
