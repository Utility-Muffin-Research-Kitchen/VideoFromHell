#include "vfh_status.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void read_all(int fd, void *buffer, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t read_count = read(fd, (char *)buffer + offset, size - offset);
        assert(read_count > 0);
        offset += (size_t)read_count;
    }
}

static void write_all(int fd, const void *buffer, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t write_count = write(fd, (const char *)buffer + offset, size - offset);
        assert(write_count > 0);
        offset += (size_t)write_count;
    }
}

static void serve_statuses(int listener) {
    static const char *const replies[] = {
        "{\"status\":{\"audio_output\":\"SPEAKER\"}}",
        "{\"status\":{\"audio_output\":\"BLUETOOTH\"}}",
        "{\"status\":{\"audio_output\":\"HEADSET\"}}",
    };
    for (size_t index = 0; index < sizeof(replies) / sizeof(replies[0]); index++) {
        int client = accept(listener, NULL, NULL);
        assert(client >= 0);
        uint32_t request_length = 0;
        read_all(client, &request_length, sizeof(request_length));
        request_length = ntohl(request_length);
        assert(request_length > 0 && request_length < 1024);
        char request[1024];
        read_all(client, request, request_length);
        assert(strstr(request, "platform-audio-status") != NULL);
        uint32_t reply_length = htonl((uint32_t)strlen(replies[index]));
        write_all(client, &reply_length, sizeof(reply_length));
        write_all(client, replies[index], strlen(replies[index]));
        close(client);
    }
}

int main(void) {
    char runtime[] = "/tmp/videofromhell-status-XXXXXX";
    assert(mkdtemp(runtime));
    assert(setenv("JAWAKA_RUNTIME_DIR", runtime, 1) == 0);

    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    assert(snprintf(address.sun_path, sizeof(address.sun_path), "%s/jawakad.sock", runtime) <
           (int)sizeof(address.sun_path));
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 3) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        serve_statuses(listener);
        close(listener);
        _exit(0);
    }
    close(listener);

    vfh_audio_status status;
    assert(vfh_status_query(&status));
    assert(status.ok && strcmp(status.output, "SPEAKER") == 0);

    vfh_status_monitor monitor;
    vfh_status_monitor_init(&monitor);
    assert(vfh_status_monitor_start(&monitor));
    char output[VFH_AUDIO_OUTPUT_MAX] = "";
    for (int attempt = 0; attempt < 30; attempt++) {
        if (vfh_status_monitor_output(&monitor, output, sizeof(output)) &&
            strcmp(output, "HEADSET") == 0) break;
        usleep(100 * 1000);
    }
    assert(strcmp(output, "HEADSET") == 0);
    vfh_status_monitor_destroy(&monitor);

    int child_status = 0;
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    unsetenv("JAWAKA_RUNTIME_DIR");
    puts("vfh_status_test: ok");
    return 0;
}
