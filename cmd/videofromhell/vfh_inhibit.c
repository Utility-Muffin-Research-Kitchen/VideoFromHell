#include "vfh_inhibit.h"
#include "cJSON.h"

#include <arpa/inet.h>      /* htonl / ntohl */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* The daemon frames messages as a 4-byte big-endian length + JSON payload
   (Jawaka/internal/ipc/ipc.c). Same contract Disco Boy uses for its audio
   status poll. */

/* Token from the daemon; empty when we hold nothing. */
static char vfh_lease_token[64];

static int vfh_io_all(int fd, void *buf, size_t n, bool writing) {
    char *p = (char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = writing ? write(fd, p + off, n - off) : read(fd, p + off, n - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int vfh_daemon_connect(void) {
    const char *path = getenv("UMRK_DAEMON_SOCKET");
    if (!path || !path[0]) path = getenv("JAWAKA_SOCKET_PATH");
    char fallback[256];
    if (!path || !path[0]) {
        const char *rt = getenv("JAWAKA_RUNTIME_DIR");
        if (!rt || !rt[0]) rt = getenv("UMRK_RUNTIME_PATH");
        if (!rt || !rt[0]) rt = "/tmp/jawaka-runtime";
        snprintf(fallback, sizeof(fallback), "%s/jawakad.sock", rt);
        path = fallback;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    /* Short timeouts: this runs on the UI thread and must never stall a frame. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) >=
        sizeof(addr.sun_path)) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

/* Send one request, return the parsed reply (caller frees) or NULL. */
static cJSON *vfh_daemon_call(cJSON *request) {
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) return NULL;
    int fd = vfh_daemon_connect();
    if (fd < 0) { free(body); return NULL; }

    size_t body_len = strlen(body);
    uint32_t len = htonl((uint32_t)body_len);
    if (vfh_io_all(fd, &len, 4, true) != 0 || vfh_io_all(fd, body, body_len, true) != 0) {
        free(body); close(fd); return NULL;
    }
    free(body);

    uint32_t reply_len = 0;
    if (vfh_io_all(fd, &reply_len, 4, false) != 0) { close(fd); return NULL; }
    reply_len = ntohl(reply_len);
    if (reply_len == 0 || reply_len > 65536) { close(fd); return NULL; }
    char *buf = (char *)malloc(reply_len + 1);
    if (!buf) { close(fd); return NULL; }
    if (vfh_io_all(fd, buf, reply_len, false) != 0) { free(buf); close(fd); return NULL; }
    buf[reply_len] = '\0';
    close(fd);

    cJSON *reply = cJSON_Parse(buf);
    free(buf);
    return reply;
}

bool vfh_inhibit_held(void) { return vfh_lease_token[0] != '\0'; }

void vfh_inhibit_acquire(const char *reason) {
    if (vfh_inhibit_held()) return;
    cJSON *request = cJSON_CreateObject();
    if (!request) return;
    cJSON_AddStringToObject(request, "type", "suspend-inhibit-acquire");
    cJSON_AddStringToObject(request, "scope", "block-screen");
    cJSON_AddStringToObject(request, "reason", reason && reason[0] ? reason : "playback");
    cJSON *reply = vfh_daemon_call(request);
    if (!reply) return;
    cJSON *token = cJSON_GetObjectItemCaseSensitive(reply, "token");
    if (cJSON_IsString(token) && token->valuestring)
        snprintf(vfh_lease_token, sizeof(vfh_lease_token), "%s", token->valuestring);
    cJSON_Delete(reply);
}

void vfh_inhibit_release(void) {
    if (!vfh_inhibit_held()) return;
    cJSON *request = cJSON_CreateObject();
    if (request) {
        cJSON_AddStringToObject(request, "type", "suspend-inhibit-release");
        cJSON_AddStringToObject(request, "token", vfh_lease_token);
        cJSON *reply = vfh_daemon_call(request);
        cJSON_Delete(reply);
    }
    /* Clear regardless: if the daemon is gone it has already dropped our lease
       (pid-tied reap), and holding a stale token would block a later acquire. */
    vfh_lease_token[0] = '\0';
}
