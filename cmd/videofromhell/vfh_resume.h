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

/* An immutable in-memory view of playback-v2.json. Browser rendering loads one
 * snapshot per rebuild so a large library never reopens and reparses its FAT
 * store once for every row. `root` is private to vfh_resume.c. */
typedef struct {
    void *root;
} vfh_resume_snapshot;

void vfh_resume_snapshot_init(vfh_resume_snapshot *snapshot);
bool vfh_resume_snapshot_load(vfh_resume_snapshot *snapshot);
void vfh_resume_snapshot_destroy(vfh_resume_snapshot *snapshot);
double vfh_resume_snapshot_get(const vfh_resume_snapshot *snapshot, const char *path);
double vfh_resume_snapshot_get_identity(const vfh_resume_snapshot *snapshot,
                                        const vfh_resume_identity *identity,
                                        const char *absolute_path);

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

/* Explicitly remove a continuation without marking it watched. */
bool vfh_resume_remove(const char *path);
bool vfh_resume_remove_identity(const vfh_resume_identity *identity,
                                const char *absolute_path);

#endif /* VFH_RESUME_H */
