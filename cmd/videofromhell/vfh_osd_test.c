#include "vfh_osd.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    const char *name;
    vfh_osd_focus start;
    int horizontal;
    int vertical;
    vfh_osd_focus expected;
} vfh_osd_move_case;

static void vfh_test_visibility_and_pinning(void) {
    vfh_osd osd;
    vfh_osd_init(&osd);
    assert(osd.state == VFH_OSD_HIDDEN);
    assert(!vfh_osd_visible(&osd));

    vfh_osd_flash(&osd, 1000);
    assert(osd.state == VFH_OSD_TRANSIENT);
    assert(osd.transient_until_ms == 4000);
    assert(vfh_osd_visible(&osd));
    vfh_osd_tick(&osd, 3999);
    assert(osd.state == VFH_OSD_TRANSIENT);
    vfh_osd_tick(&osd, 4000);
    assert(osd.state == VFH_OSD_HIDDEN);

    vfh_osd_toggle_pinned(&osd);
    assert(osd.state == VFH_OSD_PINNED);
    assert(osd.focus == VFH_OSD_FOCUS_PLAY_PAUSE);
    vfh_osd_flash(&osd, 6000); /* direct transport does not steal pinned focus */
    assert(osd.state == VFH_OSD_PINNED);
    vfh_osd_toggle_pinned(&osd);
    assert(osd.state == VFH_OSD_HIDDEN);

    vfh_osd_flash(&osd, 7000);
    vfh_osd_toggle_pinned(&osd);
    assert(osd.state == VFH_OSD_PINNED);
    assert(osd.focus == VFH_OSD_FOCUS_PLAY_PAUSE);
}

static void vfh_test_focus_map(void) {
    static const vfh_osd_move_case cases[] = {
        { "left edge", VFH_OSD_FOCUS_PREVIOUS, -1, 0, VFH_OSD_FOCUS_PREVIOUS },
        { "row one right", VFH_OSD_FOCUS_PREVIOUS, 1, 0, VFH_OSD_FOCUS_REWIND },
        { "row one down", VFH_OSD_FOCUS_REWIND, 0, 1, VFH_OSD_FOCUS_SUBTITLES },
        { "row two up", VFH_OSD_FOCUS_MORE, 0, -1, VFH_OSD_FOCUS_FORWARD },
        { "right edge", VFH_OSD_FOCUS_INFORMATION, 1, 0, VFH_OSD_FOCUS_INFORMATION },
        { "progress up", VFH_OSD_FOCUS_PLAY_PAUSE, 0, -1, VFH_OSD_FOCUS_PROGRESS },
        { "progress down", VFH_OSD_FOCUS_PROGRESS, 0, 1, VFH_OSD_FOCUS_PLAY_PAUSE },
        { "progress keeps scrub", VFH_OSD_FOCUS_PROGRESS, 1, 0, VFH_OSD_FOCUS_PROGRESS },
    };
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vfh_osd osd;
        vfh_osd_init(&osd);
        vfh_osd_toggle_pinned(&osd);
        osd.focus = cases[i].start;
        vfh_osd_move(&osd, cases[i].horizontal, cases[i].vertical);
        if (osd.focus != cases[i].expected) {
            fprintf(stderr, "%s: got %s, expected %s\n", cases[i].name,
                    vfh_osd_focus_label(osd.focus), vfh_osd_focus_label(cases[i].expected));
            assert(0);
        }
    }
}

static void vfh_test_submenu_back_stack(void) {
    vfh_osd osd;
    vfh_osd_init(&osd);
    vfh_osd_toggle_pinned(&osd);
    osd.focus = VFH_OSD_FOCUS_SUBTITLES;
    assert(vfh_osd_open_submenu(&osd, VFH_OSD_SUBMENU_SUBTITLES));
    assert(osd.state == VFH_OSD_SUBMENU);
    assert(osd.submenu == VFH_OSD_SUBMENU_SUBTITLES);
    assert(vfh_osd_back(&osd));
    assert(osd.state == VFH_OSD_PINNED);
    assert(osd.submenu == VFH_OSD_SUBMENU_NONE);
    assert(vfh_osd_back(&osd));
    assert(osd.state == VFH_OSD_HIDDEN);
    assert(!vfh_osd_back(&osd));
}

int main(void) {
    vfh_test_visibility_and_pinning();
    vfh_test_focus_map();
    vfh_test_submenu_back_stack();
    puts("vfh osd tests passed");
    return 0;
}
