/* Resume and watched state inside playback-v2.json. This module preserves the
 * queue member managed by vfh_queue; all writes use the same FAT-safe atomic
 * replacement protocol. */
#ifndef VFH_RESUME_H
#define VFH_RESUME_H

#include <stdbool.h>

#include "vfh_library.h"

typedef struct {
    vfh_content_kind content_kind;
    int source_index;
    const char *relative_path;
} vfh_resume_identity;

/* Seconds into `path`, or 0 when there is no usable resume point. */
double vfh_resume_get(const char *path);
double vfh_resume_get_identity(const vfh_resume_identity *identity, const char *absolute_path);

/* Record a position. Positions at/after 90% become watched; <=30 seconds do
   not remain in Continue Watching. */
void vfh_resume_set(const char *path, double position, double duration);
void vfh_resume_set_identity(const vfh_resume_identity *identity, const char *absolute_path,
                             double position, double duration);

/* EOF is definitive even if a decoder's last PTS is short of the duration. */
void vfh_resume_mark_watched(const char *path, double duration);
void vfh_resume_mark_identity_watched(const vfh_resume_identity *identity,
                                      const char *absolute_path, double duration);

#endif /* VFH_RESUME_H */
