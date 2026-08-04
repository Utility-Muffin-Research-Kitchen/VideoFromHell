#include "vfh_art.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VFH_ART_CACHE_VERSION "thumbs-v2"
/* Bump this whenever frame selection or the dark-frame test changes.  The
 * directory remains stable while stale generated frames become unreachable. */
#define VFH_ART_CACHE_KEY_VERSION "frame-seek-10-dark-25-v6"

static uint64_t vfh_art_hash_bytes(uint64_t hash, const void *data, size_t count) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < count; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void vfh_art_cache_dir(char *out, int out_size) {
    const char *userdata = getenv("USERDATA_PATH");
    if (!userdata || !userdata[0]) userdata = ".userdata";
    snprintf(out, (size_t)out_size, "%s/VideoFromHell/%s", userdata, VFH_ART_CACHE_VERSION);
}

static void vfh_art_stat_mtime(const struct stat *st, time_t *seconds, long *nanoseconds) {
    if (!st || !seconds || !nanoseconds) return;
#if defined(__APPLE__)
    *seconds = st->st_mtimespec.tv_sec;
    *nanoseconds = st->st_mtimespec.tv_nsec;
#else
    *seconds = st->st_mtim.tv_sec;
    *nanoseconds = st->st_mtim.tv_nsec;
#endif
}

void vfh_art_cache_path(const char *video_path, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (video_path) (void)stat(video_path, &st);
    time_t mtime_seconds = 0;
    long mtime_nanoseconds = 0;
    vfh_art_stat_mtime(&st, &mtime_seconds, &mtime_nanoseconds);
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = vfh_art_hash_bytes(hash, VFH_ART_CACHE_VERSION, sizeof(VFH_ART_CACHE_VERSION) - 1);
    hash = vfh_art_hash_bytes(hash, VFH_ART_CACHE_KEY_VERSION,
                              sizeof(VFH_ART_CACHE_KEY_VERSION) - 1);
    hash = vfh_art_hash_bytes(hash, video_path ? video_path : "",
                              video_path ? strlen(video_path) : 0);
    hash = vfh_art_hash_bytes(hash, &st.st_size, sizeof(st.st_size));
    hash = vfh_art_hash_bytes(hash, &mtime_seconds, sizeof(mtime_seconds));
    hash = vfh_art_hash_bytes(hash, &mtime_nanoseconds, sizeof(mtime_nanoseconds));
    char directory[VFH_ART_PATH_MAX];
    vfh_art_cache_dir(directory, sizeof(directory));
    snprintf(out, (size_t)out_size, "%s/%016llx.png", directory,
             (unsigned long long)hash);
}

static bool vfh_art_is_video_name(const char *name) {
    static const char *const extensions[] = {
        ".mp4", ".m4v", ".mkv", ".mov", ".avi", ".webm", ".ts",
        ".m2ts", ".mts", ".mpg", ".mpeg", ".3gp", ".flv",
    };
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
        if (!strcasecmp(dot, extensions[i])) return true;
    return false;
}

static bool vfh_art_regular_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static void vfh_art_split_video_path(const char *video_path, char *directory, int directory_size,
                                     char *stem, int stem_size) {
    if (directory && directory_size > 0) directory[0] = '\0';
    if (stem && stem_size > 0) stem[0] = '\0';
    if (!video_path) return;
    const char *base = strrchr(video_path, '/');
    if (base) {
        size_t directory_length = (size_t)(base - video_path);
        if (directory_length == 0) directory_length = 1;
        if (directory_length >= (size_t)directory_size) return;
        memcpy(directory, video_path, directory_length);
        directory[directory_length] = '\0';
        base++;
    } else {
        snprintf(directory, (size_t)directory_size, ".");
        base = video_path;
    }
    const char *dot = strrchr(base, '.');
    size_t length = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (length >= (size_t)stem_size) return;
    memcpy(stem, base, length);
    stem[length] = '\0';
}

