/* Interactive playback OSD state.
 *
 * Rendering and player operations intentionally stay outside this module.  It
 * owns only the state graph and the keyboard-like focus map so it can be host
 * tested without SDL, Catastrophe, or a decoder.
 */
#ifndef VFH_OSD_H
#define VFH_OSD_H

#include <stdbool.h>
#include <stdint.h>

enum { VFH_OSD_LINGER_MS = 3000 };

typedef enum {
    VFH_OSD_HIDDEN = 0,
    VFH_OSD_TRANSIENT,
    VFH_OSD_PINNED,
    VFH_OSD_SUBMENU,
} vfh_osd_state;

typedef enum {
    VFH_OSD_FOCUS_PREVIOUS = 0,
    VFH_OSD_FOCUS_REWIND,
    VFH_OSD_FOCUS_PLAY_PAUSE,
    VFH_OSD_FOCUS_FORWARD,
    VFH_OSD_FOCUS_NEXT,
    VFH_OSD_FOCUS_QUEUE,
    VFH_OSD_FOCUS_SUBTITLES,
    VFH_OSD_FOCUS_ASPECT,
    VFH_OSD_FOCUS_MORE,
    VFH_OSD_FOCUS_INFORMATION,
    VFH_OSD_FOCUS_PROGRESS,
} vfh_osd_focus;

typedef enum {
    VFH_OSD_SUBMENU_NONE = 0,
    VFH_OSD_SUBMENU_QUEUE,
    VFH_OSD_SUBMENU_SUBTITLES,
    VFH_OSD_SUBMENU_ASPECT,
    VFH_OSD_SUBMENU_MORE,
} vfh_osd_submenu;

typedef struct {
    vfh_osd_state state;
    vfh_osd_focus focus;
    vfh_osd_submenu submenu;
    uint32_t transient_until_ms;
    bool queue_available;
    bool subtitles_available;
    bool more_available;
} vfh_osd;

void vfh_osd_init(vfh_osd *osd);
void vfh_osd_tick(vfh_osd *osd, uint32_t now_ms);
bool vfh_osd_visible(const vfh_osd *osd);

/* Direct transport actions reveal the OSD briefly without stealing focus. */
void vfh_osd_flash(vfh_osd *osd, uint32_t now_ms);

/* Y opens a focused OSD from hidden/transient and closes a pinned/submenu OSD. */
void vfh_osd_toggle_pinned(vfh_osd *osd);

/* The second row is capability-driven. Aspect and Video Information are
 * always present; queue, subtitles, and chapters are only focusable when the
 * current playback state can actually service them. */
void vfh_osd_set_capabilities(vfh_osd *osd, bool queue_available,
                              bool subtitles_available, bool more_available);
int vfh_osd_row_two_focuses(const vfh_osd *osd, vfh_osd_focus *out, int out_count);

/* Moves focus only while the main OSD surface is pinned. */
void vfh_osd_move(vfh_osd *osd, int horizontal, int vertical);

/* Entering a submenu preserves the focused control for the return journey. */
bool vfh_osd_open_submenu(vfh_osd *osd, vfh_osd_submenu submenu);

/* B returns from a submenu, then unpins the OSD. Returns whether it handled B. */
bool vfh_osd_back(vfh_osd *osd);

const char *vfh_osd_focus_label(vfh_osd_focus focus);

#endif /* VFH_OSD_H */
