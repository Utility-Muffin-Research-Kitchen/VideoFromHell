#include "vfh_resume.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VFH_PLAYBACK_STORE_VERSION 2
#define VFH_PLAYBACK_STORE_MAX_BYTES (1 << 20)
#define VFH_RESUME_MIN_SECONDS 30.0
#define VFH_RESUME_WATCHED_FRACTION 0.90
#define VFH_RESUME_MAX_ENTRIES 200

static bool vfh_resume_dir(char *out, size_t out_size) {
    const char *base = getenv("USERDATA_PATH");
    if (!base || !base[0]) base = getenv("SHARED_USERDATA_PATH");
    if (!base || !base[0]) return false;
    int length = snprintf(out, out_size, "%s/VideoFromHell", base);
    if (length < 0 || length >= (int)out_size) return false;
    (void)mkdir(out, 0755);
    return true;
}

static bool vfh_resume_path(char *out, size_t out_size, const char *name) {
    char directory[768];
    if (!vfh_resume_dir(directory, sizeof(directory))) return false;
    int length = snprintf(out, out_size, "%s/%s", directory, name);
    return length >= 0 && length < (int)out_size;
}

static cJSON *vfh_resume_read_json(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 1 || length > VFH_PLAYBACK_STORE_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)length + 1);
    if (!text) { fclose(file); return NULL; }
    bool read = fread(text, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!read) { free(text); return NULL; }
    text[length] = '\0';
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (root && !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool vfh_resume_store(const cJSON *root) {
    char path[1024], temporary[1088];
    if (!root || !vfh_resume_path(path, sizeof(path), "playback-v2.json")) return false;
    int temporary_length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (temporary_length < 0 || temporary_length >= (int)sizeof(temporary)) return false;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return false;
    FILE *file = fopen(temporary, "wb");
    bool saved = false;
    if (file) {
        size_t length = strlen(text);
        bool wrote = fwrite(text, 1, length, file) == length;
        saved = wrote && fclose(file) == 0 && rename(temporary, path) == 0;
        if (!saved) (void)unlink(temporary);
    }
    free(text);
    return saved;
}

static cJSON *vfh_resume_new_root(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "version", VFH_PLAYBACK_STORE_VERSION) ||
        !cJSON_AddObjectToObject(root, "history")) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool vfh_resume_root_valid(const cJSON *root) {
    cJSON *version = root ? cJSON_GetObjectItemCaseSensitive(root, "version") : NULL;
    return root && cJSON_IsObject(root) && cJSON_IsNumber(version) &&
           version->valueint == VFH_PLAYBACK_STORE_VERSION;
}

static cJSON *vfh_resume_history(cJSON *root) {
    cJSON *history = cJSON_GetObjectItemCaseSensitive(root, "history");
    if (cJSON_IsObject(history)) return history;
    cJSON_DeleteItemFromObjectCaseSensitive(root, "history");
    return cJSON_AddObjectToObject(root, "history");
}

static bool vfh_resume_identity_key(const vfh_resume_identity *identity,
                                    char *out, size_t out_size) {
    if (!identity || !identity->relative_path || !identity->relative_path[0] ||
        identity->source_index < 0 ||
        (identity->content_kind != VFH_CONTENT_VIDEO &&
         identity->content_kind != VFH_CONTENT_RECORDING)) return false;
    int length = snprintf(out, out_size, "%d:%d:%s", identity->content_kind,
                          identity->source_index, identity->relative_path);
    return length >= 0 && length < (int)out_size;
}

