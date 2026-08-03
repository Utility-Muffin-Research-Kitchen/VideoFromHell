#include "vfh_queue.h"
#include "vfh_resume.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static vfh_queue_item item(vfh_content_kind kind, int source, const char *relative_path) {
    vfh_queue_item result = {
        .content_kind = kind,
        .source_index = source,
    };
    snprintf(result.relative_path, sizeof(result.relative_path), "%s", relative_path);
    return result;
}

static void expect_path(const vfh_queue *queue, size_t index, const char *path) {
    assert(index < queue->count);
    assert(strcmp(queue->items[index].relative_path, path) == 0);
}

int main(void) {
    char temporary[] = "/tmp/videofromhell-queue-XXXXXX";
    char *userdata = mkdtemp(temporary);
    assert(userdata);
    assert(setenv("USERDATA_PATH", userdata, 1) == 0);
    unsetenv("SHARED_USERDATA_PATH");

    vfh_queue queue;
    vfh_queue_init(&queue);
    vfh_queue_item one = item(VFH_CONTENT_VIDEO, 0, "Films/One.mp4");
    vfh_queue_item two = item(VFH_CONTENT_VIDEO, 1, "Films/Two.mkv");
    vfh_queue_item game = item(VFH_CONTENT_RECORDING, 0, "Game-part2.mp4");
    vfh_queue_item unsafe = item(VFH_CONTENT_VIDEO, 0, "../outside.mp4");

    assert(!vfh_queue_append(&queue, &unsafe));
    assert(vfh_queue_append(&queue, &one));
    assert(vfh_queue_append(&queue, &two));
    assert(vfh_queue_play_next(&queue, &game, 1));
    assert(queue.count == 3);
    expect_path(&queue, 0, "Films/One.mp4");
    expect_path(&queue, 1, "Game-part2.mp4");
    expect_path(&queue, 2, "Films/Two.mkv");

    assert(vfh_queue_move(&queue, 2, -1));
    assert(!vfh_queue_move(&queue, 0, -1));
    expect_path(&queue, 0, "Films/One.mp4");
    expect_path(&queue, 1, "Films/Two.mkv");
    expect_path(&queue, 2, "Game-part2.mp4");
    assert(vfh_queue_remove(&queue, 1));
    assert(queue.count == 2);
    expect_path(&queue, 1, "Game-part2.mp4");

    assert(vfh_queue_save(&queue));
    vfh_queue restored;
    assert(vfh_queue_load(&restored));
    assert(restored.count == 2);
    expect_path(&restored, 0, "Films/One.mp4");
    expect_path(&restored, 1, "Game-part2.mp4");
    assert(restored.items[1].content_kind == VFH_CONTENT_RECORDING);

    /* Queue and resume state share one atomic document. Each writer must leave
       the other member intact, otherwise a periodic checkpoint would erase a
       carefully curated queue (or vice versa). */
    const char *film = "/mnt/sdcard/Videos/film.mkv";
    vfh_resume_set(film, 120.0, 1000.0);
    assert(vfh_resume_get(film) == 120.0);
    assert(vfh_queue_load(&restored));
    assert(restored.count == 2);

    vfh_queue_clear(&restored);
    assert(restored.count == 0);
    assert(vfh_queue_save(&restored));
    assert(vfh_queue_load(&restored));
    assert(restored.count == 0);
    assert(vfh_resume_get(film) == 120.0);

    char path[1200];
    snprintf(path, sizeof(path), "%s/VideoFromHell/playback-v2.json", userdata);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fputs("{corrupt", file) >= 0);
    assert(fclose(file) == 0);
    assert(!vfh_queue_load(&restored));
    assert(restored.count == 0);

    unsetenv("USERDATA_PATH");
    assert(!vfh_queue_save(&queue));
    assert(!vfh_queue_load(&restored));
    puts("vfh_queue_test: ok");
    return 0;
}
