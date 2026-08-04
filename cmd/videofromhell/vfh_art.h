#ifndef VFH_ART_H
#define VFH_ART_H

#include <stdbool.h>

#define VFH_ART_PATH_MAX 1200

/* The cache directory is intentionally versioned.  A thumbnail produced by an
 * older frame-selection algorithm must never hide an improved replacement. */
void vfh_art_cache_path(const char *video_path, char *out, int out_size);
bool vfh_art_prepare_cache_dir(void);

/* Local artwork is deliberately narrow and deterministic: an exact-stem image
 * wins; a directory poster is used only when the directory has one video. */
bool vfh_art_find_sidecar(const char *video_path, char *out, int out_size);
bool vfh_art_find_folder_art(const char *directory, char *out, int out_size);

/* Failure markers are keyed by the same size/mtime-sensitive cache key. */
void vfh_art_failure_path(const char *cache_path, bool error, char *out, int out_size);
bool vfh_art_failure_exists(const char *cache_path, bool *out_error);
bool vfh_art_write_failure(const char *cache_path, bool error);
void vfh_art_clear_failures(const char *cache_path);

#endif
