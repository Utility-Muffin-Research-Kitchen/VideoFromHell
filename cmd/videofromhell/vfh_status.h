#ifndef VFH_STATUS_H
#define VFH_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#include <pthread.h>

#define VFH_AUDIO_OUTPUT_MAX 16

typedef struct {
    char output[VFH_AUDIO_OUTPUT_MAX];
    bool ok;
} vfh_audio_status;

/* A short, framed IPC query to Jawaka's live platform state. The launch-time
 * environment is only a fallback; it does not change when a jack or BT route
 * changes while VFH is running. */
bool vfh_status_query(vfh_audio_status *status);

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    atomic_bool stop;
    bool started;
    char output[VFH_AUDIO_OUTPUT_MAX];
} vfh_status_monitor;

void vfh_status_monitor_init(vfh_status_monitor *monitor);
bool vfh_status_monitor_start(vfh_status_monitor *monitor);
void vfh_status_monitor_destroy(vfh_status_monitor *monitor);
bool vfh_status_monitor_output(vfh_status_monitor *monitor, char *out, size_t out_size);

#endif
