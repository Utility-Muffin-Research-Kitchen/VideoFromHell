#include "vfh_osd.h"

#include <stddef.h>

static bool vfh_osd_deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    /* SDL_GetTicks-style clocks wrap. Signed subtraction makes deadlines work
       on both sides of that wrap as long as intervals stay below 24 days. */
    return deadline_ms != 0 && (int32_t)(now_ms - deadline_ms) >= 0;
}

static int vfh_osd_row(const vfh_osd *osd, vfh_osd_focus focus) {
    if (focus >= VFH_OSD_FOCUS_PREVIOUS && focus <= VFH_OSD_FOCUS_NEXT) return 0;
    vfh_osd_focus row_two[5];
    int count = vfh_osd_row_two_focuses(osd, row_two, 5);
    for (int i = 0; i < count; i++)
        if (row_two[i] == focus) return 1;
    return -1;
}

static int vfh_osd_column(const vfh_osd *osd, vfh_osd_focus focus) {
    if (focus >= VFH_OSD_FOCUS_PREVIOUS && focus <= VFH_OSD_FOCUS_NEXT)
        return (int)focus - (int)VFH_OSD_FOCUS_PREVIOUS;
    vfh_osd_focus row_two[5];
    int count = vfh_osd_row_two_focuses(osd, row_two, 5);
    for (int i = 0; i < count; i++)
        if (row_two[i] == focus) return i;
    return 2; /* progress returns to the central Play/Pause control */
}

static vfh_osd_focus vfh_osd_from_row_column(const vfh_osd *osd, int row, int column) {
    if (row == 0) return (vfh_osd_focus)((int)VFH_OSD_FOCUS_PREVIOUS + column);
    vfh_osd_focus row_two[5];
    int count = vfh_osd_row_two_focuses(osd, row_two, 5);
    if (count <= 0) return VFH_OSD_FOCUS_PLAY_PAUSE;
    if (column < 0) column = 0;
    if (column >= count) column = count - 1;
    return row_two[column];
}

void vfh_osd_init(vfh_osd *osd) {
    if (!osd) return;
    osd->state = VFH_OSD_HIDDEN;
    osd->focus = VFH_OSD_FOCUS_PLAY_PAUSE;
    osd->submenu = VFH_OSD_SUBMENU_NONE;
    osd->transient_until_ms = 0;
    /* Conservative defaults retain the full graph until playback publishes
     * its real capabilities. This keeps a newly pinned OSD usable. */
    osd->queue_available = true;
    osd->subtitles_available = true;
    osd->more_available = true;
}

void vfh_osd_tick(vfh_osd *osd, uint32_t now_ms) {
    if (!osd) return;
    if (osd->state == VFH_OSD_TRANSIENT &&
        vfh_osd_deadline_reached(now_ms, osd->transient_until_ms)) {
        osd->state = VFH_OSD_HIDDEN;
        osd->transient_until_ms = 0;
    }
}

bool vfh_osd_visible(const vfh_osd *osd) {
    return osd && osd->state != VFH_OSD_HIDDEN;
}

void vfh_osd_flash(vfh_osd *osd, uint32_t now_ms) {
    if (!osd || osd->state == VFH_OSD_PINNED || osd->state == VFH_OSD_SUBMENU) return;
    osd->state = VFH_OSD_TRANSIENT;
    osd->transient_until_ms = now_ms + VFH_OSD_LINGER_MS;
    /* Zero denotes an inactive deadline. It is an exceedingly rare wrap case,
       but handling it keeps the state model internally consistent. */
    if (osd->transient_until_ms == 0) osd->transient_until_ms = 1;
}

void vfh_osd_toggle_pinned(vfh_osd *osd) {
    if (!osd) return;
    if (osd->state == VFH_OSD_PINNED || osd->state == VFH_OSD_SUBMENU) {
        osd->state = VFH_OSD_HIDDEN;
        osd->submenu = VFH_OSD_SUBMENU_NONE;
        osd->transient_until_ms = 0;
        return;
    }
    osd->state = VFH_OSD_PINNED;
    osd->focus = VFH_OSD_FOCUS_PLAY_PAUSE;
    osd->submenu = VFH_OSD_SUBMENU_NONE;
    osd->transient_until_ms = 0;
}

