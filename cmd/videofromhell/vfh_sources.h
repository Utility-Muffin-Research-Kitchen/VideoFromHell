#ifndef VFH_SOURCES_H
#define VFH_SOURCES_H

#include <stddef.h>

#define VFH_SOURCE_MAX 8
#define VFH_SOURCE_PATH_MAX 1024

/* Why a source cannot be browsed. "Card not mounted" and "card mounted but has
   no Videos folder" are different problems with different fixes, and telling the
   user the wrong one sends them looking for a hardware fault that isn't there. */
typedef enum {
    VFH_SOURCE_OK = 0,        /* card present, Videos/ exists */
    VFH_SOURCE_NO_CARD,       /* the card itself is not mounted */
    VFH_SOURCE_NO_FOLDER      /* card is mounted, Videos/ is missing */
} vfh_source_state;

typedef struct {
    char id[32];
    char label[16];
    char root[VFH_SOURCE_PATH_MAX];
    char card_root[VFH_SOURCE_PATH_MAX];  /* the card mount point, "" if unknown */
    int available;                        /* == (state == VFH_SOURCE_OK) */
    vfh_source_state state;
} vfh_source;

/* Human-readable reason a source is unusable, or NULL when it is fine. */
const char *vfh_source_state_message(const vfh_source *source);

typedef struct {
    vfh_source items[VFH_SOURCE_MAX];
    int count;
} vfh_sources;

/* Resolve VIDEO_PATHS in order, falling back to VIDEO_PATH, SDCARD_PATH/Videos,
   then ./Videos. Existing roots are canonicalized with realpath; duplicates and
   malformed colon lists are rejected. When both plural variables are present,
   VIDEO_PATHS must contain one item per SDCARD_PATHS item. */
int vfh_sources_resolve(vfh_sources *out, char *error, size_t error_size);
int vfh_sources_parse(vfh_sources *out, const char *video_paths,
                      char *error, size_t error_size);

/* Always return one primary source even if VIDEO_PATHS is malformed, allowing
   the browser to show a degraded empty library instead of refusing to launch. */
void vfh_sources_single_fallback(vfh_sources *out);

#endif
