#include "vfh_resume.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char temp[] = "/tmp/videofromhell-resume-XXXXXX";
    char *base = mkdtemp(temp);
    assert(base);
    setenv("USERDATA_PATH", base, 1);

    const char *film = "/mnt/sdcard/Videos/film.mkv";

    /* Nothing stored yet. Keep the app directory non-world-writable even when
       the launcher inherited a completely permissive umask. */
    mode_t previous_umask = umask(0);
    assert(vfh_resume_get(film) == 0.0);
    char app_dir[1024];
    snprintf(app_dir, sizeof(app_dir), "%s/VideoFromHell", base);
    struct stat app_dir_stat;
    assert(stat(app_dir, &app_dir_stat) == 0);
    assert((app_dir_stat.st_mode & 0777) == 0755);
    umask(previous_umask);

    /* An all-under-cutoff legacy map must still be retired only after an empty
       playback-v2.json has committed, otherwise each launch retries migration. */
    char legacy_path[1024], playback_path[1024];
    snprintf(legacy_path, sizeof(legacy_path), "%s/VideoFromHell/resume.json", base);
    FILE *legacy = fopen(legacy_path, "wb");
    assert(legacy);
    fputs("{\"/mnt/sdcard/Videos/too-short.mkv\":30}", legacy);
    assert(fclose(legacy) == 0);
    assert(vfh_resume_get("/mnt/sdcard/Videos/too-short.mkv") == 0.0);
    snprintf(playback_path, sizeof(playback_path), "%s/VideoFromHell/playback-v2.json", base);
    assert(access(playback_path, F_OK) == 0);
    assert(access(legacy_path, F_OK) != 0);

    /* The first usable legacy position imports atomically, then retires the
       old file only after playback-v2.json exists. */
    legacy = fopen(legacy_path, "wb");
    assert(legacy);
    fputs("{\"/mnt/sdcard/Videos/legacy.mkv\":88}", legacy);
    assert(fclose(legacy) == 0);
    assert(vfh_resume_get("/mnt/sdcard/Videos/legacy.mkv") == 88.0);
    assert(access(playback_path, F_OK) == 0);
    assert(access(legacy_path, F_OK) != 0);

    /* New records use the stable catalog identity rather than an SD mount
       path, while an available legacy absolute path promotes itself on read. */
    vfh_resume_identity identity = {
        .content_kind = VFH_CONTENT_VIDEO,
        .source_index = 1,
        .relative_path = "Films/identity.mkv",
    };
    const char *identity_path = "/media/sdcard1/Videos/Films/identity.mkv";
    vfh_resume_set_identity(&identity, identity_path, 321.0, 1000.0);
    assert(vfh_resume_get_identity(&identity, identity_path) == 321.0);

    /* A position inside the film round-trips. */
    vfh_resume_set(film, 615.0, 3600.0);
    assert(vfh_resume_get(film) == 615.0);

    /* Barely-started films are not worth resuming: storing one clears it, so
       the user is not prompted over a few seconds of accidental playback. */
    vfh_resume_set(film, 5.0, 3600.0);
    assert(vfh_resume_get(film) == 0.0);

    /* At 90% the film is watched and must start clean next time.  Keep the
       exact boundary explicit: it is part of the player contract. */
    vfh_resume_set(film, 900.0, 3600.0);
    assert(vfh_resume_get(film) > 0.0);
    vfh_resume_set(film, 899.0, 1000.0);
    assert(vfh_resume_get(film) == 899.0);
    vfh_resume_set(film, 900.0, 1000.0);
    assert(vfh_resume_get(film) == 0.0);
    vfh_resume_set(film, 901.0, 1000.0);
    assert(vfh_resume_get(film) == 0.0);
    vfh_resume_set(film, 300.0, 1000.0);
    vfh_resume_mark_watched(film, 1000.0);
    assert(vfh_resume_get(film) == 0.0);

    /* Unknown duration (0) must still store: it only disables the end check. */
    vfh_resume_set(film, 500.0, 0.0);
    assert(vfh_resume_get(film) == 500.0);

    /* Entries are independent per path. */
    const char *other = "/media/sdcard1/Videos/other.mp4";
    vfh_resume_set(other, 120.0, 2000.0);
    assert(vfh_resume_get(film) == 500.0);
    assert(vfh_resume_get(other) == 120.0);

    /* Browser rendering keeps one immutable store snapshot. Repeated row
       queries must not reopen the JSON document or promote legacy entries;
       an explicit write becomes visible only to the next rebuild. */
    vfh_resume_snapshot snapshot;
    vfh_resume_snapshot_init(&snapshot);
    assert(vfh_resume_snapshot_load(&snapshot));
    assert(vfh_resume_snapshot_get(&snapshot, film) == 500.0);
    assert(vfh_resume_snapshot_get_identity(&snapshot, &identity, identity_path) == 321.0);
    vfh_resume_set(film, 650.0, 3600.0);
    assert(vfh_resume_snapshot_get(&snapshot, film) == 500.0);
    vfh_resume_snapshot_destroy(&snapshot);
    assert(vfh_resume_snapshot_load(&snapshot));
    assert(vfh_resume_snapshot_get(&snapshot, film) == 650.0);
    vfh_resume_snapshot_destroy(&snapshot);

    /* Continue Watching has an explicit removal path, distinct from watched. */
    assert(vfh_resume_remove_identity(&identity, identity_path));
    assert(vfh_resume_get_identity(&identity, identity_path) == 0.0);
    assert(vfh_resume_remove(film));
    assert(vfh_resume_get(film) == 0.0);

    /* A corrupt store must read as "no resume point", never crash or wedge. */
    FILE *fp = fopen(playback_path, "wb");
    assert(fp);
    fputs("{ this is not json", fp);
    fclose(fp);
    assert(vfh_resume_get(film) == 0.0);
    /* ...and writing over it recovers. */
    vfh_resume_set(film, 777.0, 3600.0);
    assert(vfh_resume_get(film) == 777.0);

    /* No USERDATA_PATH at all (direct launch, no Leaf env) is not an error. */
    unsetenv("USERDATA_PATH");
    unsetenv("SHARED_USERDATA_PATH");
    assert(vfh_resume_get(film) == 0.0);
    vfh_resume_set(film, 42.0, 3600.0);   /* must not crash */

    printf("vfh_resume_test: ok\n");
    return 0;
}
