#ifndef VFH_MEDIA_H
#define VFH_MEDIA_H

#include <stdbool.h>

#include <SDL.h>

bool vfh_media_file_supported(const char *name);
bool vfh_media_probe_duration(const char *path, double *out_seconds);
void vfh_media_format_duration(double seconds, char *out, int out_size);

/* Decode one early video frame and scale it for a browser poster. The caller
   owns the returned surface. This function does not touch an SDL renderer. */
SDL_Surface *vfh_media_decode_poster(const char *path, int max_dimension);

#endif
