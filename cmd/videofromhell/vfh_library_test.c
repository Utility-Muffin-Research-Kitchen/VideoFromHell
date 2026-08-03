#include "vfh_library.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

static void make_dir(const char *path) {
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void make_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite(content, 1, strlen(content), fp) == strlen(content));
    assert(fclose(fp) == 0);
}

static vfh_library_item *find_mutable(vfh_library *library, vfh_content_kind kind,
                                      int source, const char *relative) {
    for (size_t i = 0; i < library->count; i++) {
        vfh_library_item *item = &library->items[i];
        if (item->content_kind == kind && item->source_index == source &&
            strcmp(item->relative_path, relative) == 0) return item;
    }
    return NULL;
}

typedef struct {
    int checks;
    int cancel_on_check;
} scan_cancel_fixture;

static bool cancel_scan(void *opaque) {
    scan_cancel_fixture *fixture = opaque;
    return fixture && ++fixture->checks >= fixture->cancel_on_check;
}

int main(void) {
    char base[] = "/tmp/vfh-library-XXXXXX";
    assert(mkdtemp(base));
    char sd1[1024], sd2[1024], videos1[1024], videos2[1024], recordings[1024];
    char nested[1024], userdata[1024], list[2050], path[1200], error[256];
    snprintf(sd1, sizeof(sd1), "%s/sd1", base);
    snprintf(sd2, sizeof(sd2), "%s/sd2", base);
    snprintf(videos1, sizeof(videos1), "%s/Videos", sd1);
    snprintf(videos2, sizeof(videos2), "%s/Videos", sd2);
    snprintf(recordings, sizeof(recordings), "%s/Recordings", sd1);
    snprintf(userdata, sizeof(userdata), "%s/userdata", base);
    make_dir(sd1); make_dir(sd2); make_dir(videos1); make_dir(videos2);
    make_dir(recordings); make_dir(userdata);
    snprintf(nested, sizeof(nested), "%s/Movies", videos1); make_dir(nested);
    snprintf(path, sizeof(path), "%s/One.mp4", nested); make_file(path, "one");
    snprintf(path, sizeof(path), "%s/One.nfo", nested);
    make_file(path, "<movie><title>One: Local Title</title><year>2025</year></movie>");
    snprintf(path, sizeof(path), "%s/Two.mkv", videos2); make_file(path, "two");
    snprintf(path, sizeof(path), "%s/Game.mkv", recordings); make_file(path, "mkv");
    snprintf(path, sizeof(path), "%s/Game.mp4", recordings); make_file(path, "mp4");
    snprintf(path, sizeof(path), "%s/Split.mkv", recordings); make_file(path, "source");
    snprintf(path, sizeof(path), "%s/Split-part10.mp4", recordings); make_file(path, "ten");
    snprintf(path, sizeof(path), "%s/Split-part2.mp4", recordings); make_file(path, "two");
    snprintf(path, sizeof(path), "%s/unfinished.mkv", recordings); make_file(path, "");
    snprintf(path, sizeof(path), "%s/converter.mp4.part", recordings); make_file(path, "scratch");
    snprintf(path, sizeof(path), "%s/active.mkv", recordings); make_file(path, "open");
    int lock_fd = open(path, O_RDONLY);
    assert(lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) == 0);

    snprintf(list, sizeof(list), "%s:%s", videos1, videos2);
    vfh_sources sources;
    assert(vfh_sources_parse(&sources, list, error, sizeof(error)));
    setenv("USERDATA_PATH", userdata, 1);
    vfh_library library;
    vfh_library_init(&library);
    assert(vfh_library_scan(&library, &sources, recordings, error, sizeof(error)));
    assert(library.count == 5);
    assert(vfh_library_find(&library, VFH_CONTENT_VIDEO, 0, "Movies/One.mp4"));
    assert(vfh_library_find(&library, VFH_CONTENT_VIDEO, 1, "Two.mkv"));
    assert(vfh_library_find(&library, VFH_CONTENT_RECORDING, 0, "Game.mp4"));
    assert(!vfh_library_find(&library, VFH_CONTENT_RECORDING, 0, "Game.mkv"));
    assert(!vfh_library_find(&library, VFH_CONTENT_RECORDING, 0, "active.mkv"));
    const vfh_library_item *part2 = vfh_library_find(&library, VFH_CONTENT_RECORDING, 0,
                                                      "Split-part2.mp4");
    const vfh_library_item *part10 = vfh_library_find(&library, VFH_CONTENT_RECORDING, 0,
                                                       "Split-part10.mp4");
    assert(part2 && part10 && part2->recording_part == 2 && part10->recording_part == 10);
    assert(part2 < part10);
    assert(vfh_library_resolve_path(part2, &sources, recordings, path, sizeof(path)));
    assert(strstr(path, "/Recordings/Split-part2.mp4") != NULL);
    vfh_library_item *one = find_mutable(&library, VFH_CONTENT_VIDEO, 0, "Movies/One.mp4");
    assert(one);
    assert(!strcmp(one->display_title, "One: Local Title") && one->nfo_title);
    assert(one->year == 2025 && one->nfo_year);
    one->duration = 123.0;
    one->metadata_ready = true;
    snprintf(one->container, sizeof(one->container), "mov,mp4,m4a,3gp,3g2,mj2");
    snprintf(one->video_codec, sizeof(one->video_codec), "h264");
    snprintf(one->audio_codec, sizeof(one->audio_codec), "aac");
    one->has_embedded_art = true;
    one->art_source = VFH_LIBRARY_ART_EMBEDDED;
    assert(vfh_library_save(&library));

    vfh_library warm;
    vfh_library_init(&warm);
    assert(vfh_library_load(&warm));
    const vfh_library_item *warm_one = vfh_library_find(&warm, VFH_CONTENT_VIDEO, 0,
                                                        "Movies/One.mp4");
    assert(warm_one && warm_one->duration == 123.0 && warm_one->metadata_ready &&
           warm_one->has_embedded_art && warm_one->art_source == VFH_LIBRARY_ART_EMBEDDED &&
           !strcmp(warm_one->video_codec, "h264") && warm_one->available);
    assert(vfh_library_scan(&warm, &sources, recordings, error, sizeof(error)));
    warm_one = vfh_library_find(&warm, VFH_CONTENT_VIDEO, 0, "Movies/One.mp4");
    assert(warm_one && warm_one->available && warm_one->duration == 123.0);

    /* Cancellation is transactional: the worker can discard an interrupted
       scan without publishing a partial catalog over the warm cache. */
    snprintf(path, sizeof(path), "%s/Cancelled.mp4", videos2); make_file(path, "later");
    size_t count_before_cancel = warm.count;
    scan_cancel_fixture cancel = { .cancel_on_check = 5 };
    assert(vfh_library_scan_cancellable(&warm, &sources, recordings, cancel_scan, &cancel,
                                        error, sizeof(error)) == VFH_LIBRARY_SCAN_CANCELLED);
    assert(warm.count == count_before_cancel);
    assert(!vfh_library_find(&warm, VFH_CONTENT_VIDEO, 1, "Cancelled.mp4"));
    assert(vfh_library_scan(&warm, &sources, recordings, error, sizeof(error)));
    assert(vfh_library_find(&warm, VFH_CONTENT_VIDEO, 1, "Cancelled.mp4"));

    snprintf(path, sizeof(path), "%s/One.nfo", nested);
    assert(unlink(path) == 0);
    assert(vfh_library_scan(&warm, &sources, recordings, error, sizeof(error)));
    warm_one = vfh_library_find(&warm, VFH_CONTENT_VIDEO, 0, "Movies/One.mp4");
    assert(warm_one && !strcmp(warm_one->display_title, "One") && !warm_one->nfo_title &&
           warm_one->year == 0 && !warm_one->nfo_year && !warm_one->metadata_ready &&
           warm_one->duration == 0.0);

    snprintf(path, sizeof(path), "%s/One.mp4", nested); make_file(path, "changed");
    struct utimbuf newer = { .actime = time(NULL) + 2, .modtime = time(NULL) + 2 };
    assert(utime(path, &newer) == 0);
    assert(vfh_library_scan(&warm, &sources, recordings, error, sizeof(error)));
    warm_one = vfh_library_find(&warm, VFH_CONTENT_VIDEO, 0, "Movies/One.mp4");
    assert(warm_one && warm_one->duration == 0.0 && !warm_one->metadata_ready);

    char hidden_recordings[1024];
    snprintf(hidden_recordings, sizeof(hidden_recordings), "%s/Recordings-hidden", sd1);
    assert(rename(recordings, hidden_recordings) == 0);
    assert(vfh_library_scan(&warm, &sources, recordings, error, sizeof(error)));
    const vfh_library_item *game = vfh_library_find(&warm, VFH_CONTENT_RECORDING, 0, "Game.mp4");
    assert(game && !game->available);
    assert(rename(hidden_recordings, recordings) == 0);

    sources.items[1].available = 0;
    assert(vfh_library_scan(&warm, &sources, recordings, error, sizeof(error)));
    const vfh_library_item *two = vfh_library_find(&warm, VFH_CONTENT_VIDEO, 1, "Two.mkv");
    assert(two && !two->available);
    sources.items[1].available = 1;
    snprintf(path, sizeof(path), "%s/One.mp4", nested);
    assert(unlink(path) == 0);
    assert(vfh_library_scan(&warm, &sources, recordings, error, sizeof(error)));
    assert(!vfh_library_find(&warm, VFH_CONTENT_VIDEO, 0, "Movies/One.mp4"));

    vfh_library_destroy(&warm);
    close(lock_fd);
    snprintf(path, sizeof(path), "%s/VideoFromHell/library-v2.json", userdata);
    make_file(path, "{corrupt");
    vfh_library corrupt;
    vfh_library_init(&corrupt);
    assert(!vfh_library_load(&corrupt) && corrupt.count == 0);
    vfh_library_destroy(&corrupt);
    vfh_library_destroy(&library);
    puts("vfh_library_test: ok");
    return 0;
}
