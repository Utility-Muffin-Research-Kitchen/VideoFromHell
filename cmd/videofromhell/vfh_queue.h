#ifndef VFH_QUEUE_H
#define VFH_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "vfh_library.h"

#define VFH_QUEUE_MAX_ITEMS 256

typedef struct {
    vfh_content_kind content_kind;
    int source_index;
    char relative_path[VFH_LIBRARY_RELATIVE_PATH_MAX];
} vfh_queue_item;

typedef struct {
    vfh_queue_item items[VFH_QUEUE_MAX_ITEMS];
    size_t count;
} vfh_queue;

void vfh_queue_init(vfh_queue *queue);
bool vfh_queue_append(vfh_queue *queue, const vfh_queue_item *item);
bool vfh_queue_play_next(vfh_queue *queue, const vfh_queue_item *item, size_t current_index);
bool vfh_queue_remove(vfh_queue *queue, size_t index);
bool vfh_queue_move(vfh_queue *queue, size_t index, int delta);
void vfh_queue_clear(vfh_queue *queue);
bool vfh_queue_load(vfh_queue *queue);
bool vfh_queue_save(const vfh_queue *queue);

#endif
