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
    char nested[1024], nested2[1024], depth[1024], userdata[1024], list[2050], path[1200], error[256];
    snprintf(sd1, sizeof(sd1), "%s/sd1", base);
    snprintf(sd2, sizeof(sd2), "%s/sd2", base);
    snprintf(videos1, sizeof(videos1), "%s/Videos", sd1);
    snprintf(videos2, sizeof(videos2), "%s/Videos", sd2);
    snprintf(recordings, sizeof(recordings), "%s/Recordings", sd1);
    snprintf(userdata, sizeof(userdata), "%s/userdata", base);
    make_dir(sd1); make_dir(sd2); make_dir(videos1); make_dir(videos2);
    make_dir(recordings); make_dir(userdata);
    snprintf(nested, sizeof(nested), "%s/Movies", videos1); make_dir(nested);
    snprintf(nested2, sizeof(nested2), "%s/Movies", videos2); make_dir(nested2);
    snprintf(path, sizeof(path), "%s/One.mp4", nested); make_file(path, "one");
    snprintf(path, sizeof(path), "%s/One.nfo", nested);
    make_file(path, "<movie><title>One: Local Title</title><year>2025</year></movie>");
    snprintf(path, sizeof(path), "%s/Two.mkv", videos2); make_file(path, "two");
    snprintf(path, sizeof(path), "%s/One.mp4", nested2); make_file(path, "other one");
    snprintf(path, sizeof(path), "%s/Movie.2019.1080p.x264-GROUP.mkv", videos1);
    make_file(path, "movie");
    snprintf(path, sizeof(path), "%s/Studio.1080.mkv", videos1); make_file(path, "studio");
    snprintf(path, sizeof(path), "%s/Spider-Man.2021.mkv", videos1); make_file(path, "spider");
    snprintf(path, sizeof(path), "%s/Same.Title.mkv", videos1); make_file(path, "same one");
    snprintf(path, sizeof(path), "%s/Same_Title.mkv", videos2); make_file(path, "same two");
    snprintf(depth, sizeof(depth), "%s/Depth1", videos1); make_dir(depth);
    for (int level = 2; level <= 6; level++) {
        char next[1024];
        snprintf(next, sizeof(next), "%s/Depth%d", depth, level);
        make_dir(next);
        snprintf(depth, sizeof(depth), "%s", next);
    }
    snprintf(path, sizeof(path), "%s/Allowed.mp4", depth); make_file(path, "depth six");
    {
        char next[1024];
        snprintf(next, sizeof(next), "%s/Depth7", depth);
        snprintf(depth, sizeof(depth), "%s", next);
    }
    make_dir(depth);
    snprintf(path, sizeof(path), "%s/Hidden.mp4", depth); make_file(path, "depth seven");
    snprintf(path, sizeof(path), "%s/Game.mkv", recordings); make_file(path, "mkv");
    snprintf(path, sizeof(path), "%s/Game.mp4", recordings); make_file(path, "mp4");
    snprintf(path, sizeof(path), "%s/Sonic-2026-08-03_14-23-12.mkv", recordings);
    make_file(path, "source");
    snprintf(path, sizeof(path), "%s/Sonic-2026-08-03_14-23-12-part10.mp4", recordings);
    make_file(path, "ten");
    snprintf(path, sizeof(path), "%s/Sonic-2026-08-03_14-23-12-part2.mp4", recordings);
    make_file(path, "two");
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
    assert(library.count == 12);
    assert(vfh_library_find(&library, VFH_CONTENT_VIDEO, 0, "Movies/One.mp4"));
    assert(vfh_library_find(&library, VFH_CONTENT_VIDEO, 1, "Movies/One.mp4"));
    assert(vfh_library_find(&library, VFH_CONTENT_VIDEO, 1, "Two.mkv"));
    assert(vfh_library_find(&library, VFH_CONTENT_VIDEO, 0,
                            "Depth1/Depth2/Depth3/Depth4/Depth5/Depth6/Allowed.mp4"));
    assert(!vfh_library_find(&library, VFH_CONTENT_VIDEO, 0,
                             "Depth1/Depth2/Depth3/Depth4/Depth5/Depth6/Depth7/Hidden.mp4"));
    assert(vfh_library_find(&library, VFH_CONTENT_RECORDING, 0, "Game.mp4"));
    assert(!vfh_library_find(&library, VFH_CONTENT_RECORDING, 0, "Game.mkv"));
    assert(!vfh_library_find(&library, VFH_CONTENT_RECORDING, 0, "converter.mp4.part"));
    assert(!vfh_library_find(&library, VFH_CONTENT_RECORDING, 0, "active.mkv"));
    const vfh_library_item *movie = vfh_library_find(&library, VFH_CONTENT_VIDEO, 0,
                                                      "Movie.2019.1080p.x264-GROUP.mkv");
    const vfh_library_item *studio = vfh_library_find(&library, VFH_CONTENT_VIDEO, 0,
                                                       "Studio.1080.mkv");
    const vfh_library_item *spider = vfh_library_find(&library, VFH_CONTENT_VIDEO, 0,
                                                       "Spider-Man.2021.mkv");
    assert(movie && !strcmp(movie->display_title, "Movie 2019"));
    assert(studio && !strcmp(studio->display_title, "Studio 1080"));
    assert(spider && !strcmp(spider->display_title, "Spider-Man 2021"));
    const vfh_library_item *same_one = vfh_library_find(&library, VFH_CONTENT_VIDEO, 0,
                                                         "Same.Title.mkv");
    const vfh_library_item *same_two = vfh_library_find(&library, VFH_CONTENT_VIDEO, 1,
                                                         "Same_Title.mkv");
    assert(same_one && same_two && same_one != same_two &&
           !strcmp(same_one->display_title, "Same Title") &&
           !strcmp(same_two->display_title, "Same Title"));
    const vfh_library_item *part2 = vfh_library_find(&library, VFH_CONTENT_RECORDING, 0,
                                                      "Sonic-2026-08-03_14-23-12-part2.mp4");
    const vfh_library_item *part10 = vfh_library_find(&library, VFH_CONTENT_RECORDING, 0,
                                                       "Sonic-2026-08-03_14-23-12-part10.mp4");
    assert(part2 && part10 && part2->recording_part == 2 && part10->recording_part == 10);
    assert(!strcmp(part2->display_title, "Sonic") &&
           part2->capture_timestamp == INT64_C(20260803142312));
    assert(part2 < part10);
    assert(vfh_library_resolve_path(part2, &sources, recordings, path, sizeof(path)));
    assert(strstr(path, "/Recordings/Sonic-2026-08-03_14-23-12-part2.mp4") != NULL);
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
    const vfh_library_item *warm_part2 = vfh_library_find(
        &warm, VFH_CONTENT_RECORDING, 0, "Sonic-2026-08-03_14-23-12-part2.mp4");
    assert(warm_part2 && warm_part2->capture_timestamp == INT64_C(20260803142312) &&
           warm_part2->recording_part == 2);
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

    /* An unreadable folder is a local problem. It must not discard the other
       card's library, and its unseen files must not be pruned as deletions. */
    char locked_base[1024], locked_v1[1024], locked_v2[1024], locked_sub[1024];
    char locked_userdata[1024], locked_list[2100];
    snprintf(locked_base, sizeof(locked_base), "%s/partial", base);
    make_dir(locked_base);
    snprintf(locked_v1, sizeof(locked_v1), "%s/Videos1", locked_base);
    snprintf(locked_v2, sizeof(locked_v2), "%s/Videos2", locked_base);
    snprintf(locked_userdata, sizeof(locked_userdata), "%s/userdata", locked_base);
    make_dir(locked_v1); make_dir(locked_v2); make_dir(locked_userdata);
    setenv("USERDATA_PATH", locked_userdata, 1);
    snprintf(path, sizeof(path), "%s/Good.mp4", locked_v1); make_file(path, "good");
    snprintf(path, sizeof(path), "%s/AlsoGood.mp4", locked_v2); make_file(path, "good2");
    snprintf(locked_sub, sizeof(locked_sub), "%s/Locked", locked_v2); make_dir(locked_sub);
    snprintf(path, sizeof(path), "%s/Inside.mp4", locked_sub); make_file(path, "inside");
    snprintf(locked_list, sizeof(locked_list), "%s:%s", locked_v1, locked_v2);
    vfh_sources locked_sources;
    assert(vfh_sources_parse(&locked_sources, locked_list, error, sizeof(error)));
    vfh_library locked;
    vfh_library_init(&locked);
    assert(vfh_library_scan(&locked, &locked_sources, NULL, error, sizeof(error)));
    assert(vfh_library_find(&locked, VFH_CONTENT_VIDEO, 1, "Locked/Inside.mp4"));

    assert(chmod(locked_sub, 0000) == 0);
    assert(vfh_library_scan_cancellable(&locked, &locked_sources, NULL, NULL, NULL,
                                        error, sizeof(error)) == VFH_LIBRARY_SCAN_PARTIAL);
    assert(error[0]);
    /* Both readable cards survive, and the cached record under the unreadable
       folder is retained but marked unavailable rather than deleted. */
    const vfh_library_item *good = vfh_library_find(&locked, VFH_CONTENT_VIDEO, 0, "Good.mp4");
    const vfh_library_item *also = vfh_library_find(&locked, VFH_CONTENT_VIDEO, 1, "AlsoGood.mp4");
    const vfh_library_item *inside = vfh_library_find(&locked, VFH_CONTENT_VIDEO, 1,
                                                      "Locked/Inside.mp4");
    assert(good && good->available);
    assert(also && also->available);
    assert(inside && !inside->available);
    assert(chmod(locked_sub, 0700) == 0);
    /* Once readable again a complete scan reports no warning and restores it. */
    assert(vfh_library_scan_cancellable(&locked, &locked_sources, NULL, NULL, NULL,
                                        error, sizeof(error)) == VFH_LIBRARY_SCAN_COMPLETE);
    assert(!error[0]);
    inside = vfh_library_find(&locked, VFH_CONTENT_VIDEO, 1, "Locked/Inside.mp4");
    assert(inside && inside->available);

    vfh_library_destroy(&locked);
    setenv("USERDATA_PATH", userdata, 1);

    /* A regular file is neither an empty Videos directory nor an absent card:
       once it is declared available the scan must report the broken root. */
    char invalid_root[1024];
    snprintf(invalid_root, sizeof(invalid_root), "%s/not-a-directory", base);
    make_file(invalid_root, "not a directory");
    vfh_sources invalid_sources;
    assert(vfh_sources_parse(&invalid_sources, invalid_root, error, sizeof(error)));
    invalid_sources.items[0].available = 1;
    vfh_library invalid;
    vfh_library_init(&invalid);
    assert(!vfh_library_scan(&invalid, &invalid_sources, recordings, error, sizeof(error)));
    vfh_library_destroy(&invalid);

    /* The scanner must reject rather than silently truncate a malformed or
       unexpectedly huge card. Keep this fixture separate from the functional
       catalog so it cannot mask the normal path assertions above. */
    char overflow_root[1024];
    snprintf(overflow_root, sizeof(overflow_root), "%s/overflow", base);
    make_dir(overflow_root);
    for (int i = 0; i <= VFH_LIBRARY_MAX_RECORDS; i++) {
        snprintf(path, sizeof(path), "%s/Video-%04d.mp4", overflow_root, i);
        make_file(path, "x");
    }
    vfh_sources overflow_sources;
    assert(vfh_sources_parse(&overflow_sources, overflow_root, error, sizeof(error)));
    vfh_library overflow;
    vfh_library_init(&overflow);
    assert(!vfh_library_scan(&overflow, &overflow_sources, recordings, error, sizeof(error)));
    assert(overflow.count == 0);
    vfh_library_destroy(&overflow);
    for (int i = 0; i <= VFH_LIBRARY_MAX_RECORDS; i++) {
        snprintf(path, sizeof(path), "%s/Video-%04d.mp4", overflow_root, i);
        assert(unlink(path) == 0);
    }
    assert(rmdir(overflow_root) == 0);
    puts("vfh_library_test: ok");
    return 0;
}
