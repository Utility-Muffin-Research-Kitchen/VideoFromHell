#include "vfh_sources.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char temp[] = "/tmp/videofromhell-sources-XXXXXX";
    char *base = mkdtemp(temp);
    assert(base);
    char primary[1024], missing[1024], list[2050], error[256];
    snprintf(primary, sizeof(primary), "%s/one", base);
    snprintf(missing, sizeof(missing), "%s/two", base);
    assert(mkdir(primary, 0700) == 0);
    snprintf(list, sizeof(list), "%s/:%s/", primary, missing);

    vfh_sources sources;
    assert(vfh_sources_parse(&sources, list, error, sizeof(error)));
    assert(sources.count == 2);
    assert(strcmp(sources.items[0].id, "primary") == 0);
    assert(strcmp(sources.items[1].id, "secondary_sd") == 0);
    assert(sources.items[0].available == 1);
    assert(sources.items[1].available == 0);
    /* A mounted card that simply has no Videos/ must not be reported as an
       unmounted card — different problem, different fix for the user. */
    assert(sources.items[0].state == VFH_SOURCE_OK);
    assert(sources.items[1].state == VFH_SOURCE_NO_FOLDER);
    assert(vfh_source_state_message(&sources.items[0]) == NULL);
    assert(strstr(vfh_source_state_message(&sources.items[1]), "Videos folder") != NULL);

    snprintf(list, sizeof(list), "%s:%s/", primary, primary);
    assert(!vfh_sources_parse(&sources, list, error, sizeof(error)));
    assert(strstr(error, "duplicate") != NULL);
    assert(!vfh_sources_parse(&sources, ":/tmp/videos", error, sizeof(error)));

    snprintf(list, sizeof(list), "%s:%s", primary, missing);
    setenv("VIDEO_PATHS", list, 1);
    setenv("SDCARD_PATHS", "/card1:/card2", 1);
    unsetenv("VIDEO_PATH");
    assert(vfh_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.count == 2);
    /* Both Videos/ roots exist; the card is what decides now. */
    assert(mkdir(missing, 0700) == 0);
    setenv("VFH_SOURCE_TEST_AVAILABLE", "1", 1);
    assert(vfh_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.items[1].available == 1);
    assert(sources.items[1].state == VFH_SOURCE_OK);
    /* Card gone: that, not the folder, is what the user must be told. */
    setenv("VFH_SOURCE_TEST_AVAILABLE", "0", 1);
    assert(vfh_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.items[1].available == 0);
    assert(sources.items[1].state == VFH_SOURCE_NO_CARD);
    assert(strstr(vfh_source_state_message(&sources.items[1]), "not mounted") != NULL);
    /* The configured root and stable source index survive a card becoming
       available after launch; an explicit library rescan must see it. */
    setenv("VFH_SOURCE_TEST_AVAILABLE", "1", 1);
    vfh_sources_refresh(&sources);
    assert(sources.items[1].available == 1);
    assert(sources.items[1].state == VFH_SOURCE_OK);
    unsetenv("VFH_SOURCE_TEST_AVAILABLE");

    char recordings[1024];
    setenv("RECORDINGS_PATH", "/fixture/recordings-contract", 1);
    assert(vfh_recordings_path_resolve(recordings, sizeof(recordings)));
    assert(strcmp(recordings, "/fixture/recordings-contract") == 0);
    unsetenv("RECORDINGS_PATH");
    setenv("SDCARD_PATH", primary, 1);
    assert(vfh_recordings_path_resolve(recordings, sizeof(recordings)));
    snprintf(list, sizeof(list), "%s/Recordings", primary);
    assert(strcmp(recordings, list) == 0);
    unsetenv("SDCARD_PATH");
    assert(vfh_recordings_path_resolve(recordings, sizeof(recordings)));
    assert(strcmp(recordings, "./Recordings") == 0);

    setenv("SDCARD_PATHS", "/card1:/card2:/card3", 1);
    assert(!vfh_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.count == 0);
    assert(strstr(error, "item count") != NULL);

    setenv("SDCARD_PATHS", "/card1::/card3", 1);
    assert(!vfh_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.count == 0);
    assert(strstr(error, "empty item") != NULL);

    setenv("SDCARD_PATHS", "/card1:/card2", 1);
    setenv("VIDEO_PATH", "/not-the-primary-root", 1);
    assert(!vfh_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.count == 0);
    assert(strstr(error, "primary") != NULL);
    unsetenv("VIDEO_PATH");
    unsetenv("VIDEO_PATHS");

    setenv("SDCARD_PATH", primary, 1);
    setenv("SDCARD_PATHS", "/card1:/card2", 1);
    assert(vfh_sources_resolve(&sources, error, sizeof(error)));
    assert(sources.count == 1);
    unsetenv("SDCARD_PATH");
    unsetenv("SDCARD_PATHS");

    setenv("VIDEO_PATH", primary, 1);
    vfh_sources_single_fallback(&sources);
    assert(sources.count == 1);
    assert(sources.items[0].available == 1);
    unsetenv("VIDEO_PATH");

    assert(rmdir(missing) == 0);
    assert(rmdir(primary) == 0);
    assert(rmdir(base) == 0);
    puts("vfh_sources_test: ok");
    return 0;
}
