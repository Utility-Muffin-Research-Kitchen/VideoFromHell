/* Resume positions.
 *
 * A flat JSON object of {path: seconds} under USERDATA_PATH, so exiting a film
 * part-way and coming back lands where you left off. Deliberately keyed by
 * absolute path: the SD mount points swap between boots on this device, but a
 * path is still what the browser hands us, and a stale key costs nothing but a
 * pruned entry.
 *
 * Everything here is best-effort. A missing or corrupt store means "no resume
 * point", never a failure to play. */
#ifndef VFH_RESUME_H
#define VFH_RESUME_H

#include <stdbool.h>

/* Seconds into `path`, or 0 when there is no usable resume point. */
double vfh_resume_get(const char *path);

/* Record a position. `position <= 0`, or within the end-of-film margin, clears
   the entry instead -- finishing a film should not leave it "half watched". */
void vfh_resume_set(const char *path, double position, double duration);

#endif /* VFH_RESUME_H */
