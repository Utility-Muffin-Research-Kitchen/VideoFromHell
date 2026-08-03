#include "vfh_library.h"

#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define VFH_LIBRARY_VERSION 2
#define VFH_LIBRARY_JSON_MAX (1024u * 1024u)
#define VFH_LIBRARY_SCAN_DEPTH 6
#define VFH_LIBRARY_NFO_MAX (64u * 1024u)

static void vfh_library_error(char *out, size_t out_size, const char *message) {
    if (out && out_size) snprintf(out, out_size, "%s", message ? message : "");
}

static int64_t vfh_library_mtime(const struct stat *st) {
#if defined(__APPLE__)
    return st ? (int64_t)st->st_mtimespec.tv_sec : 0;
#else
    return st ? (int64_t)st->st_mtim.tv_sec : 0;
#endif
}

static bool vfh_library_supported_file(const char *name) {
    static const char *const extensions[] = {
        ".mp4", ".m4v", ".mkv", ".mov", ".avi", ".webm", ".ts",
        ".m2ts", ".mts", ".mpg", ".mpeg", ".3gp", ".flv",
    };
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (strcasecmp(dot, extensions[i]) == 0) return true;
    }
    return false;
}

static bool vfh_library_ends_with(const char *text, const char *suffix) {
    size_t text_len = text ? strlen(text) : 0;
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    return text_len >= suffix_len && strcasecmp(text + text_len - suffix_len, suffix) == 0;
}

static bool vfh_library_recording_scratch(const char *name) {
    return vfh_library_ends_with(name, ".part") ||
           vfh_library_ends_with(name, ".partial") ||
           vfh_library_ends_with(name, ".tmp") ||
           vfh_library_ends_with(name, ".inprogress");
}

/* RetroArch and Leaf's converter use advisory locks while a capture is live.
 * A non-cooperating writer cannot be identified portably, so this is paired
 * with the zero-byte and scratch-name guards rather than pretending polling is
 * reliable. */
static bool vfh_library_file_is_write_locked(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return true;
    int locked = flock(fd, LOCK_SH | LOCK_NB) != 0 &&
                 (errno == EWOULDBLOCK || errno == EAGAIN);
    if (!locked) (void)flock(fd, LOCK_UN);
    close(fd);
    return locked;
}

static bool vfh_library_release_token(const char *token) {
    static const char *const known[] = {
        "480p", "576p", "720p", "1080p", "1440p", "2160p", "4k", "8k",
        "x264", "x265", "h264", "h265", "hevc", "avc",
        "bluray", "bdrip", "webrip", "web-dl", "webdl", "hdtv", "dvdrip",
    };
    if (!token || !token[0]) return false;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcasecmp(token, known[i]) == 0) return true;
    return false;
}

/* Release groups commonly follow a useful codec token with a dash.  Only the
 * known prefix is stripped, so meaningful hyphenated titles stay intact. */
static bool vfh_library_release_suffix(char *token) {
    if (vfh_library_release_token(token)) return true;
    char *dash = token ? strchr(token, '-') : NULL;
    if (!dash || dash == token || !dash[1]) return false;
    *dash = '\0';
    bool known = vfh_library_release_token(token);
    *dash = '-';
    return known;
}

static void vfh_library_clean_title(const char *filename, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!filename) return;
    size_t length = strlen(filename);
    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename) length = (size_t)(dot - filename);
    size_t used = 0;
    bool space = false;
    for (size_t i = 0; i < length && used + 1 < out_size; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (c == '_' || c == '.') c = ' ';
        if (isspace(c)) {
            space = used > 0;
            continue;
        }
        if (space && used + 1 < out_size) out[used++] = ' ';
        out[used++] = (char)c;
        space = false;
    }
    while (used > 0 && isspace((unsigned char)out[used - 1])) used--;
    out[used] = '\0';
    while (out[0]) {
        char *token = out + strlen(out);
        while (token > out && !isspace((unsigned char)token[-1])) token--;
        if (!vfh_library_release_suffix(token)) break;
        if (token == out) {
            out[0] = '\0';
            break;
        }
        do { token--; } while (token > out && isspace((unsigned char)*token));
        token[1] = '\0';
    }
    if (!out[0]) snprintf(out, out_size, "%s", filename);
}

static bool vfh_library_digits(const char *text, size_t count) {
    if (!text) return false;
    for (size_t i = 0; i < count; i++)
        if (!isdigit((unsigned char)text[i])) return false;
    return true;
}

static int vfh_library_decimal(const char *text, size_t count) {
    int value = 0;
    for (size_t i = 0; i < count; i++) value = value * 10 + text[i] - '0';
    return value;
}

