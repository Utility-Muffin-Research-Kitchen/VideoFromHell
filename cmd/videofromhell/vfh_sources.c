#include "vfh_sources.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void vfh_source_error(char *out, size_t n, const char *message) {
    if (out && n) snprintf(out, n, "%s", message);
}

static int vfh_source_normalize(const char *input, char *out, size_t n) {
    char resolved[PATH_MAX];
    const char *value = realpath(input, resolved) ? resolved : input;
    size_t value_len = strlen(value);
    if (value_len >= n) return 0;
    memcpy(out, value, value_len + 1);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    return 1;
}

static int vfh_source_list_count(const char *value, int *count,
                                 char *error, size_t error_size) {
    if (!value || !value[0] || !count) {
        vfh_source_error(error, error_size, "SDCARD_PATHS is empty");
        return 0;
    }
    int items = 1;
    for (const char *cursor = value; *cursor; cursor++) {
        if (*cursor != ':') continue;
        if (cursor == value || cursor[1] == '\0' || cursor[1] == ':') {
            vfh_source_error(error, error_size, "SDCARD_PATHS contains an empty item");
            return 0;
        }
        items++;
    }
    *count = items;
    return 1;
}

static int vfh_source_nth_path(const char *value, int index,
                               char *out, size_t out_size) {
    const char *start = value;
    for (int i = 0; i < index; i++) {
        start = strchr(start, ':');
        if (!start) return 0;
        start++;
    }
    const char *end = strchr(start, ':');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len == 0 || len >= out_size) return 0;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

#if defined(__linux__)
static int vfh_mount_field(char *out, size_t out_size, const char *raw) {
    size_t used = 0;
    for (size_t i = 0; raw[i]; i++) {
        unsigned char value = (unsigned char)raw[i];
        if (raw[i] == '\\' &&
            raw[i + 1] >= '0' && raw[i + 1] <= '7' &&
            raw[i + 2] >= '0' && raw[i + 2] <= '7' &&
            raw[i + 3] >= '0' && raw[i + 3] <= '7') {
            value = (unsigned char)(((raw[i + 1] - '0') << 6) |
                                    ((raw[i + 2] - '0') << 3) |
                                    (raw[i + 3] - '0'));
            i += 3;
        }
        if (used + 1 >= out_size) return 0;
        out[used++] = (char)value;
    }
    out[used] = '\0';
    return 1;
}

static int vfh_mountinfo_has_root(const char *root) {
    const char *fixture = getenv("VFH_SOURCE_TEST_MOUNTINFO");
    FILE *fp = fopen(fixture && fixture[0] ? fixture : "/proc/self/mountinfo", "r");
    if (!fp) return 0;
    char *line = NULL;
    size_t capacity = 0;
    int found = 0;
    while (getline(&line, &capacity, fp) >= 0) {
        char *save = NULL;
        char *field = strtok_r(line, " \n", &save);
        for (int index = 0; field && index < 4; index++)
            field = strtok_r(NULL, " \n", &save);
        if (!field) continue;
        char mountpoint[PATH_MAX];
        if (vfh_mount_field(mountpoint, sizeof(mountpoint), field) &&
            strcmp(mountpoint, root) == 0) {
            found = 1;
            break;
        }
    }
    free(line);
    fclose(fp);
    return found;
}
#endif

/* Is the card itself mounted? Independent of whether Videos/ exists on it. */
static int vfh_card_mounted(const char *card_root) {
    const char *fixture = getenv("VFH_SOURCE_TEST_AVAILABLE");
    if (fixture && fixture[0])
        return strcmp(fixture, "1") == 0 || strcasecmp(fixture, "true") == 0;
    if (!card_root || !card_root[0]) return 1;   /* unknown card root: assume present */
#if defined(__linux__)
    return vfh_mountinfo_has_root(card_root);
#else
    return 1;
#endif
}

/* Classify a source. Order matters: an unmounted card is the more fundamental
   problem, so it wins over the missing folder it necessarily implies. */
static void vfh_source_classify(vfh_source *source) {
    struct stat st;
    int folder = stat(source->root, &st) == 0 && S_ISDIR(st.st_mode);
    if (!vfh_card_mounted(source->card_root)) source->state = VFH_SOURCE_NO_CARD;
    else if (!folder) source->state = VFH_SOURCE_NO_FOLDER;
    else source->state = VFH_SOURCE_OK;
    source->available = source->state == VFH_SOURCE_OK;
}

void vfh_sources_refresh(vfh_sources *sources) {
    if (!sources) return;
    for (int i = 0; i < sources->count; i++)
        vfh_source_classify(&sources->items[i]);
}

const char *vfh_source_state_message(const vfh_source *source) {
    if (!source) return NULL;
    switch (source->state) {
        case VFH_SOURCE_NO_CARD:   return "This SD card is not mounted.";
        case VFH_SOURCE_NO_FOLDER: return "No Videos folder on this card yet.";
        default:                   return NULL;
    }
}

