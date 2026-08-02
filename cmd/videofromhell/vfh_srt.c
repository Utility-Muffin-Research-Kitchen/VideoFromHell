#include "vfh_srt.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define VFH_SRT_MAX_CUES 8192
#define VFH_SRT_MAX_BYTES (4u * 1024u * 1024u)

typedef struct {
    double start;
    double end;
    char  *text;
} vfh_srt_cue;

struct vfh_srt {
    vfh_srt_cue *cues;
    int count;
    char source[2048];
};

/* ── parsing ─────────────────────────────────────────────────────────────── */

/* "HH:MM:SS,mmm" (or '.' for the decimal separator, which some muxers emit).
   Returns the seconds, or -1.0 when the field is not a timecode. */
static double vfh_srt_timecode(const char *s, const char **end) {
    int h = 0, m = 0, sec = 0, ms = 0;
    char sep = 0;
    int consumed = 0;
    if (sscanf(s, "%d:%d:%d%c%d%n", &h, &m, &sec, &sep, &ms, &consumed) != 5 ||
        (sep != ',' && sep != '.'))
        return -1.0;
    if (end) *end = s + consumed;
    return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
}

static const char *vfh_srt_skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Parse "start --> end" from one line. */
static bool vfh_srt_parse_timing(const char *line, double *start, double *end) {
    const char *cursor = NULL;
    double a = vfh_srt_timecode(vfh_srt_skip_spaces(line), &cursor);
    if (a < 0.0) return false;
    cursor = vfh_srt_skip_spaces(cursor);
    if (strncmp(cursor, "-->", 3) != 0) return false;
    cursor = vfh_srt_skip_spaces(cursor + 3);
    double b = vfh_srt_timecode(cursor, NULL);
    if (b < 0.0) return false;
    *start = a;
    *end = b;
    return true;
}

static int vfh_srt_compare(const void *a, const void *b) {
    double x = ((const vfh_srt_cue *)a)->start;
    double y = ((const vfh_srt_cue *)b)->start;
    return x < y ? -1 : (x > y ? 1 : 0);
}

vfh_srt *vfh_srt_parse(const char *text) {
    if (!text) return NULL;
    /* Skip a UTF-8 BOM; leaving it makes the first index line unparseable. */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text += 3;

    vfh_srt *srt = calloc(1, sizeof(*srt));
    if (!srt) return NULL;
    srt->cues = calloc(VFH_SRT_MAX_CUES, sizeof(*srt->cues));
    if (!srt->cues) { free(srt); return NULL; }

    char *work = strdup(text);
    if (!work) { free(srt->cues); free(srt); return NULL; }

    /* Walk the buffer line by line, trimming CR so CRLF files parse the same. */
    char *cursor = work;

    double start = 0.0, end = 0.0;
    bool have_timing = false;
    char body[2048];
    size_t body_len = 0;

    while (cursor) {
        char *newline = strchr(cursor, '\n');
        if (newline) *newline = '\0';
        char *line = cursor;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                       line[len - 1] == '\t'))
            line[--len] = '\0';

        if (len == 0) {
            /* Blank line closes the current cue. */
            if (have_timing && body_len && srt->count < VFH_SRT_MAX_CUES) {
                srt->cues[srt->count].start = start;
                srt->cues[srt->count].end = end;
                srt->cues[srt->count].text = strdup(body);
                if (srt->cues[srt->count].text) srt->count++;
            }
            have_timing = false;
            body_len = 0;
            body[0] = '\0';
        } else if (!have_timing) {
            /* Either the index line (ignored) or the timing line. Index lines
               are optional in the wild, so drive off the timing match rather
               than assuming a fixed three-line block shape. */
            if (vfh_srt_parse_timing(line, &start, &end)) {
                have_timing = true;
                body_len = 0;
                body[0] = '\0';
            }
        } else {
            size_t need = len + (body_len ? 1 : 0);
            if (body_len + need < sizeof(body)) {
                if (body_len) body[body_len++] = '\n';
                memcpy(body + body_len, line, len);
                body_len += len;
                body[body_len] = '\0';
            }
        }

        cursor = newline ? newline + 1 : NULL;
    }
    /* A file that ends without a trailing blank line still has a final cue. */
    if (have_timing && body_len && srt->count < VFH_SRT_MAX_CUES) {
        srt->cues[srt->count].start = start;
        srt->cues[srt->count].end = end;
        srt->cues[srt->count].text = strdup(body);
        if (srt->cues[srt->count].text) srt->count++;
    }
    free(work);

    if (srt->count == 0) { vfh_srt_free(srt); return NULL; }
    /* Some files are not in chronological order; the lookup binary-searches. */
    qsort(srt->cues, (size_t)srt->count, sizeof(*srt->cues), vfh_srt_compare);
    return srt;
}