static bool vfh_library_capture_date_valid(int year, int month, int day,
                                           int hour, int minute, int second) {
    return year >= 2000 && year <= 3000 && month >= 1 && month <= 12 &&
           day >= 1 && day <= 31 && hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

/* RetroArch filenames vary by frontend/version, so accept both familiar
 * ISO-style (`Game-2026-08-03_14-23-12`) and compact
 * (`Game_20260803-142312`) timestamp segments. */
static const char *vfh_library_capture_timestamp(const char *text, int64_t *out) {
    if (out) *out = 0;
    if (!text) return NULL;
    for (const char *at = text; at[0]; at++) {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        const char *after_date = NULL;
        size_t remaining = strlen(at);
        if (remaining >= 10 && vfh_library_digits(at, 4) &&
            (at[4] == '-' || at[4] == '_' || at[4] == '.') &&
            vfh_library_digits(at + 5, 2) &&
            (at[7] == '-' || at[7] == '_' || at[7] == '.') &&
            vfh_library_digits(at + 8, 2)) {
            year = vfh_library_decimal(at, 4);
            month = vfh_library_decimal(at + 5, 2);
            day = vfh_library_decimal(at + 8, 2);
            after_date = at + 10;
        } else if (remaining >= 8 && vfh_library_digits(at, 8)) {
            year = vfh_library_decimal(at, 4);
            month = vfh_library_decimal(at + 4, 2);
            day = vfh_library_decimal(at + 6, 2);
            after_date = at + 8;
        } else {
            continue;
        }
        const char *time = after_date;
        if (*time == 'T' || *time == 't' || *time == '-' || *time == '_' || *time == ' ')
            time++;
        size_t time_length = strlen(time);
        if (time_length >= 2 && vfh_library_digits(time, 2)) {
            hour = vfh_library_decimal(time, 2);
            if (time_length >= 5 &&
                (time[2] == '-' || time[2] == '_' || time[2] == ':' || time[2] == '.') &&
                vfh_library_digits(time + 3, 2)) {
                minute = vfh_library_decimal(time + 3, 2);
                if (time_length >= 8 &&
                    (time[5] == '-' || time[5] == '_' || time[5] == ':' || time[5] == '.') &&
                    vfh_library_digits(time + 6, 2))
                    second = vfh_library_decimal(time + 6, 2);
            } else if (time_length >= 6 && vfh_library_digits(time + 2, 4)) {
                minute = vfh_library_decimal(time + 2, 2);
                second = vfh_library_decimal(time + 4, 2);
            } else {
                hour = minute = second = 0;
            }
        }
        if (!vfh_library_capture_date_valid(year, month, day, hour, minute, second)) continue;
        if (out) *out = (int64_t)year * 10000000000LL + (int64_t)month * 100000000LL +
                        (int64_t)day * 1000000LL + (int64_t)hour * 10000LL +
                        (int64_t)minute * 100LL + second;
        return at;
    }
    return NULL;
}

static void vfh_library_clean_recording_title(const char *filename, char *out,
                                              size_t out_size, int64_t *capture_timestamp) {
    if (capture_timestamp) *capture_timestamp = 0;
    char stem[VFH_LIBRARY_TITLE_MAX];
    snprintf(stem, sizeof(stem), "%s", filename ? filename : "");
    char *extension = strrchr(stem, '.');
    if (extension) *extension = '\0';
    char *part = NULL;
    for (char *cursor = stem; *cursor; cursor++)
        if (strncasecmp(cursor, "-part", 5) == 0) part = cursor;
    if (part) {
        char *end = NULL;
        (void)strtol(part + 5, &end, 10);
        if (end && !*end) *part = '\0';
    }
    int64_t timestamp = 0;
    const char *timestamp_at = vfh_library_capture_timestamp(stem, &timestamp);
    if (timestamp_at) {
        char *trim = (char *)timestamp_at;
        while (trim > stem && (trim[-1] == '-' || trim[-1] == '_' || trim[-1] == '.' ||
                               isspace((unsigned char)trim[-1]))) trim--;
        *trim = '\0';
    }
    vfh_library_clean_title(stem[0] ? stem : filename, out, out_size);
    if (capture_timestamp) *capture_timestamp = timestamp;
}

static void vfh_library_set_clean_title(vfh_library_item *item) {
    if (!item) return;
    const char *name = strrchr(item->relative_path, '/');
    name = name ? name + 1 : item->relative_path;
    if (item->content_kind == VFH_CONTENT_RECORDING)
        vfh_library_clean_recording_title(name, item->display_title,
                                          sizeof(item->display_title),
                                          &item->capture_timestamp);
    else
        vfh_library_clean_title(name, item->display_title, sizeof(item->display_title));
}

static void vfh_library_clear_probe_metadata(vfh_library_item *item) {
    if (!item) return;
    item->duration = 0.0;
    if (!item->nfo_year) item->year = 0;
    item->metadata_ready = false;
    item->has_embedded_art = false;
    item->art_source = VFH_LIBRARY_ART_UNKNOWN;
    item->container[0] = '\0';
    item->video_codec[0] = '\0';
    item->audio_codec[0] = '\0';
}

static const char *vfh_library_find_ci(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) return NULL;
    for (const char *candidate = text; *candidate; candidate++) {
        size_t index = 0;
        while (needle[index] && candidate[index] &&
               tolower((unsigned char)candidate[index]) == tolower((unsigned char)needle[index]))
            index++;
        if (!needle[index]) return candidate;
    }
    return NULL;
}

