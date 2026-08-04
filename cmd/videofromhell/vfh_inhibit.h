/* Screen-on lease for playback.
 *
 * jawakad's auto-sleep blanks the backlight purely on input idle, so a film
 * plays for twenty minutes with no button press and the screen dies mid-scene.
 * The daemon owns the mechanism (a pid-tied `block-screen` lease, released
 * automatically if we crash); this app owns only the policy: hold while a video
 * is actually playing, drop it the moment it is not.
 *
 * Every call is best-effort and silent. A direct launch with no daemon running
 * is a normal case, not an error -- playback must never depend on the lease. */
#ifndef VFH_INHIBIT_H
#define VFH_INHIBIT_H

#include <stdbool.h>

/* Take the lease if we do not already hold one. Safe to call repeatedly. */
void vfh_inhibit_acquire(const char *reason);

/* Drop the lease if held. Safe to call when nothing is held. */
void vfh_inhibit_release(void);

/* True while this process holds a screen lease. */
bool vfh_inhibit_held(void);

#endif /* VFH_INHIBIT_H */