int vfh_sources_parse(vfh_sources *out, const char *video_paths,
                      char *error, size_t error_size) {
    if (!out || !video_paths || !video_paths[0]) {
        vfh_source_error(error, error_size, "video path list is empty");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    const char *start = video_paths;
    while (1) {
        const char *end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len == 0) {
            vfh_source_error(error, error_size, "VIDEO_PATHS contains an empty item");
            return 0;
        }
        if (out->count >= VFH_SOURCE_MAX || len >= VFH_SOURCE_PATH_MAX) {
            vfh_source_error(error, error_size, "VIDEO_PATHS has too many or oversized items");
            return 0;
        }
        char raw[VFH_SOURCE_PATH_MAX];
        memcpy(raw, start, len);
        raw[len] = '\0';
        vfh_source *source = &out->items[out->count];
        if (!vfh_source_normalize(raw, source->root, sizeof(source->root))) {
            vfh_source_error(error, error_size, "VIDEO_PATHS resolved path is too long");
            memset(out, 0, sizeof(*out));
            return 0;
        }
        for (int i = 0; i < out->count; i++) {
            if (strcmp(source->root, out->items[i].root) == 0) {
                vfh_source_error(error, error_size, "VIDEO_PATHS contains a duplicate root");
                memset(out, 0, sizeof(*out));
                return 0;
            }
        }
        if (out->count == 0) snprintf(source->id, sizeof(source->id), "primary");
        else if (out->count == 1) snprintf(source->id, sizeof(source->id), "secondary_sd");
        else snprintf(source->id, sizeof(source->id), "source%d", out->count + 1);
        snprintf(source->label, sizeof(source->label), "SD%d", out->count + 1);
        /* No card root known here; vfh_sources_resolve fills it in from
           SDCARD_PATHS and reclassifies. Until then "mounted" is assumed. */
        vfh_source_classify(source);
        out->count++;
        if (!end) break;
        start = end + 1;
    }
    if (error && error_size) error[0] = '\0';
    return 1;
}

int vfh_sources_resolve(vfh_sources *out, char *error, size_t error_size) {
    const char *paths = getenv("VIDEO_PATHS");
    int has_video_paths = paths && paths[0];
    char fallback[VFH_SOURCE_PATH_MAX];
    if (!has_video_paths) {
        const char *video = getenv("VIDEO_PATH");
        const char *sd = getenv("SDCARD_PATH");
        if (video && video[0]) snprintf(fallback, sizeof(fallback), "%s", video);
        else if (sd && sd[0]) snprintf(fallback, sizeof(fallback), "%s/Videos", sd);
        else snprintf(fallback, sizeof(fallback), "Videos");
        paths = fallback;
    }
    if (!vfh_sources_parse(out, paths, error, error_size)) return 0;
    const char *sdcard_paths = getenv("SDCARD_PATHS");
    if (has_video_paths && sdcard_paths && sdcard_paths[0]) {
        int card_count = 0;
        if (!vfh_source_list_count(sdcard_paths, &card_count, error, error_size)) {
            memset(out, 0, sizeof(*out));
            return 0;
        }
        if (card_count != out->count) {
            vfh_source_error(error, error_size,
                             "VIDEO_PATHS item count does not match SDCARD_PATHS");
            memset(out, 0, sizeof(*out));
            return 0;
        }
        for (int i = 0; i < out->count; i++) {
            char card_root[VFH_SOURCE_PATH_MAX];
            char normalized[VFH_SOURCE_PATH_MAX];
            if (!vfh_source_nth_path(sdcard_paths, i, card_root, sizeof(card_root)) ||
                !vfh_source_normalize(card_root, normalized, sizeof(normalized))) {
                vfh_source_error(error, error_size,
                                 "SDCARD_PATHS contains an invalid source root");
                memset(out, 0, sizeof(*out));
                return 0;
            }
            snprintf(out->items[i].card_root, sizeof(out->items[i].card_root),
                     "%s", normalized);
            vfh_source_classify(&out->items[i]);
        }
    }
    const char *primary = getenv("VIDEO_PATH");
    if (primary && primary[0]) {
        char normalized[VFH_SOURCE_PATH_MAX];
        if (!vfh_source_normalize(primary, normalized, sizeof(normalized)) ||
            strcmp(normalized, out->items[0].root) != 0) {
            vfh_source_error(error, error_size, "VIDEO_PATH does not match VIDEO_PATHS primary");
            memset(out, 0, sizeof(*out));
            return 0;
        }
    }
    return 1;
}

int vfh_recordings_path_resolve(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    const char *recordings = getenv("RECORDINGS_PATH");
    char fallback[VFH_SOURCE_PATH_MAX];
    if (!recordings || !recordings[0]) {
        const char *sd = getenv("SDCARD_PATH");
        if (sd && sd[0]) {
            int written = snprintf(fallback, sizeof(fallback), "%s/Recordings", sd);
            if (written < 0 || written >= (int)sizeof(fallback)) return 0;
        } else {
            snprintf(fallback, sizeof(fallback), "./Recordings");
        }
        recordings = fallback;
    }
    return vfh_source_normalize(recordings, out, out_size);
}

void vfh_sources_single_fallback(vfh_sources *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const char *video = getenv("VIDEO_PATH");
    const char *sd = getenv("SDCARD_PATH");
    char raw[VFH_SOURCE_PATH_MAX];
    if (video && video[0]) snprintf(raw, sizeof(raw), "%s", video);
    else if (sd && sd[0]) snprintf(raw, sizeof(raw), "%s/Videos", sd);
    else snprintf(raw, sizeof(raw), "Videos");
    vfh_source *source = &out->items[0];
    if (!vfh_source_normalize(raw, source->root, sizeof(source->root)))
        snprintf(source->root, sizeof(source->root), "%s", raw);
    snprintf(source->id, sizeof(source->id), "primary");
    snprintf(source->label, sizeof(source->label), "SD1");
    if (sd && sd[0]) snprintf(source->card_root, sizeof(source->card_root), "%s", sd);
    vfh_source_classify(source);
    out->count = 1;
}