static bool vfh_art_find_named_image(const char *directory, const char *name,
                                     char *out, int out_size) {
    static const char *const extensions[] = { ".jpg", ".jpeg", ".png" };
    if (!directory || !name || !out || out_size <= 0) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        int length = snprintf(out, (size_t)out_size, "%s/%s%s", directory, name, extensions[i]);
        if (length > 0 && length < out_size && vfh_art_regular_file(out)) return true;
    }
    out[0] = '\0';
    return false;
}

static int vfh_art_video_count(const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (vfh_art_is_video_name(entry->d_name) && ++count > 1) break;
    }
    closedir(dir);
    return count;
}

bool vfh_art_find_sidecar(const char *video_path, char *out, int out_size) {
    char directory[VFH_ART_PATH_MAX], stem[VFH_ART_PATH_MAX];
    vfh_art_split_video_path(video_path, directory, sizeof(directory), stem, sizeof(stem));
    if (!directory[0] || !stem[0]) return false;
    if (vfh_art_find_named_image(directory, stem, out, out_size)) return true;
    if (vfh_art_video_count(directory) == 1)
        return vfh_art_find_named_image(directory, "poster", out, out_size);
    return false;
}

bool vfh_art_find_folder_art(const char *directory, char *out, int out_size) {
    if (!directory || !directory[0]) return false;
    if (vfh_art_find_named_image(directory, "poster", out, out_size)) return true;
    return vfh_art_find_named_image(directory, "folder", out, out_size);
}

void vfh_art_failure_path(const char *cache_path, bool error, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    snprintf(out, (size_t)out_size, "%s.%s", cache_path ? cache_path : "",
             error ? "error" : "none");
}

bool vfh_art_failure_exists(const char *cache_path, bool *out_error) {
    char marker[VFH_ART_PATH_MAX];
    vfh_art_failure_path(cache_path, false, marker, sizeof(marker));
    if (access(marker, R_OK) == 0) {
        if (out_error) *out_error = false;
        return true;
    }
    vfh_art_failure_path(cache_path, true, marker, sizeof(marker));
    if (access(marker, R_OK) == 0) {
        if (out_error) *out_error = true;
        return true;
    }
    return false;
}

bool vfh_art_prepare_cache_dir(void) {
    const char *userdata = getenv("USERDATA_PATH");
    if (!userdata || !userdata[0]) userdata = ".userdata";
    char app_dir[VFH_ART_PATH_MAX], cache_dir[VFH_ART_PATH_MAX];
    int app_length = snprintf(app_dir, sizeof(app_dir), "%s/VideoFromHell", userdata);
    int cache_length = snprintf(cache_dir, sizeof(cache_dir), "%s/%s", app_dir, VFH_ART_CACHE_VERSION);
    if (app_length < 0 || app_length >= (int)sizeof(app_dir) ||
        cache_length < 0 || cache_length >= (int)sizeof(cache_dir)) return false;
    if (mkdir(userdata, 0755) != 0 && errno != EEXIST) return false;
    if (mkdir(app_dir, 0755) != 0 && errno != EEXIST) return false;
    return mkdir(cache_dir, 0755) == 0 || errno == EEXIST;
}

bool vfh_art_write_failure(const char *cache_path, bool error) {
    if (!cache_path || !cache_path[0]) return false;
    if (!vfh_art_prepare_cache_dir()) return false;
    char marker[VFH_ART_PATH_MAX];
    vfh_art_failure_path(cache_path, error, marker, sizeof(marker));
    FILE *file = fopen(marker, "w");
    if (!file) return false;
    bool ok = fputs(error ? "error\n" : "not-found\n", file) >= 0 && fclose(file) == 0;
    if (!ok) (void)unlink(marker);
    return ok;
}

void vfh_art_clear_failures(const char *cache_path) {
    char marker[VFH_ART_PATH_MAX];
    vfh_art_failure_path(cache_path, false, marker, sizeof(marker));
    (void)unlink(marker);
    vfh_art_failure_path(cache_path, true, marker, sizeof(marker));
    (void)unlink(marker);
}
