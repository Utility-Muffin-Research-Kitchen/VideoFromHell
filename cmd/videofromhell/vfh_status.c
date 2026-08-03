#include "vfh_status.h"

#include "cJSON.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define VFH_STATUS_MAX_REPLY 65536u
#define VFH_STATUS_POLL_MS 500

static int vfh_status_io_all(int fd, void *buffer, size_t size, bool writing) {
    char *cursor = buffer;
    size_t offset = 0;
    while (offset < size) {
        ssize_t result = writing ? write(fd, cursor + offset, size - offset)
                                 : read(fd, cursor + offset, size - offset);
        if (result <= 0) return -1;
        offset += (size_t)result;
    }
    return 0;
}

bool vfh_status_query(vfh_audio_status *status) {
    if (!status) return false;
    memset(status, 0, sizeof(*status));

    const char *runtime = getenv("JAWAKA_RUNTIME_DIR");
    if (!runtime || !runtime[0]) runtime = "/tmp/jawaka-runtime";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 250000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    int socket_length = snprintf(address.sun_path, sizeof(address.sun_path), "%s/jawakad.sock",
                                 runtime);
    if (socket_length < 0 || socket_length >= (int)sizeof(address.sun_path) ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return false;
    }

    static const char request[] = "{\"type\":\"platform-audio-status\"}";
    uint32_t request_length = htonl((uint32_t)(sizeof(request) - 1));
    if (vfh_status_io_all(fd, &request_length, sizeof(request_length), true) != 0 ||
        vfh_status_io_all(fd, (void *)request, sizeof(request) - 1, true) != 0) {
        close(fd);
        return false;
    }

    uint32_t reply_length = 0;
    if (vfh_status_io_all(fd, &reply_length, sizeof(reply_length), false) != 0) {
        close(fd);
        return false;
    }
    reply_length = ntohl(reply_length);
    if (!reply_length || reply_length > VFH_STATUS_MAX_REPLY) {
        close(fd);
        return false;
    }
    char *reply = malloc((size_t)reply_length + 1);
    if (!reply) { close(fd); return false; }
    bool read = vfh_status_io_all(fd, reply, reply_length, false) == 0;
    close(fd);
    if (!read) { free(reply); return false; }
    reply[reply_length] = '\0';

    cJSON *root = cJSON_Parse(reply);
    free(reply);
    cJSON *state = root ? cJSON_GetObjectItemCaseSensitive(root, "status") : NULL;
    cJSON *output = state ? cJSON_GetObjectItemCaseSensitive(state, "audio_output") : NULL;
    if (!root || !cJSON_IsObject(state) || !cJSON_IsString(output) || !output->valuestring ||
        snprintf(status->output, sizeof(status->output), "%s", output->valuestring) >=
            (int)sizeof(status->output)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);
    status->ok = true;
    return true;
}

static void *vfh_status_monitor_thread(void *opaque) {
    vfh_status_monitor *monitor = opaque;
    while (!atomic_load(&monitor->stop)) {
        vfh_audio_status status;
        if (vfh_status_query(&status) && status.output[0]) {
            pthread_mutex_lock(&monitor->mutex);
            snprintf(monitor->output, sizeof(monitor->output), "%s", status.output);
            pthread_mutex_unlock(&monitor->mutex);
        }
        for (int elapsed = 0; elapsed < VFH_STATUS_POLL_MS && !atomic_load(&monitor->stop);
             elapsed += 50)
            usleep(50 * 1000);
    }
    return NULL;
}

void vfh_status_monitor_init(vfh_status_monitor *monitor) {
    if (!monitor) return;
    memset(monitor, 0, sizeof(*monitor));
    atomic_init(&monitor->stop, false);
    pthread_mutex_init(&monitor->mutex, NULL);
}

bool vfh_status_monitor_start(vfh_status_monitor *monitor) {
    if (!monitor || monitor->started) return false;
    atomic_store(&monitor->stop, false);
    if (pthread_create(&monitor->thread, NULL, vfh_status_monitor_thread, monitor) != 0)
        return false;
    monitor->started = true;
    return true;
}

void vfh_status_monitor_destroy(vfh_status_monitor *monitor) {
    if (!monitor) return;
    if (monitor->started) {
        atomic_store(&monitor->stop, true);
        pthread_join(monitor->thread, NULL);
        monitor->started = false;
    }
    pthread_mutex_destroy(&monitor->mutex);
}

bool vfh_status_monitor_output(vfh_status_monitor *monitor, char *out, size_t out_size) {
    if (!monitor || !out || !out_size) return false;
    pthread_mutex_lock(&monitor->mutex);
    int length = snprintf(out, out_size, "%s", monitor->output);
    pthread_mutex_unlock(&monitor->mutex);
    return length > 0 && length < (int)out_size;
}
