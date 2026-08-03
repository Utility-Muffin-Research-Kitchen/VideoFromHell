#ifndef VFH_LIBRARY_H
#define VFH_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vfh_sources.h"

/* This is deliberately a small filesystem index, not a media database. It is
 * bounded well above the expected card library size so a malformed directory
 * tree cannot consume unbounded memory on the device. */
#define VFH_LIBRARY_MAX_RECORDS 4096
#define VFH_LIBRARY_RELATIVE_PATH_MAX 768
#define VFH_LIBRARY_TITLE_MAX 256
#define VFH_LIBRARY_LABEL_MAX 64

typedef enum {
    VFH_CONTENT_VIDEO = 0,
    VFH_CONTENT_RECORDING = 1,
} vfh_content_kind;

typedef enum {
    VFH_LIBRARY_ART_UNKNOWN = 0,
    VFH_LIBRARY_ART_SIDECAR,
    VFH_LIBRARY_ART_EMBEDDED,
    VFH_LIBRARY_ART_GENERATED,
} vfh_library_art_source;

typedef struct {
    vfh_content_kind content_kind;
    int source_index;                 /* VIDEO_PATHS slot; recording uses 0 */
    bool available;                   /* known openable during the latest scan */
    uint64_t size;
    int64_t mtime;
    int64_t first_seen;
    /* Recorder filename timestamp, encoded as YYYYMMDDhhmmss (or zero when
     * the capture name has no recognizable timestamp).  Keeping it as a
     * local wall-clock value avoids inventing a timezone for RetroArch's
     * filename convention while remaining naturally sortable. */
    int64_t capture_timestamp;
    double duration;
    int year;
    int recording_part;               /* 0 for ordinary files, otherwise -partN */
    unsigned seen_generation;         /* internal scan bookkeeping */
    bool metadata_ready;              /* exact size/mtime FFmpeg probe completed */
    bool nfo_title;
    bool nfo_year;
    bool has_embedded_art;
    vfh_library_art_source art_source;
    char relative_path[VFH_LIBRARY_RELATIVE_PATH_MAX];
    char display_title[VFH_LIBRARY_TITLE_MAX];
    char container[VFH_LIBRARY_LABEL_MAX];
    char video_codec[VFH_LIBRARY_LABEL_MAX];
    char audio_codec[VFH_LIBRARY_LABEL_MAX];
} vfh_library_item;

typedef struct {
    vfh_library_item *items;
    size_t count;
    size_t capacity;
    unsigned scan_generation;
} vfh_library;

/* The catalog worker polls this between directory entries and media records.
 * A cancelled scan never publishes its partial result, so callers retain their
 * previous usable catalog rather than briefly showing a half-enumerated card. */
typedef bool (*vfh_library_cancelled_fn)(void *opaque);

typedef enum {
    VFH_LIBRARY_SCAN_COMPLETE = 0,
    VFH_LIBRARY_SCAN_CANCELLED,
    VFH_LIBRARY_SCAN_FAILED,
} vfh_library_scan_result;

void vfh_library_init(vfh_library *library);
void vfh_library_destroy(vfh_library *library);

/* `recordings_path` is the singular primary-owned RECORDINGS_PATH resolution.
 * It may name a directory that does not exist yet; that is a successful empty
 * source rather than a launch error. The scan never probes media; the main
 * thread delegates exact size/mtime cache misses to its catalog worker. */
bool vfh_library_scan(vfh_library *library, const vfh_sources *video_sources,
                      const char *recordings_path, char *error, size_t error_size);

/* Cancellable transactional form used by the background catalog worker. */
vfh_library_scan_result vfh_library_scan_cancellable(
    vfh_library *library, const vfh_sources *video_sources,
    const char *recordings_path, vfh_library_cancelled_fn cancelled,
    void *cancel_opaque, char *error, size_t error_size);

/* Versioned, bounded, FAT-safe cache. Corrupt or an unknown-major store is
 * discarded in memory and reported as false; callers can immediately rebuild. */
bool vfh_library_load(vfh_library *library);
bool vfh_library_save(const vfh_library *library);

const vfh_library_item *vfh_library_find(const vfh_library *library,
                                         vfh_content_kind content_kind,
                                         int source_index,
                                         const char *relative_path);

/* Rebuild a playable absolute path from the durable source-relative identity. */
bool vfh_library_resolve_path(const vfh_library_item *item,
                              const vfh_sources *video_sources,
                              const char *recordings_path,
                              char *out, size_t out_size);

#endif
