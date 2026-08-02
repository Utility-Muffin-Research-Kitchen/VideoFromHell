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

    /* A position inside the film round-trips. */
    vfh_resume_set(film, 615.0, 3600.0);
    assert(vfh_resume_get(film) == 615.0);

    /* Barely-started films are not worth resuming: storing one clears it, so
       the user is not prompted over a few seconds of accidental playback. */
    vfh_resume_set(film, 5.0, 3600.0);
    assert(vfh_resume_get(film) == 0.0);

    /* Reaching the end clears the entry -- a finished film must start clean
       next time rather than resuming three seconds from the credits. */
    vfh_resume_set(film, 900.0, 3600.0);
    assert(vfh_resume_get(film) > 0.0);
    vfh_resume_set(film, 3595.0, 3600.0);
    assert(vfh_resume_get(film) == 0.0);

    /* Unknown duration (0) must still store: it only disables the end check. */
    vfh_resume_set(film, 500.0, 0.0);
    assert(vfh_resume_get(film) == 500.0);

    /* Entries are independent per path. */
    const char *other = "/media/sdcard1/Videos/other.mp4";
    vfh_resume_set(other, 120.0, 2000.0);
    assert(vfh_resume_get(film) == 500.0);
    assert(vfh_resume_get(other) == 120.0);

    /* A corrupt store must read as "no resume point", never crash or wedge. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/VideoFromHell/resume.json", base);
    FILE *fp = fopen(path, "wb");
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
