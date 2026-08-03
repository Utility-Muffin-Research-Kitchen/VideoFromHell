#ifndef VFH_THUMB_H
#define VFH_THUMB_H

#include <stdbool.h>
#include <pthread.h>

#include <SDL.h>

#define VFH_THUMB_PATH_MAX 1200

typedef enum {
    VFH_THUMB_IDLE,
    VFH_THUMB_PENDING,
    VFH_THUMB_READY,
    VFH_THUMB_NOT_FOUND,
    VFH_THUMB_ERROR,
} vfh_thumb_state;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    bool stop;
    bool pending;
    bool busy;
    vfh_thumb_state state;
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
void vfh_thumb_worker_retry(vfh_thumb_worker *worker, const char *video_path);
SDL_Surface *vfh_thumb_worker_take(vfh_thumb_worker *worker, const char *video_path);
vfh_thumb_state vfh_thumb_worker_state(vfh_thumb_worker *worker, const char *video_path);

#endif