static bool vfh_resume_add_record(cJSON *history, const char *key, double resume_seconds,
                                  double duration_seconds, bool watched, time_t last_played,
                                  const vfh_resume_identity *identity) {
    cJSON *record = cJSON_CreateObject();
    if (!record || !cJSON_AddNumberToObject(record, "resume_seconds", resume_seconds) ||
        !cJSON_AddNumberToObject(record, "duration_seconds", duration_seconds) ||
        !cJSON_AddBoolToObject(record, "watched", watched) ||
        !cJSON_AddNumberToObject(record, "last_played", (double)last_played)) {
        cJSON_Delete(record);
        return false;
    }
    if (identity && (!cJSON_AddNumberToObject(record, "content_kind", identity->content_kind) ||
                     !cJSON_AddNumberToObject(record, "source_index", identity->source_index) ||
                     !cJSON_AddStringToObject(record, "relative_path", identity->relative_path))) {
        cJSON_Delete(record);
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(history, key);
    return cJSON_AddItemToObject(history, key, record);
}

static void vfh_resume_prune(cJSON *history) {
    while (cJSON_GetArraySize(history) > VFH_RESUME_MAX_ENTRIES) {
        cJSON *oldest = cJSON_GetArrayItem(history, 0);
        if (!oldest || !oldest->string) break;
        char key[1024];
        if (snprintf(key, sizeof(key), "%s", oldest->string) >= (int)sizeof(key)) break;
        cJSON_DeleteItemFromObjectCaseSensitive(history, key);
    }
}

/* Migrate the old flat path→seconds store only once a new atomic playback-v2
 * write succeeds. Queue writes preserve the history member, so either owner can
 * safely checkpoint its part of the common store on the UI thread. */
static void vfh_resume_migrate_legacy(cJSON *root, cJSON *history) {
    char legacy_path[1024];
    if (!vfh_resume_path(legacy_path, sizeof(legacy_path), "resume.json") ||
        cJSON_GetArraySize(history) > 0 || access(legacy_path, F_OK) != 0) return;
    cJSON *legacy = vfh_resume_read_json(legacy_path);
    if (!legacy) return;
    for (cJSON *entry = legacy->child; entry; entry = entry->next) {
        if (!entry->string || !cJSON_IsNumber(entry) || entry->valuedouble <= VFH_RESUME_MIN_SECONDS)
            continue;
        (void)vfh_resume_add_record(history, entry->string, entry->valuedouble, 0.0,
                                    false, 0, NULL);
    }
    vfh_resume_prune(history);
    /* Retire even an all-under-cutoff legacy map, but only after the empty or
     * populated v2 document is safely committed. Otherwise a failed FAT write
     * leaves the source of truth intact for the next launch. */
    if (vfh_resume_store(root)) (void)unlink(legacy_path);
    cJSON_Delete(legacy);
}

static cJSON *vfh_resume_load(void) {
    char path[1024];
    if (!vfh_resume_path(path, sizeof(path), "playback-v2.json")) return NULL;
    cJSON *root = vfh_resume_read_json(path);
    if (!vfh_resume_root_valid(root)) {
        cJSON_Delete(root);
        root = vfh_resume_new_root();
    }
    if (!root) return NULL;
    cJSON *history = vfh_resume_history(root);
    if (!history) { cJSON_Delete(root); return NULL; }
    vfh_resume_migrate_legacy(root, history);
    return root;
}

double vfh_resume_get(const char *path) {
    if (!path || !path[0]) return 0.0;
    cJSON *root = vfh_resume_load();
    if (!root) return 0.0;
    cJSON *history = vfh_resume_history(root);
    cJSON *record = history ? cJSON_GetObjectItemCaseSensitive(history, path) : NULL;
    cJSON *resume = record ? cJSON_GetObjectItemCaseSensitive(record, "resume_seconds") : NULL;
    double seconds = cJSON_IsNumber(resume) ? resume->valuedouble : 0.0;
    cJSON_Delete(root);
    return seconds > VFH_RESUME_MIN_SECONDS ? seconds : 0.0;
}

static double vfh_resume_record_seconds(const cJSON *record) {
    cJSON *resume = record ? cJSON_GetObjectItemCaseSensitive(record, "resume_seconds") : NULL;
    double seconds = cJSON_IsNumber(resume) ? resume->valuedouble : 0.0;
    return seconds > VFH_RESUME_MIN_SECONDS ? seconds : 0.0;
}

double vfh_resume_get_identity(const vfh_resume_identity *identity, const char *absolute_path) {
    char key[1024];
    if (!vfh_resume_identity_key(identity, key, sizeof(key))) return vfh_resume_get(absolute_path);
    cJSON *root = vfh_resume_load();
    if (!root) return 0.0;
    cJSON *history = vfh_resume_history(root);
    cJSON *record = history ? cJSON_GetObjectItemCaseSensitive(history, key) : NULL;
    if (!record && absolute_path && absolute_path[0] && history) {
        /* Legacy records were keyed by an absolute mount path. Once the catalog
           can match one, promote it to the durable source-relative identity. */
        cJSON *legacy = cJSON_GetObjectItemCaseSensitive(history, absolute_path);
        if (legacy) {
            cJSON *duration = cJSON_GetObjectItemCaseSensitive(legacy, "duration_seconds");
            cJSON *watched = cJSON_GetObjectItemCaseSensitive(legacy, "watched");
            cJSON *last_played = cJSON_GetObjectItemCaseSensitive(legacy, "last_played");
            double seconds = vfh_resume_record_seconds(legacy);
            double duration_seconds = cJSON_IsNumber(duration) ? duration->valuedouble : 0.0;
            bool is_watched = cJSON_IsTrue(watched);
            time_t played = cJSON_IsNumber(last_played) ? (time_t)last_played->valuedouble : 0;
            if (vfh_resume_add_record(history, key, seconds, duration_seconds, is_watched,
                                      played, identity)) {
                cJSON_DeleteItemFromObjectCaseSensitive(history, absolute_path);
                (void)vfh_resume_store(root);
                record = cJSON_GetObjectItemCaseSensitive(history, key);
            }
        }
    }
    double seconds = vfh_resume_record_seconds(record);
    cJSON_Delete(root);
    return seconds;
}

void vfh_resume_mark_watched(const char *path, double duration) {
    if (!path || !path[0]) return;
    cJSON *root = vfh_resume_load();
    if (!root) return;
    cJSON *history = vfh_resume_history(root);
    if (history && vfh_resume_add_record(history, path, 0.0, duration, true, time(NULL), NULL)) {
        vfh_resume_prune(history);
        (void)vfh_resume_store(root);
    }
    cJSON_Delete(root);
}

void vfh_resume_set(const char *path, double position, double duration) {
    if (!path || !path[0]) return;
    cJSON *root = vfh_resume_load();
    if (!root) return;
    cJSON *history = vfh_resume_history(root);
    if (!history) { cJSON_Delete(root); return; }

    bool finished = duration > 0.0 && position >= duration * VFH_RESUME_WATCHED_FRACTION;
    if (finished) {
        (void)vfh_resume_add_record(history, path, 0.0, duration, true, time(NULL), NULL);
    } else if (position > VFH_RESUME_MIN_SECONDS) {
        (void)vfh_resume_add_record(history, path, position, duration, false, time(NULL), NULL);
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(history, path);
    }
    vfh_resume_prune(history);
    (void)vfh_resume_store(root);
    cJSON_Delete(root);
}

void vfh_resume_set_identity(const vfh_resume_identity *identity, const char *absolute_path,
                             double position, double duration) {
    char key[1024];
    if (!vfh_resume_identity_key(identity, key, sizeof(key))) {
        vfh_resume_set(absolute_path, position, duration);
        return;
    }
    cJSON *root = vfh_resume_load();
    if (!root) return;
    cJSON *history = vfh_resume_history(root);
    if (!history) { cJSON_Delete(root); return; }
    bool finished = duration > 0.0 && position >= duration * VFH_RESUME_WATCHED_FRACTION;
    if (finished) {
        (void)vfh_resume_add_record(history, key, 0.0, duration, true, time(NULL), identity);
    } else if (position > VFH_RESUME_MIN_SECONDS) {
        (void)vfh_resume_add_record(history, key, position, duration, false, time(NULL), identity);
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(history, key);
        if (absolute_path && absolute_path[0])
            cJSON_DeleteItemFromObjectCaseSensitive(history, absolute_path);
    }
    vfh_resume_prune(history);
    (void)vfh_resume_store(root);
    cJSON_Delete(root);
}

void vfh_resume_mark_identity_watched(const vfh_resume_identity *identity,
                                      const char *absolute_path, double duration) {
    char key[1024];
    if (!vfh_resume_identity_key(identity, key, sizeof(key))) {
        vfh_resume_mark_watched(absolute_path, duration);
        return;
    }
    cJSON *root = vfh_resume_load();
    if (!root) return;
    cJSON *history = vfh_resume_history(root);
    if (history && vfh_resume_add_record(history, key, 0.0, duration, true, time(NULL), identity)) {
        if (absolute_path && absolute_path[0])
            cJSON_DeleteItemFromObjectCaseSensitive(history, absolute_path);
        vfh_resume_prune(history);
        (void)vfh_resume_store(root);
    }
    cJSON_Delete(root);
}
