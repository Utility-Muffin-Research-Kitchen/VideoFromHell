#include "vfh_queue.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VFH_QUEUE_STORE_VERSION 2
#define VFH_QUEUE_STORE_MAX_BYTES (1 << 20)

static bool vfh_queue_path(char *out, size_t out_size) {
    const char *userdata = getenv("USERDATA_PATH");
    if (!userdata || !userdata[0]) userdata = getenv("SHARED_USERDATA_PATH");
    if (!userdata || !userdata[0]) return false;

    char directory[1024];
    int directory_length = snprintf(directory, sizeof(directory), "%s/VideoFromHell", userdata);
    if (directory_length < 0 || directory_length >= (int)sizeof(directory)) return false;
    (void)mkdir(directory, 0755);

    int path_length = snprintf(out, out_size, "%s/playback-v2.json", directory);
    return path_length >= 0 && path_length < (int)out_size;
}

static bool vfh_queue_item_valid(const vfh_queue_item *item) {
    return item && (item->content_kind == VFH_CONTENT_VIDEO ||
                    item->content_kind == VFH_CONTENT_RECORDING) &&
           item->source_index >= 0 && item->relative_path[0] &&
           item->relative_path[0] != '/' && !strstr(item->relative_path, "../");
}

static cJSON *vfh_queue_read_root(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 1 || length > VFH_QUEUE_STORE_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
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
    return root;
}

static bool vfh_queue_root_valid(const cJSON *root) {
    cJSON *version = root ? cJSON_GetObjectItemCaseSensitive(root, "version") : NULL;
    return root && cJSON_IsObject(root) && cJSON_IsNumber(version) &&
           version->valueint == VFH_QUEUE_STORE_VERSION;
}

static cJSON *vfh_queue_new_root(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "version", VFH_QUEUE_STORE_VERSION)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool vfh_queue_store(const cJSON *root, const char *path) {
    char temporary[1216];
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

void vfh_queue_init(vfh_queue *queue) {
    if (queue) memset(queue, 0, sizeof(*queue));
}

bool vfh_queue_append(vfh_queue *queue, const vfh_queue_item *item) {
    if (!queue || !vfh_queue_item_valid(item) || queue->count >= VFH_QUEUE_MAX_ITEMS) return false;
    queue->items[queue->count++] = *item;
    return true;
}

bool vfh_queue_play_next(vfh_queue *queue, const vfh_queue_item *item, size_t current_index) {
    if (!queue || !vfh_queue_item_valid(item) || queue->count >= VFH_QUEUE_MAX_ITEMS) return false;
    if (current_index > queue->count) current_index = queue->count;
    memmove(&queue->items[current_index + 1], &queue->items[current_index],
            (queue->count - current_index) * sizeof(*queue->items));
    queue->items[current_index] = *item;
    queue->count++;
    return true;
}

bool vfh_queue_remove(vfh_queue *queue, size_t index) {
    if (!queue || index >= queue->count) return false;
    memmove(&queue->items[index], &queue->items[index + 1],
            (queue->count - index - 1) * sizeof(*queue->items));
    queue->count--;
    return true;
}

void vfh_queue_clear(vfh_queue *queue) {
    if (queue) queue->count = 0;
}

bool vfh_queue_save(const vfh_queue *queue) {
    char path[1200];
    if (!queue || !vfh_queue_path(path, sizeof(path))) return false;
    cJSON *root = vfh_queue_read_root(path);
    if (!vfh_queue_root_valid(root)) {
        cJSON_Delete(root);
        root = vfh_queue_new_root();
    }
    if (!root) return false;

    cJSON_DeleteItemFromObjectCaseSensitive(root, "queue");
    cJSON *items = cJSON_AddArrayToObject(root, "queue");
    if (!items) { cJSON_Delete(root); return false; }
    for (size_t index = 0; index < queue->count; index++) {
        const vfh_queue_item *item = &queue->items[index];
        if (!vfh_queue_item_valid(item)) { cJSON_Delete(root); return false; }
        cJSON *entry = cJSON_CreateObject();
        if (!entry || !cJSON_AddNumberToObject(entry, "kind", item->content_kind) ||
            !cJSON_AddNumberToObject(entry, "source", item->source_index) ||
            !cJSON_AddStringToObject(entry, "path", item->relative_path)) {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return false;
        }
        cJSON_AddItemToArray(items, entry);
    }
    bool saved = vfh_queue_store(root, path);
    cJSON_Delete(root);
    return saved;
}

bool vfh_queue_load(vfh_queue *queue) {
    char path[1200];
    if (!queue) return false;
    vfh_queue_init(queue);
    if (!vfh_queue_path(path, sizeof(path))) return false;
    cJSON *root = vfh_queue_read_root(path);
    cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "queue") : NULL;
    if (!vfh_queue_root_valid(root) || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(items);
    if (count < 0 || count > VFH_QUEUE_MAX_ITEMS) {
        cJSON_Delete(root);
        return false;
    }
    for (int index = 0; index < count; index++) {
        cJSON *entry = cJSON_GetArrayItem(items, index);
        cJSON *kind = entry ? cJSON_GetObjectItemCaseSensitive(entry, "kind") : NULL;
        cJSON *source = entry ? cJSON_GetObjectItemCaseSensitive(entry, "source") : NULL;
        cJSON *relative_path = entry ? cJSON_GetObjectItemCaseSensitive(entry, "path") : NULL;
        vfh_queue_item item = { 0 };
        if (!cJSON_IsNumber(kind) || !cJSON_IsNumber(source) || !cJSON_IsString(relative_path)) {
            cJSON_Delete(root);
            vfh_queue_init(queue);
            return false;
        }
        item.content_kind = (vfh_content_kind)kind->valueint;
        item.source_index = source->valueint;
        int path_length = snprintf(item.relative_path, sizeof(item.relative_path), "%s",
                                   relative_path->valuestring);
        if (path_length < 0 || path_length >= (int)sizeof(item.relative_path) ||
            !vfh_queue_append(queue, &item)) {
            cJSON_Delete(root);
            vfh_queue_init(queue);
            return false;
        }
    }
    cJSON_Delete(root);
    return true;
}
