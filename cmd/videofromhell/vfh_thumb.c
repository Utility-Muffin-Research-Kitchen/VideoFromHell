#include "vfh_thumb.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <SDL_image.h>

#include "vfh_art.h"
#include "vfh_media.h"

void vfh_thumb_cache_path(const char *video_path, char *out, int out_size) {
    vfh_art_cache_path(video_path, out, out_size);
}

static void *vfh_thumb_worker_main(void *opaque) {
    vfh_thumb_worker *worker = opaque;
    while (true) {
        char path[VFH_THUMB_PATH_MAX];
        char cache[VFH_THUMB_PATH_MAX];
        unsigned long sequence;
        pthread_mutex_lock(&worker->mutex);
        while (!worker->stop && !worker->pending)
            pthread_cond_wait(&worker->ready, &worker->mutex);
        if (worker->stop) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }
        snprintf(path, sizeof(path), "%s", worker->requested_path);
        snprintf(cache, sizeof(cache), "%s", worker->requested_cache);
        sequence = worker->sequence;
        worker->pending = false;
        worker->busy = true;
        pthread_mutex_unlock(&worker->mutex);

        SDL_Surface *surface = NULL;
        vfh_thumb_state state = VFH_THUMB_ERROR;
        char sidecar[VFH_THUMB_PATH_MAX];
        bool failed_as_error = false;
        if (vfh_art_find_sidecar(path, sidecar, sizeof(sidecar))) {
            surface = IMG_Load(sidecar);
            state = surface ? VFH_THUMB_READY : VFH_THUMB_ERROR;
        } else if (vfh_art_failure_exists(cache, &failed_as_error)) {
            state = failed_as_error ? VFH_THUMB_ERROR : VFH_THUMB_NOT_FOUND;
        } else if (access(cache, R_OK) == 0) {
            surface = IMG_Load(cache);
            state = surface ? VFH_THUMB_READY : VFH_THUMB_ERROR;
            if (!surface) (void)vfh_art_write_failure(cache, true);
        } else {
            vfh_media_poster_status media_state = VFH_MEDIA_POSTER_ERROR;
            surface = vfh_media_decode_poster(path, 260, &media_state);
            if (surface) {
                state = VFH_THUMB_READY;
                if (vfh_art_prepare_cache_dir()) (void)IMG_SavePNG(surface, cache);
            } else {
                state = media_state == VFH_MEDIA_POSTER_NOT_FOUND
                    ? VFH_THUMB_NOT_FOUND : VFH_THUMB_ERROR;
                (void)vfh_art_write_failure(cache, state == VFH_THUMB_ERROR);
            }
        }

        pthread_mutex_lock(&worker->mutex);
        worker->busy = false;
        if (sequence == worker->sequence) {
            if (surface) {
                if (worker->complete_surface) SDL_FreeSurface(worker->complete_surface);
                worker->complete_surface = surface;
                worker->complete_sequence = sequence;
            }
            worker->state = state;
        } else if (surface) {
            SDL_FreeSurface(surface);
        }
        pthread_mutex_unlock(&worker->mutex);
    }
    return NULL;
}

bool vfh_thumb_worker_init(vfh_thumb_worker *worker) {
    if (!worker) return false;
    memset(worker, 0, sizeof(*worker));
    if (pthread_mutex_init(&worker->mutex, NULL) != 0) return false;
    if (pthread_cond_init(&worker->ready, NULL) != 0) {
        pthread_mutex_destroy(&worker->mutex);
        return false;
    }
    if (pthread_create(&worker->thread, NULL, vfh_thumb_worker_main, worker) != 0) {
        pthread_cond_destroy(&worker->ready);
        pthread_mutex_destroy(&worker->mutex);
        return false;
    }
    return true;
}

void vfh_thumb_worker_destroy(vfh_thumb_worker *worker) {
    if (!worker) return;
    pthread_mutex_lock(&worker->mutex);
    worker->stop = true;
    pthread_cond_signal(&worker->ready);
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);
    if (worker->complete_surface) SDL_FreeSurface(worker->complete_surface);
    pthread_cond_destroy(&worker->ready);
    pthread_mutex_destroy(&worker->mutex);
    memset(worker, 0, sizeof(*worker));
}

void vfh_thumb_worker_request(vfh_thumb_worker *worker, const char *video_path) {
    if (!worker || !video_path || !video_path[0]) return;
    char cache[VFH_THUMB_PATH_MAX];
    vfh_thumb_cache_path(video_path, cache, sizeof(cache));
    pthread_mutex_lock(&worker->mutex);
    if (strcmp(worker->requested_path, video_path) != 0) {
        worker->sequence++;
        if (worker->complete_surface) {
            SDL_FreeSurface(worker->complete_surface);
            worker->complete_surface = NULL;
        }
        snprintf(worker->requested_path, sizeof(worker->requested_path), "%s", video_path);
        snprintf(worker->requested_cache, sizeof(worker->requested_cache), "%s", cache);
        worker->pending = true;
        worker->state = VFH_THUMB_PENDING;
        pthread_cond_signal(&worker->ready);
    }
    pthread_mutex_unlock(&worker->mutex);
}

void vfh_thumb_worker_retry(vfh_thumb_worker *worker, const char *video_path) {
    if (!worker || !video_path || !video_path[0]) return;
    char cache[VFH_THUMB_PATH_MAX];
    vfh_thumb_cache_path(video_path, cache, sizeof(cache));
    vfh_art_clear_failures(cache);
    pthread_mutex_lock(&worker->mutex);
    worker->sequence++;
    if (worker->complete_surface) {
        SDL_FreeSurface(worker->complete_surface);
        worker->complete_surface = NULL;
    }
    snprintf(worker->requested_path, sizeof(worker->requested_path), "%s", video_path);
    snprintf(worker->requested_cache, sizeof(worker->requested_cache), "%s", cache);
    worker->pending = true;
    worker->state = VFH_THUMB_PENDING;
    pthread_cond_signal(&worker->ready);
    pthread_mutex_unlock(&worker->mutex);
}

SDL_Surface *vfh_thumb_worker_take(vfh_thumb_worker *worker, const char *video_path) {
    if (!worker || !video_path) return NULL;
    SDL_Surface *surface = NULL;
    pthread_mutex_lock(&worker->mutex);
    if (strcmp(worker->requested_path, video_path) == 0 &&
        worker->complete_sequence == worker->sequence) {
        surface = worker->complete_surface;
        worker->complete_surface = NULL;
    }
    pthread_mutex_unlock(&worker->mutex);
    return surface;
}

vfh_thumb_state vfh_thumb_worker_state(vfh_thumb_worker *worker, const char *video_path) {
    if (!worker || !video_path) return VFH_THUMB_IDLE;
    pthread_mutex_lock(&worker->mutex);
    vfh_thumb_state state = strcmp(worker->requested_path, video_path) == 0
        ? worker->state : VFH_THUMB_IDLE;
    pthread_mutex_unlock(&worker->mutex);
    return state;
}