static bool vfh_library_nfo_value(const char *text, const char *name,
                                  char *out, size_t out_size) {
    if (!text || !name || !out || out_size == 0) return false;
    char opening[40], closing[40];
    if (snprintf(opening, sizeof(opening), "<%s>", name) >= (int)sizeof(opening) ||
        snprintf(closing, sizeof(closing), "</%s>", name) >= (int)sizeof(closing))
        return false;
    const char *start = vfh_library_find_ci(text, opening);
    if (!start) return false;
    start += strlen(opening);
    const char *end = vfh_library_find_ci(start, closing);
    if (!end) return false;
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    size_t length = (size_t)(end - start);
    if (length == 0 || length >= out_size) return false;
    memcpy(out, start, length);
    out[length] = '\0';
    return true;
}

static bool vfh_library_read_nfo(const char *video_path, char *title, size_t title_size,
                                 int *year, bool *has_title, bool *has_year) {
    if (has_title) *has_title = false;
    if (has_year) *has_year = false;
    if (year) *year = 0;
    if (!video_path) return false;
    char path[VFH_SOURCE_PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s", video_path);
    char *extension = length >= 0 && length < (int)sizeof(path) ? strrchr(path, '.') : NULL;
    if (!extension || extension == strrchr(path, '/')) return false;
    snprintf(extension, (size_t)(path + sizeof(path) - extension), ".nfo");
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long raw_size = ftell(file);
    if (raw_size < 1 || (unsigned long)raw_size > VFH_LIBRARY_NFO_MAX ||
        fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    char *text = malloc((size_t)raw_size + 1u);
    if (!text) { fclose(file); return false; }
    size_t got = fread(text, 1, (size_t)raw_size, file);
    fclose(file);
    text[got] = '\0';
    if (got != (size_t)raw_size) { free(text); return false; }
    bool found_title = vfh_library_nfo_value(text, "title", title, title_size);
    char raw_year[16] = "";
    bool found_year = vfh_library_nfo_value(text, "year", raw_year, sizeof(raw_year));
    char *end = NULL;
    long parsed_year = found_year ? strtol(raw_year, &end, 10) : 0;
    if (!end || *end || parsed_year < 1800 || parsed_year > 3000) found_year = false;
    if (found_year && year) *year = (int)parsed_year;
    if (has_title) *has_title = found_title;
    if (has_year) *has_year = found_year;
    free(text);
    return found_title || found_year;
}

static bool vfh_library_apply_nfo_metadata(vfh_library_item *item, const char *video_path) {
    if (!item || !video_path) return false;
    char title[VFH_LIBRARY_TITLE_MAX] = "";
    int year = 0;
    bool has_title = false, has_year = false;
    (void)vfh_library_read_nfo(video_path, title, sizeof(title), &year, &has_title, &has_year);
    bool changed = false;
    if (has_title) {
        if (!item->nfo_title || strcmp(item->display_title, title) != 0) changed = true;
        snprintf(item->display_title, sizeof(item->display_title), "%s", title);
        item->nfo_title = true;
    } else if (item->nfo_title) {
        vfh_library_set_clean_title(item);
        item->nfo_title = false;
        changed = true;
    }
    if (has_year) {
        if (!item->nfo_year || item->year != year) changed = true;
        item->year = year;
        item->nfo_year = true;
    } else if (item->nfo_year) {
        item->year = 0;
        item->nfo_year = false;
        changed = true;
    }
    return changed;
}

static bool vfh_library_reserve(vfh_library *library, size_t wanted) {
    if (!library || wanted > VFH_LIBRARY_MAX_RECORDS) return false;
    if (wanted <= library->capacity) return true;
    size_t capacity = library->capacity ? library->capacity * 2u : 32u;
    if (capacity < wanted) capacity = wanted;
    if (capacity > VFH_LIBRARY_MAX_RECORDS) capacity = VFH_LIBRARY_MAX_RECORDS;
    vfh_library_item *items = realloc(library->items, capacity * sizeof(*items));
    if (!items) return false;
    library->items = items;
    library->capacity = capacity;
    return true;
}

void vfh_library_init(vfh_library *library) {
    if (library) memset(library, 0, sizeof(*library));
}

void vfh_library_destroy(vfh_library *library) {
    if (!library) return;
    free(library->items);
    memset(library, 0, sizeof(*library));
}

static bool vfh_library_copy(vfh_library *out, const vfh_library *source) {
    if (!out || !source || source->count > VFH_LIBRARY_MAX_RECORDS) return false;
    vfh_library_init(out);
    if (source->count && !vfh_library_reserve(out, source->count)) return false;
    if (source->count)
        memcpy(out->items, source->items, source->count * sizeof(*out->items));
    out->count = source->count;
    out->scan_generation = source->scan_generation;
    return true;
}

const vfh_library_item *vfh_library_find(const vfh_library *library,
                                         vfh_content_kind content_kind,
                                         int source_index,
                                         const char *relative_path) {
    if (!library || !relative_path) return NULL;
    for (size_t i = 0; i < library->count; i++) {
        const vfh_library_item *item = &library->items[i];
        if (item->content_kind == content_kind && item->source_index == source_index &&
            strcmp(item->relative_path, relative_path) == 0) return item;
    }
    return NULL;
}

bool vfh_library_resolve_path(const vfh_library_item *item,
                              const vfh_sources *video_sources,
                              const char *recordings_path,
                              char *out, size_t out_size) {
    if (!item || !out || out_size == 0 || !item->relative_path[0] ||
        item->relative_path[0] == '/' || strstr(item->relative_path, "../")) return false;
    const char *root = recordings_path;
    if (item->content_kind == VFH_CONTENT_VIDEO) {
        if (!video_sources || item->source_index < 0 ||
            item->source_index >= video_sources->count) return false;
        root = video_sources->items[item->source_index].root;
    }
    if (!root || !root[0]) return false;
    int written = snprintf(out, out_size, "%s/%s", root, item->relative_path);
    return written >= 0 && written < (int)out_size;
}

static vfh_library_item *vfh_library_find_mutable(vfh_library *library,
                                                   vfh_content_kind content_kind,
                                                   int source_index,
                                                   const char *relative_path) {
    return (vfh_library_item *)vfh_library_find(library, content_kind, source_index,
                                                relative_path);
}

static bool vfh_library_upsert(vfh_library *library, vfh_content_kind content_kind,
                               int source_index, const char *relative_path,
                               const struct stat *st, unsigned generation) {
    vfh_library_item *item = vfh_library_find_mutable(library, content_kind, source_index,
                                                       relative_path);
    int64_t mtime = vfh_library_mtime(st);
    uint64_t size = st ? (uint64_t)st->st_size : 0;
    if (!item) {
        if (!vfh_library_reserve(library, library->count + 1u)) return false;
        item = &library->items[library->count++];
        memset(item, 0, sizeof(*item));
        item->content_kind = content_kind;
        item->source_index = source_index;
        item->first_seen = mtime;
        snprintf(item->relative_path, sizeof(item->relative_path), "%s", relative_path);
        vfh_library_set_clean_title(item);
    } else if (item->size != size || item->mtime != mtime) {
        /* Probed metadata belongs to exact bytes only. Watch history and the
         * source-relative identity stay stable across a changed file. */
        item->nfo_title = false;
        item->nfo_year = false;
        vfh_library_set_clean_title(item);
        vfh_library_clear_probe_metadata(item);
    }
    item->size = size;
    item->mtime = mtime;
    item->available = true;
    item->seen_generation = generation;
    return true;
}

static void vfh_library_remove_index(vfh_library *library, size_t index) {
    if (!library || index >= library->count) return;
    if (index + 1u < library->count) {
        memmove(&library->items[index], &library->items[index + 1u],
                (library->count - index - 1u) * sizeof(*library->items));
    }
    library->count--;
}

static bool vfh_library_recording_base(const char *relative_path, char *base,
                                       size_t base_size, int *part) {
    if (!relative_path || !base || base_size == 0 || !part) return false;
    *part = 0;
    const char *name = strrchr(relative_path, '/');
    name = name ? name + 1 : relative_path;
    const char *extension = strrchr(name, '.');
    if (!extension || (strcasecmp(extension, ".mp4") != 0 &&
                       strcasecmp(extension, ".mkv") != 0)) return false;
    size_t stem_len = (size_t)(extension - name);
    if (stem_len == 0 || stem_len >= base_size) return false;
    memcpy(base, name, stem_len);
    base[stem_len] = '\0';

    char *marker = NULL;
    for (char *cursor = base; *cursor; cursor++) {
        if (strncasecmp(cursor, "-part", 5) == 0) marker = cursor;
    }
    if (!marker || !marker[5]) return true;
    char *end = NULL;
    errno = 0;
    long value = strtol(marker + 5, &end, 10);
    if (errno || !end || *end || value < 1 || value > INT_MAX ||
        strcasecmp(extension, ".mp4") != 0) return true;
    *marker = '\0';
    *part = (int)value;
    return true;
}

static bool vfh_library_same_recording_base(const vfh_library_item *left,
                                            const vfh_library_item *right) {
    char left_base[VFH_LIBRARY_TITLE_MAX], right_base[VFH_LIBRARY_TITLE_MAX];
    int ignored_left = 0, ignored_right = 0;
    return vfh_library_recording_base(left->relative_path, left_base, sizeof(left_base),
                                      &ignored_left) &&
           vfh_library_recording_base(right->relative_path, right_base, sizeof(right_base),
                                      &ignored_right) &&
           strcasecmp(left_base, right_base) == 0;
}

static void vfh_library_group_recordings(vfh_library *library, unsigned generation) {
    if (!library) return;
    for (size_t i = 0; i < library->count;) {
        vfh_library_item *item = &library->items[i];
        if (item->content_kind != VFH_CONTENT_RECORDING ||
            item->seen_generation != generation) {
            i++;
            continue;
        }
        char base[VFH_LIBRARY_TITLE_MAX];
        int part = 0;
        if (!vfh_library_recording_base(item->relative_path, base, sizeof(base), &part)) {
            i++;
            continue;
        }
        bool has_parts = false, has_mp4 = false;
        for (size_t j = 0; j < library->count; j++) {
            vfh_library_item *other = &library->items[j];
            if (other->content_kind != VFH_CONTENT_RECORDING ||
                other->seen_generation != generation ||
                !vfh_library_same_recording_base(item, other)) continue;
            int other_part = 0;
            char ignored[VFH_LIBRARY_TITLE_MAX];
            (void)vfh_library_recording_base(other->relative_path, ignored, sizeof(ignored),
                                             &other_part);
            if (other_part > 0) has_parts = true;
            if (vfh_library_ends_with(other->relative_path, ".mp4") && other_part == 0)
                has_mp4 = true;
        }
        item->recording_part = part;
        bool hide = (has_parts && part == 0) || (!has_parts && part == 0 &&
                     vfh_library_ends_with(item->relative_path, ".mkv") && has_mp4);
        if (hide) {
            vfh_library_remove_index(library, i);
            continue;
        }
        i++;
    }
}

static int vfh_library_compare(const void *a, const void *b) {
    const vfh_library_item *left = a;
    const vfh_library_item *right = b;
    if (left->content_kind != right->content_kind)
        return (int)left->content_kind - (int)right->content_kind;
    if (left->source_index != right->source_index)
        return left->source_index - right->source_index;
    if (left->recording_part && right->recording_part) {
        char left_base[VFH_LIBRARY_TITLE_MAX], right_base[VFH_LIBRARY_TITLE_MAX];
        int ignored_left = 0, ignored_right = 0;
        if (vfh_library_recording_base(left->relative_path, left_base, sizeof(left_base),
                                       &ignored_left) &&
            vfh_library_recording_base(right->relative_path, right_base, sizeof(right_base),
                                       &ignored_right) && strcasecmp(left_base, right_base) == 0)
            return left->recording_part - right->recording_part;
    }
    return strcasecmp(left->relative_path, right->relative_path);
}

typedef struct {
    vfh_library *library;
    vfh_content_kind content_kind;
    int source_index;
    const char *root;
    unsigned generation;
    vfh_library_cancelled_fn cancelled;
    void *cancel_opaque;
    bool cancelled_scan;
    bool failed;
    bool unreadable;
} vfh_library_scan_context;

static bool vfh_library_scan_cancelled(vfh_library_scan_context *context) {
    if (!context || !context->cancelled || !context->cancelled(context->cancel_opaque))
        return false;
    context->cancelled_scan = true;
    return true;
}

static void vfh_library_scan_directory(vfh_library_scan_context *context,
                                       const char *directory, const char *relative,
                                       int depth) {
    if (!context || context->failed || context->cancelled_scan ||
        depth > VFH_LIBRARY_SCAN_DEPTH || vfh_library_scan_cancelled(context)) return;
    DIR *dir = opendir(directory);
    if (!dir) {
        /* A permission or I/O failure on one folder is a local problem, not a
         * reason to discard the other card's library. Record it so the caller
         * can skip pruning: files under an unreadable folder were never seen
         * this generation and must not be mistaken for deletions. */
        context->unreadable = true;
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && !context->failed && !context->cancelled_scan) {
        if (vfh_library_scan_cancelled(context)) break;
        if (entry->d_name[0] == '.') continue;
        char path[VFH_SOURCE_PATH_MAX];
        char rel[VFH_LIBRARY_RELATIVE_PATH_MAX];
        int path_size = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        int rel_size = relative && relative[0]
            ? snprintf(rel, sizeof(rel), "%s/%s", relative, entry->d_name)
            : snprintf(rel, sizeof(rel), "%s", entry->d_name);
        if (path_size < 0 || path_size >= (int)sizeof(path) ||
            rel_size < 0 || rel_size >= (int)sizeof(rel)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;  /* file raced away; a later scan retries */
        if (S_ISDIR(st.st_mode)) {
            if (depth < VFH_LIBRARY_SCAN_DEPTH)
                vfh_library_scan_directory(context, path, rel, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        if (context->content_kind == VFH_CONTENT_RECORDING &&
            (st.st_size <= 0 || vfh_library_recording_scratch(entry->d_name) ||
             vfh_library_file_is_write_locked(path))) continue;
        if (!vfh_library_supported_file(entry->d_name)) continue;
        if (!vfh_library_upsert(context->library, context->content_kind,
                                context->source_index, rel, &st, context->generation)) {
            context->failed = true;
        } else {
            vfh_library_item *item = vfh_library_find_mutable(context->library,
                context->content_kind, context->source_index, rel);
            if (vfh_library_apply_nfo_metadata(item, path))
                vfh_library_clear_probe_metadata(item);
        }
    }
    closedir(dir);
}

static vfh_library_scan_result vfh_library_scan_root(
    vfh_library *library, vfh_content_kind content_kind, int source_index,
    const char *root, bool allow_missing, vfh_library_cancelled_fn cancelled,
    void *cancel_opaque, char *error, size_t error_size) {
    if (cancelled && cancelled(cancel_opaque)) return VFH_LIBRARY_SCAN_CANCELLED;
    if (!root || !root[0]) return allow_missing ? VFH_LIBRARY_SCAN_COMPLETE
                                                : VFH_LIBRARY_SCAN_FAILED;
    struct stat root_stat;
    if (stat(root, &root_stat) != 0) {
        if (allow_missing && errno == ENOENT) {
            /* A recordings card/path can be absent between launches. The
             * caller already marked every item unavailable before scanning;
             * retain its durable identity so Continue Watching can explain
             * the missing source rather than silently forgetting it. */
            return VFH_LIBRARY_SCAN_COMPLETE;
        }
        vfh_library_error(error, error_size, "Unable to read a video source.");
        return VFH_LIBRARY_SCAN_FAILED;
    }
    if (!S_ISDIR(root_stat.st_mode)) {
        vfh_library_error(error, error_size, "A video source is not a folder.");
        return VFH_LIBRARY_SCAN_FAILED;
    }
    unsigned generation = ++library->scan_generation;
    if (!generation) generation = ++library->scan_generation;
    vfh_library_scan_context context = {
        .library = library,
        .content_kind = content_kind,
        .source_index = source_index,
        .root = root,
        .generation = generation,
        .cancelled = cancelled,
        .cancel_opaque = cancel_opaque,
    };
    vfh_library_scan_directory(&context, root, "", 0);
    if (context.cancelled_scan) return VFH_LIBRARY_SCAN_CANCELLED;
    if (context.failed) {
        vfh_library_error(error, error_size, "Video library is too large to index.");
        return VFH_LIBRARY_SCAN_FAILED;
    }
    /* Prune only after a complete enumeration of this source (§4.1): an
     * incomplete walk cannot tell a deleted file from an unreadable folder. */
    if (!context.unreadable) {
        for (size_t i = 0; i < library->count;) {
            vfh_library_item *item = &library->items[i];
            if (item->content_kind == content_kind && item->source_index == source_index &&
                item->seen_generation != generation) {
                vfh_library_remove_index(library, i);
                continue;
            }
            i++;
        }
    }
    if (content_kind == VFH_CONTENT_RECORDING)
        vfh_library_group_recordings(library, generation);
    if (context.unreadable) {
        vfh_library_error(error, error_size, "Some folders could not be read; showing what is available.");
        return VFH_LIBRARY_SCAN_PARTIAL;
    }
    return VFH_LIBRARY_SCAN_COMPLETE;
}

vfh_library_scan_result vfh_library_scan_cancellable(
    vfh_library *library, const vfh_sources *video_sources,
    const char *recordings_path, vfh_library_cancelled_fn cancelled,
    void *cancel_opaque, char *error, size_t error_size) {
    if (!library || !video_sources) {
        vfh_library_error(error, error_size, "Missing video-source configuration.");
        return VFH_LIBRARY_SCAN_FAILED;
    }
    if (cancelled && cancelled(cancel_opaque)) return VFH_LIBRARY_SCAN_CANCELLED;
    vfh_library working;
    if (!vfh_library_copy(&working, library)) {
        vfh_library_error(error, error_size, "Unable to allocate the video library.");
        return VFH_LIBRARY_SCAN_FAILED;
    }
    for (size_t i = 0; i < working.count; i++) working.items[i].available = false;
    vfh_library_scan_result result = VFH_LIBRARY_SCAN_COMPLETE;
    bool partial = false;
    for (int i = 0; i < video_sources->count; i++) {
        const vfh_source *source = &video_sources->items[i];
        if (!source->available) continue;  /* absent sources retain cached records */
        result = vfh_library_scan_root(&working, VFH_CONTENT_VIDEO, i, source->root, false,
                                       cancelled, cancel_opaque, error, error_size);
        /* One partly readable card must not cost the other card its library. */
        if (result == VFH_LIBRARY_SCAN_PARTIAL) {
            partial = true;
            result = VFH_LIBRARY_SCAN_COMPLETE;
        }
        if (result != VFH_LIBRARY_SCAN_COMPLETE) break;
    }
    if (result == VFH_LIBRARY_SCAN_COMPLETE) {
        result = vfh_library_scan_root(&working, VFH_CONTENT_RECORDING, 0, recordings_path,
                                       true, cancelled, cancel_opaque, error, error_size);
        if (result == VFH_LIBRARY_SCAN_PARTIAL) {
            partial = true;
            result = VFH_LIBRARY_SCAN_COMPLETE;
        }
    }
    if (result == VFH_LIBRARY_SCAN_COMPLETE) {
        qsort(working.items, working.count, sizeof(*working.items), vfh_library_compare);
        vfh_library_destroy(library);
        *library = working;
        if (partial) return VFH_LIBRARY_SCAN_PARTIAL;
        if (error && error_size) error[0] = '\0';
    } else {
        vfh_library_destroy(&working);
    }
    return result;
}

bool vfh_library_scan(vfh_library *library, const vfh_sources *video_sources,
                      const char *recordings_path, char *error, size_t error_size) {
    /* A partial scan still commits usable results; `error` carries the reason
     * so the browser can say so without pretending the library is empty. */
    vfh_library_scan_result result =
        vfh_library_scan_cancellable(library, video_sources, recordings_path,
                                     NULL, NULL, error, error_size);
    return result == VFH_LIBRARY_SCAN_COMPLETE || result == VFH_LIBRARY_SCAN_PARTIAL;
}

static bool vfh_library_store_path(char *out, size_t out_size) {
    const char *base = getenv("USERDATA_PATH");
    if (!base || !base[0]) base = getenv("SHARED_USERDATA_PATH");
    if (!base || !base[0]) return false;
    char dir[VFH_SOURCE_PATH_MAX];
    int directory_size = snprintf(dir, sizeof(dir), "%s/VideoFromHell", base);
    if (directory_size < 0 || directory_size >= (int)sizeof(dir)) return false;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    int path_size = snprintf(out, out_size, "%s/library-v2.json", dir);
    return path_size >= 0 && path_size < (int)out_size;
}

static bool vfh_library_json_string(const cJSON *object, const char *name,
                                    char *out, size_t out_size) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(value) || !value->valuestring ||
        strlen(value->valuestring) >= out_size) return false;
    snprintf(out, out_size, "%s", value->valuestring);
    return true;
}

static bool vfh_library_json_optional_string(const cJSON *object, const char *name,
                                             char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!value) return true;
    return vfh_library_json_string(object, name, out, out_size);
}

static const char *vfh_library_art_source_name(vfh_library_art_source source) {
    switch (source) {
        case VFH_LIBRARY_ART_SIDECAR: return "sidecar";
        case VFH_LIBRARY_ART_EMBEDDED: return "embedded";
        case VFH_LIBRARY_ART_GENERATED: return "generated";
        default: return "unknown";
    }
}

static bool vfh_library_art_source_parse(const cJSON *record,
                                         vfh_library_art_source *out_source) {
    if (!out_source) return false;
    *out_source = VFH_LIBRARY_ART_UNKNOWN;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(record, "art");
    if (!value) return true;  /* v2 records written before the metadata worker */
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    if (!strcmp(value->valuestring, "sidecar")) *out_source = VFH_LIBRARY_ART_SIDECAR;
    else if (!strcmp(value->valuestring, "embedded")) *out_source = VFH_LIBRARY_ART_EMBEDDED;
    else if (!strcmp(value->valuestring, "generated")) *out_source = VFH_LIBRARY_ART_GENERATED;
    else if (strcmp(value->valuestring, "unknown")) return false;
    return true;
}

bool vfh_library_load(vfh_library *library) {
    if (!library) return false;
    vfh_library_destroy(library);
    vfh_library_init(library);
    char path[VFH_SOURCE_PATH_MAX];
    if (!vfh_library_store_path(path, sizeof(path))) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long raw_size = ftell(fp);
    if (raw_size <= 0 || (unsigned long)raw_size > VFH_LIBRARY_JSON_MAX ||
        fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return false; }
    char *text = malloc((size_t)raw_size + 1u);
    if (!text) { fclose(fp); return false; }
    size_t got = fread(text, 1, (size_t)raw_size, fp);
    fclose(fp);
    text[got] = '\0';
    cJSON *root = got == (size_t)raw_size ? cJSON_ParseWithLength(text, got) : NULL;
    free(text);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *records = cJSON_GetObjectItemCaseSensitive(root, "records");
    if (!cJSON_IsNumber(version) || version->valueint != VFH_LIBRARY_VERSION ||
        !cJSON_IsArray(records) || cJSON_GetArraySize(records) > VFH_LIBRARY_MAX_RECORDS) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *record = NULL;
    cJSON_ArrayForEach(record, records) {
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(record, "kind");
        cJSON *source = cJSON_GetObjectItemCaseSensitive(record, "source");
        cJSON *size = cJSON_GetObjectItemCaseSensitive(record, "size");
        cJSON *mtime = cJSON_GetObjectItemCaseSensitive(record, "mtime");
        cJSON *first_seen = cJSON_GetObjectItemCaseSensitive(record, "first_seen");
        cJSON *capture_timestamp = cJSON_GetObjectItemCaseSensitive(record, "capture_timestamp");
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(record, "duration");
        cJSON *part = cJSON_GetObjectItemCaseSensitive(record, "part");
        cJSON *available = cJSON_GetObjectItemCaseSensitive(record, "available");
        cJSON *year = cJSON_GetObjectItemCaseSensitive(record, "year");
        cJSON *metadata_ready = cJSON_GetObjectItemCaseSensitive(record, "metadata_ready");
        cJSON *nfo_title = cJSON_GetObjectItemCaseSensitive(record, "nfo_title");
        cJSON *nfo_year = cJSON_GetObjectItemCaseSensitive(record, "nfo_year");
        cJSON *embedded_art = cJSON_GetObjectItemCaseSensitive(record, "embedded_art");
        if (!cJSON_IsObject(record) || !cJSON_IsString(kind) || !kind->valuestring ||
            !cJSON_IsNumber(source) || !cJSON_IsNumber(size) || !cJSON_IsNumber(mtime) ||
            !cJSON_IsNumber(first_seen) || !cJSON_IsNumber(duration) || !cJSON_IsNumber(part) ||
            (capture_timestamp && !cJSON_IsNumber(capture_timestamp)) ||
            (available && !cJSON_IsBool(available)) ||
            (year && (!cJSON_IsNumber(year) || year->valueint < 0 || year->valueint > 3000)) ||
            (metadata_ready && !cJSON_IsBool(metadata_ready)) ||
            (nfo_title && !cJSON_IsBool(nfo_title)) ||
            (nfo_year && !cJSON_IsBool(nfo_year)) ||
            (embedded_art && !cJSON_IsBool(embedded_art)) ||
            (strcmp(kind->valuestring, "video") && strcmp(kind->valuestring, "recording")) ||
            source->valueint < 0 || source->valueint >= VFH_SOURCE_MAX ||
            !vfh_library_reserve(library, library->count + 1u)) {
            vfh_library_destroy(library);
            cJSON_Delete(root);
            return false;
        }
        vfh_library_item *item = &library->items[library->count];
        memset(item, 0, sizeof(*item));
        if (!vfh_library_json_string(record, "relative", item->relative_path,
                                     sizeof(item->relative_path)) ||
            !vfh_library_json_string(record, "title", item->display_title,
                                     sizeof(item->display_title)) ||
            !vfh_library_json_optional_string(record, "container", item->container,
                                              sizeof(item->container)) ||
            !vfh_library_json_optional_string(record, "video_codec", item->video_codec,
                                              sizeof(item->video_codec)) ||
            !vfh_library_json_optional_string(record, "audio_codec", item->audio_codec,
                                              sizeof(item->audio_codec)) ||
            !vfh_library_art_source_parse(record, &item->art_source)) {
            vfh_library_destroy(library);
            cJSON_Delete(root);
            return false;
        }
        item->content_kind = strcmp(kind->valuestring, "recording") == 0
                           ? VFH_CONTENT_RECORDING : VFH_CONTENT_VIDEO;
        item->source_index = source->valueint;
        item->size = (uint64_t)size->valuedouble;
        item->mtime = (int64_t)mtime->valuedouble;
        item->first_seen = (int64_t)first_seen->valuedouble;
        item->capture_timestamp = capture_timestamp ? (int64_t)capture_timestamp->valuedouble : 0;
        item->duration = duration->valuedouble;
        item->year = year ? year->valueint : 0;
        item->recording_part = part->valueint;
        /* Older v2 documents did not carry availability. Treat those cache
         * rows conservatively until the background scan republishes them. */
        item->available = cJSON_IsTrue(available);
        item->metadata_ready = cJSON_IsTrue(metadata_ready);
        item->nfo_title = cJSON_IsTrue(nfo_title);
        item->nfo_year = cJSON_IsTrue(nfo_year);
        item->has_embedded_art = cJSON_IsTrue(embedded_art);
        library->count++;
    }
    cJSON_Delete(root);
    return true;
}

bool vfh_library_save(const vfh_library *library) {
    if (!library || library->count > VFH_LIBRARY_MAX_RECORDS) return false;
    char path[VFH_SOURCE_PATH_MAX], temporary[VFH_SOURCE_PATH_MAX + 8];
    if (!vfh_library_store_path(path, sizeof(path)) ||
        snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return false;
    cJSON *root = cJSON_CreateObject();
    cJSON *records = cJSON_CreateArray();
    if (!root || !records || !cJSON_AddNumberToObject(root, "version", VFH_LIBRARY_VERSION) ||
        !cJSON_AddItemToObject(root, "records", records)) {
        cJSON_Delete(root);
        return false;
    }
    for (size_t i = 0; i < library->count; i++) {
        const vfh_library_item *item = &library->items[i];
        cJSON *record = cJSON_CreateObject();
        if (!record || !cJSON_AddStringToObject(record, "kind",
                    item->content_kind == VFH_CONTENT_RECORDING ? "recording" : "video") ||
            !cJSON_AddNumberToObject(record, "source", item->source_index) ||
            !cJSON_AddStringToObject(record, "relative", item->relative_path) ||
            !cJSON_AddStringToObject(record, "title", item->display_title) ||
            !cJSON_AddNumberToObject(record, "size", (double)item->size) ||
            !cJSON_AddNumberToObject(record, "mtime", (double)item->mtime) ||
            !cJSON_AddNumberToObject(record, "first_seen", (double)item->first_seen) ||
            !cJSON_AddNumberToObject(record, "capture_timestamp", (double)item->capture_timestamp) ||
            !cJSON_AddNumberToObject(record, "duration", item->duration) ||
            !cJSON_AddNumberToObject(record, "year", item->year) ||
            !cJSON_AddNumberToObject(record, "part", item->recording_part) ||
            !cJSON_AddBoolToObject(record, "available", item->available) ||
            !cJSON_AddBoolToObject(record, "metadata_ready", item->metadata_ready) ||
            !cJSON_AddBoolToObject(record, "nfo_title", item->nfo_title) ||
            !cJSON_AddBoolToObject(record, "nfo_year", item->nfo_year) ||
            !cJSON_AddStringToObject(record, "container", item->container) ||
            !cJSON_AddStringToObject(record, "video_codec", item->video_codec) ||
            !cJSON_AddStringToObject(record, "audio_codec", item->audio_codec) ||
            !cJSON_AddBoolToObject(record, "embedded_art", item->has_embedded_art) ||
            !cJSON_AddStringToObject(record, "art", vfh_library_art_source_name(item->art_source)) ||
            !cJSON_AddItemToArray(records, record)) {
            cJSON_Delete(record);
            cJSON_Delete(root);
            return false;
        }
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text || strlen(text) > VFH_LIBRARY_JSON_MAX) {
        free(text);
        return false;
    }
    FILE *fp = fopen(temporary, "wb");
    bool ok = false;
    if (fp) {
        ok = fwrite(text, 1, strlen(text), fp) == strlen(text);
        ok = fclose(fp) == 0 && ok;
    }
    if (!ok) {
        unlink(temporary);
        free(text);
        return false;
    }
    free(text);
    if (rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}