/* ── sidecar discovery ───────────────────────────────────────────────────── */

static bool vfh_srt_ends_with_srt(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".srt") == 0;
}

/* Accept "<stem>.srt" and "<stem>.<anything>.srt" (language tags), so
   "film.mkv" matches film.srt, film.en.srt, film.English.srt. */
static bool vfh_srt_matches_stem(const char *name, const char *stem, size_t stem_len) {
    if (!vfh_srt_ends_with_srt(name)) return false;
    if (strncasecmp(name, stem, stem_len) != 0) return false;
    return name[stem_len] == '.';
}

vfh_srt *vfh_srt_load_for(const char *video_path) {
    if (!video_path || !video_path[0]) return NULL;
    const char *slash = strrchr(video_path, '/');
    const char *base = slash ? slash + 1 : video_path;
    char dir[1024];
    if (slash) {
        size_t dir_len = (size_t)(slash - video_path);
        if (dir_len >= sizeof(dir)) return NULL;
        memcpy(dir, video_path, dir_len);
        dir[dir_len] = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    const char *dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    if (!stem_len) return NULL;

    DIR *handle = opendir(dir);
    if (!handle) return NULL;
    char best[1024] = "";
    struct dirent *item;
    while ((item = readdir(handle)) != NULL) {
        if (!vfh_srt_matches_stem(item->d_name, base, stem_len)) continue;
        /* Prefer the plain "<stem>.srt" over a language-tagged one; otherwise
           take the first match so the choice is at least deterministic. */
        bool plain = strlen(item->d_name) == stem_len + 4;
        if (!best[0] || plain) {
            snprintf(best, sizeof(best), "%s", item->d_name);
            if (plain) break;
        }
    }
    closedir(handle);
    if (!best[0]) return NULL;

    char path[2048];
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, best) >= sizeof(path))
        return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || (unsigned long)size > VFH_SRT_MAX_BYTES) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';

    vfh_srt *srt = vfh_srt_parse(buf);
    free(buf);
    if (srt) snprintf(srt->source, sizeof(srt->source), "%s", path);
    return srt;
}

/* ── lookup ──────────────────────────────────────────────────────────────── */

const char *vfh_srt_text_at(const vfh_srt *srt, double seconds) {
    if (!srt || srt->count <= 0) return NULL;
    /* Last cue whose start is <= seconds. */
    int low = 0, high = srt->count - 1, found = -1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (srt->cues[mid].start <= seconds) { found = mid; low = mid + 1; }
        else high = mid - 1;
    }
    if (found < 0) return NULL;
    /* Overlapping cues are legal; walk back a little so an earlier, still-open
       cue is not missed when a later one has already started. */
    for (int i = found; i >= 0 && i > found - 8; i--)
        if (srt->cues[i].end > seconds && srt->cues[i].start <= seconds)
            return srt->cues[i].text;
    return NULL;
}

int vfh_srt_count(const vfh_srt *srt) { return srt ? srt->count : 0; }

const char *vfh_srt_source(const vfh_srt *srt) { return srt ? srt->source : ""; }

void vfh_srt_free(vfh_srt *srt) {
    if (!srt) return;
    for (int i = 0; i < srt->count; i++) free(srt->cues[i].text);
    free(srt->cues);
    free(srt);
}
