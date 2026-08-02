#include "vfh_resume.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Don't offer to resume a film you had barely started, and treat "nearly the
   end" as finished so the next launch starts clean. */
#define VFH_RESUME_MIN_SECONDS   30.0
#define VFH_RESUME_END_MARGIN    0.05   /* fraction of duration */
/* Bound the store so a big library can't grow it without limit. Oldest-written
   entries are dropped first (insertion order is preserved by cJSON). */
#define VFH_RESUME_MAX_ENTRIES   200

static bool vfh_resume_dir(char *out, size_t n) {
    const char *base = getenv("USERDATA_PATH");
    if (!base || !base[0]) base = getenv("SHARED_USERDATA_PATH");
    if (!base || !base[0]) return false;
    if ((size_t)snprintf(out, n, "%s/VideoFromHell", base) >= n) return false;
    mkdir(out, 0755);   /* best-effort; may already exist */
    return true;
}

static bool vfh_resume_file(char *out, size_t n) {
    char dir[768];
    if (!vfh_resume_dir(dir, sizeof(dir))) return false;
    return (size_t)snprintf(out, n, "%s/resume.json", dir) < n;
}

static cJSON *vfh_resume_load(void) {
    char path[1024];
    if (!vfh_resume_file(path, sizeof(path))) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 1 << 20) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root && !cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

static void vfh_resume_store(cJSON *root) {
    char path[1024], tmp[1088];
    if (!vfh_resume_file(path, sizeof(path))) return;
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) return;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return;
    /* Write-then-rename: a yanked SD card mid-write must not leave a truncated
       store that fails to parse on the next launch. */
    FILE *fp = fopen(tmp, "wb");
    if (fp) {
        bool ok = fwrite(text, 1, strlen(text), fp) == strlen(text);
        ok = (fclose(fp) == 0) && ok;
        if (ok) rename(tmp, path);
        else unlink(tmp);
    }
    free(text);
}

double vfh_resume_get(const char *path) {
    if (!path || !path[0]) return 0.0;
    cJSON *root = vfh_resume_load();
    if (!root) return 0.0;
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, path);
    double seconds = cJSON_IsNumber(entry) ? entry->valuedouble : 0.0;
    cJSON_Delete(root);
    return seconds > VFH_RESUME_MIN_SECONDS ? seconds : 0.0;
}

void vfh_resume_set(const char *path, double position, double duration) {
    if (!path || !path[0]) return;
    cJSON *root = vfh_resume_load();
    if (!root) root = cJSON_CreateObject();
    if (!root) return;

    bool finished = duration > 0.0 &&
                    position >= duration * (1.0 - VFH_RESUME_END_MARGIN);
    if (position <= VFH_RESUME_MIN_SECONDS || finished) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, path);
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(root, path);   /* re-insert last */
        cJSON_AddNumberToObject(root, path, position);
        while (cJSON_GetArraySize(root) > VFH_RESUME_MAX_ENTRIES) {
            cJSON *oldest = cJSON_GetArrayItem(root, 0);
            if (!oldest) break;
            cJSON_DeleteItemFromObjectCaseSensitive(root, oldest->string);
        }
    }
    vfh_resume_store(root);
    cJSON_Delete(root);
}