int vfh_osd_row_two_focuses(const vfh_osd *osd, vfh_osd_focus *out, int out_count) {
    const bool queue = !osd || osd->queue_available;
    const bool subtitles = !osd || osd->subtitles_available;
    const bool more = !osd || osd->more_available;
    vfh_osd_focus all[5];
    int count = 0;
    if (queue) all[count++] = VFH_OSD_FOCUS_QUEUE;
    if (subtitles) all[count++] = VFH_OSD_FOCUS_SUBTITLES;
    all[count++] = VFH_OSD_FOCUS_ASPECT;
    if (more) all[count++] = VFH_OSD_FOCUS_MORE;
    all[count++] = VFH_OSD_FOCUS_INFORMATION;
    if (out && out_count > 0) {
        int copied = count < out_count ? count : out_count;
        for (int i = 0; i < copied; i++) out[i] = all[i];
    }
    return count;
}

void vfh_osd_set_capabilities(vfh_osd *osd, bool queue_available,
                              bool subtitles_available, bool more_available) {
    if (!osd) return;
    osd->queue_available = queue_available;
    osd->subtitles_available = subtitles_available;
    osd->more_available = more_available;
    if (vfh_osd_row(osd, osd->focus) == 1) return;
    if (osd->focus >= VFH_OSD_FOCUS_QUEUE && osd->focus <= VFH_OSD_FOCUS_INFORMATION)
        osd->focus = VFH_OSD_FOCUS_PLAY_PAUSE;
}

void vfh_osd_move(vfh_osd *osd, int horizontal, int vertical) {
    if (!osd || osd->state != VFH_OSD_PINNED) return;

    if (horizontal) {
        int row = vfh_osd_row(osd, osd->focus);
        int column = vfh_osd_column(osd, osd->focus);
        if (row < 0) return;  /* progress owns horizontal input for scrubbing */
        column += horizontal < 0 ? -1 : 1;
        if (column < 0) column = 0;
        int columns = row == 0 ? 5 : vfh_osd_row_two_focuses(osd, NULL, 0);
        if (column >= columns) column = columns - 1;
        osd->focus = vfh_osd_from_row_column(osd, row, column);
        return;
    }

    if (!vertical) return;
    if (osd->focus == VFH_OSD_FOCUS_PROGRESS) {
        if (vertical > 0) osd->focus = VFH_OSD_FOCUS_PLAY_PAUSE;
        return;
    }

    int row = vfh_osd_row(osd, osd->focus);
    int column = vfh_osd_column(osd, osd->focus);
    if (vertical < 0 && row == 0) osd->focus = VFH_OSD_FOCUS_PROGRESS;
    else if (vertical > 0 && row == 0) osd->focus = vfh_osd_from_row_column(osd, 1, column);
    else if (vertical < 0 && row == 1) osd->focus = vfh_osd_from_row_column(osd, 0, column);
}

bool vfh_osd_open_submenu(vfh_osd *osd, vfh_osd_submenu submenu) {
    if (!osd || osd->state != VFH_OSD_PINNED || submenu == VFH_OSD_SUBMENU_NONE) return false;
    osd->state = VFH_OSD_SUBMENU;
    osd->submenu = submenu;
    return true;
}

bool vfh_osd_back(vfh_osd *osd) {
    if (!osd) return false;
    if (osd->state == VFH_OSD_SUBMENU) {
        osd->state = VFH_OSD_PINNED;
        osd->submenu = VFH_OSD_SUBMENU_NONE;
        return true;
    }
    if (osd->state == VFH_OSD_PINNED) {
        osd->state = VFH_OSD_HIDDEN;
        osd->submenu = VFH_OSD_SUBMENU_NONE;
        return true;
    }
    return false;
}

const char *vfh_osd_focus_label(vfh_osd_focus focus) {
    switch (focus) {
        case VFH_OSD_FOCUS_PREVIOUS:   return "Previous";
        case VFH_OSD_FOCUS_REWIND:     return "Rewind";
        case VFH_OSD_FOCUS_PLAY_PAUSE: return "Play/Pause";
        case VFH_OSD_FOCUS_FORWARD:    return "Forward";
        case VFH_OSD_FOCUS_NEXT:       return "Next";
        case VFH_OSD_FOCUS_QUEUE:      return "Queue";
        case VFH_OSD_FOCUS_SUBTITLES:  return "Subtitles";
        case VFH_OSD_FOCUS_ASPECT:     return "Aspect";
        case VFH_OSD_FOCUS_MORE:       return "More";
        case VFH_OSD_FOCUS_INFORMATION:return "Video Information";
        case VFH_OSD_FOCUS_PROGRESS:   return "Progress";
    }
    return "";
}
