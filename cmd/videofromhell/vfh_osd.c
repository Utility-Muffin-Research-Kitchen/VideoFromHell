#include "vfh_osd.h"

static bool vfh_osd_deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    /* SDL_GetTicks-style clocks wrap. Signed subtraction makes deadlines work
       on both sides of that wrap as long as intervals stay below 24 days. */
    return deadline_ms != 0 && (int32_t)(now_ms - deadline_ms) >= 0;
}

static int vfh_osd_row(vfh_osd_focus focus) {
    if (focus >= VFH_OSD_FOCUS_PREVIOUS && focus <= VFH_OSD_FOCUS_NEXT) return 0;
    if (focus >= VFH_OSD_FOCUS_QUEUE && focus <= VFH_OSD_FOCUS_INFORMATION) return 1;
    return -1;
}

static int vfh_osd_column(vfh_osd_focus focus) {
    if (focus >= VFH_OSD_FOCUS_PREVIOUS && focus <= VFH_OSD_FOCUS_NEXT)
        return (int)focus - (int)VFH_OSD_FOCUS_PREVIOUS;
    if (focus >= VFH_OSD_FOCUS_QUEUE && focus <= VFH_OSD_FOCUS_INFORMATION)
        return (int)focus - (int)VFH_OSD_FOCUS_QUEUE;
    return 2; /* progress returns to the central Play/Pause control */
}

static vfh_osd_focus vfh_osd_from_row_column(int row, int column) {
    if (row == 0) return (vfh_osd_focus)((int)VFH_OSD_FOCUS_PREVIOUS + column);
    return (vfh_osd_focus)((int)VFH_OSD_FOCUS_QUEUE + column);
}

void vfh_osd_init(vfh_osd *osd) {
    if (!osd) return;
    osd->state = VFH_OSD_HIDDEN;
    osd->focus = VFH_OSD_FOCUS_PLAY_PAUSE;
    osd->submenu = VFH_OSD_SUBMENU_NONE;
    osd->transient_until_ms = 0;
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

void vfh_osd_move(vfh_osd *osd, int horizontal, int vertical) {
    if (!osd || osd->state != VFH_OSD_PINNED) return;

    if (horizontal) {
        int row = vfh_osd_row(osd->focus);
        int column = vfh_osd_column(osd->focus);
        if (row < 0) return;  /* progress owns horizontal input for scrubbing */
        column += horizontal < 0 ? -1 : 1;
        if (column < 0) column = 0;
        if (column > 4) column = 4;
        osd->focus = vfh_osd_from_row_column(row, column);
        return;
    }

    if (!vertical) return;
    if (osd->focus == VFH_OSD_FOCUS_PROGRESS) {
        if (vertical > 0) osd->focus = VFH_OSD_FOCUS_PLAY_PAUSE;
        return;
    }

    int row = vfh_osd_row(osd->focus);
    int column = vfh_osd_column(osd->focus);
    if (vertical < 0 && row == 0) osd->focus = VFH_OSD_FOCUS_PROGRESS;
    else if (vertical > 0 && row == 0) osd->focus = vfh_osd_from_row_column(1, column);
    else if (vertical < 0 && row == 1) osd->focus = vfh_osd_from_row_column(0, column);
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
