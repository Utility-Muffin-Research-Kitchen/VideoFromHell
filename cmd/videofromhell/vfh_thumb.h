#ifndef VFH_THUMB_H
#define VFH_THUMB_H

#include <stdbool.h>
#include <pthread.h>

#include <SDL.h>

#define VFH_THUMB_PATH_MAX 1200

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    bool stop;
    bool pending;
    bool busy;
    unsigned long sequence;
    unsigned long complete_sequence;
    char requested_path[VFH_THUMB_PATH_MAX];
    char requested_cache[VFH_THUMB_PATH_MAX];
    SDL_Surface *complete_surface;
} vfh_thumb_worker;

bool vfh_thumb_worker_init(vfh_thumb_worker *worker);
void vfh_thumb_worker_destroy(vfh_thumb_worker *worker);
void vfh_thumb_cache_path(const char *video_path, char *out, int out_size);
void vfh_thumb_worker_request(vfh_thumb_worker *worker, const char *video_path);
SDL_Surface *vfh_thumb_worker_take(vfh_thumb_worker *worker, const char *video_path);

#endif
