#include "vfh_art.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch_file(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, "x", 1) == 1);
    assert(close(fd) == 0);
}

int main(void) {
    char root[] = "/tmp/vfh-art-test-XXXXXX";
    assert(mkdtemp(root));
    assert(setenv("USERDATA_PATH", root, 1) == 0);

    char movie[1200], exact[1200], poster[1200], folder[1200], other[1200], cache[1200], changed_cache[1200], found[1200];
    snprintf(movie, sizeof(movie), "%s/movie.mp4", root);
    snprintf(exact, sizeof(exact), "%s/movie.jpg", root);
    snprintf(poster, sizeof(poster), "%s/poster.png", root);
    snprintf(folder, sizeof(folder), "%s/folder.jpg", root);
    snprintf(other, sizeof(other), "%s/other.mkv", root);
    touch_file(movie);
    touch_file(exact);
    touch_file(poster);

    assert(vfh_art_find_sidecar(movie, found, sizeof(found)));
    assert(strcmp(found, exact) == 0);
    assert(unlink(exact) == 0);
    assert(vfh_art_find_sidecar(movie, found, sizeof(found)));
    assert(strcmp(found, poster) == 0);
    assert(vfh_art_find_folder_art(root, found, sizeof(found)));
    assert(strcmp(found, poster) == 0);
    assert(unlink(poster) == 0);
    touch_file(folder);
    assert(vfh_art_find_folder_art(root, found, sizeof(found)));
    assert(strcmp(found, folder) == 0);
    touch_file(other);
    assert(!vfh_art_find_sidecar(movie, found, sizeof(found)));

    vfh_art_cache_path(movie, cache, sizeof(cache));
    assert(strstr(cache, "/VideoFromHell/thumbs-v2/") != NULL);
    assert(vfh_art_write_failure(cache, false));
    bool is_error = true;
    assert(vfh_art_failure_exists(cache, &is_error) && !is_error);
    int append_fd = open(movie, O_WRONLY | O_APPEND);
    assert(append_fd >= 0);
    assert(write(append_fd, "y", 1) == 1);
    assert(close(append_fd) == 0);
    vfh_art_cache_path(movie, changed_cache, sizeof(changed_cache));
    assert(strcmp(cache, changed_cache) != 0);
    assert(!vfh_art_failure_exists(changed_cache, NULL));
    vfh_art_clear_failures(cache);
    assert(!vfh_art_failure_exists(cache, NULL));
    assert(vfh_art_write_failure(cache, true));
    assert(vfh_art_failure_exists(cache, &is_error) && is_error);

    char none[1200], error[1200];
    vfh_art_failure_path(cache, false, none, sizeof(none));
    vfh_art_failure_path(cache, true, error, sizeof(error));
    vfh_art_clear_failures(cache);
    assert(unlink(movie) == 0);
    assert(unlink(folder) == 0);
    assert(unlink(other) == 0);
    char cache_dir[1200], app_dir[1200];
    snprintf(cache_dir, sizeof(cache_dir), "%s/VideoFromHell/thumbs-v2", root);
    snprintf(app_dir, sizeof(app_dir), "%s/VideoFromHell", root);
    assert(rmdir(cache_dir) == 0);
    assert(rmdir(app_dir) == 0);
    assert(rmdir(root) == 0);
    puts("vfh art tests passed");
    return 0;
}
