#include "vfh_thumb.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL_image.h>

#include "vfh_media.h"

static uint64_t vfh_thumb_hash_bytes(uint64_t hash, const void *data, size_t count) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < count; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void vfh_thumb_cache_dir(char *out, int out_size) {
    const char *userdata = getenv("USERDATA_PATH");
    if (!userdata || !userdata[0]) userdata = ".userdata";
    snprintf(out, (size_t)out_size, "%s/VideoFromHell/thumbs", userdata);
}

void vfh_thumb_cache_path(const char *video_path, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (video_path) stat(video_path, &st);
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = vfh_thumb_hash_bytes(hash, video_path ? video_path : "", video_path ? strlen(video_path) : 0);
    hash = vfh_thumb_hash_bytes(hash, &st.st_size, sizeof(st.st_size));
    hash = vfh_thumb_hash_bytes(hash, &st.st_mtim.tv_sec, sizeof(st.st_mtim.tv_sec));
    hash = vfh_thumb_hash_bytes(hash, &st.st_mtim.tv_nsec, sizeof(st.st_mtim.tv_nsec));
    char directory[VFH_THUMB_PATH_MAX];
    vfh_thumb_cache_dir(directory, sizeof(directory));
    snprintf(out, (size_t)out_size, "%s/%016llx.png", directory,
             (unsigned long long)hash);
}

static void vfh_thumb_ensure_cache_dir(void) {
    const char *userdata = getenv("USERDATA_PATH");
    if (!userdata || !userdata[0]) userdata = ".userdata";
    char app_dir[VFH_THUMB_PATH_MAX];
    char thumbs_dir[VFH_THUMB_PATH_MAX];
    int app_len = snprintf(app_dir, sizeof(app_dir), "%s/VideoFromHell", userdata);
    if (app_len < 0 || app_len >= (int)sizeof(app_dir)) return;
    int thumbs_len = snprintf(thumbs_dir, sizeof(thumbs_dir), "%s/thumbs", app_dir);
    if (thumbs_len < 0 || thumbs_len >= (int)sizeof(thumbs_dir)) return;
    if (mkdir(userdata, 0755) != 0 && errno != EEXIST) return;
    if (mkdir(app_dir, 0755) != 0 && errno != EEXIST) return;
    (void)mkdir(thumbs_dir, 0755);
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
        if (access(cache, R_OK) != 0) {
            surface = vfh_media_decode_poster(path, 260);
            if (surface) {
                vfh_thumb_ensure_cache_dir();
                (void)IMG_SavePNG(surface, cache);
            }
        }

        pthread_mutex_lock(&worker->mutex);
        worker->busy = false;
        if (sequence == worker->sequence && surface) {
            if (worker->complete_surface) SDL_FreeSurface(worker->complete_surface);
            worker->complete_surface = surface;
            worker->complete_sequence = sequence;
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
        snprintf(worker->requested_path, sizeof(worker->requested_path), "%s", video_path);
        snprintf(worker->requested_cache, sizeof(worker->requested_cache), "%s", cache);
        worker->pending = true;
        pthread_cond_signal(&worker->ready);
    }
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
