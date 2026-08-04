/*
 * Video From Hell — merged, source-aware video library and player.
 *
 * Browser state is rendered only on the main thread. Catalog, artwork and
 * playback workers own their private data; only completed work is published.
 * Decoded video-frame uploads remain on the main thread.
 */
#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <pthread.h>

#include <libavutil/pixfmt.h>

#include "vfh_inhibit.h"
#include "vfh_art.h"
#include "vfh_library.h"
#include "vfh_media.h"
#include "vfh_osd.h"
#include "vfh_player.h"
#include "vfh_queue.h"
#include "vfh_resume.h"
#include "vfh_sources.h"
#include "vfh_srt.h"
#include "vfh_status.h"
#include "vfh_thumb.h"

#define VFH_NAME_MAX 256

typedef enum {
    VFH_ENTRY_PARENT,
    VFH_ENTRY_DIRECTORY,
    VFH_ENTRY_VIDEO,
} vfh_entry_kind;

typedef enum {
    VFH_TAB_CONTINUE = 0,
    VFH_TAB_RECENT,
    VFH_TAB_FOLDERS,
    VFH_TAB_COUNT,
} vfh_library_tab;

static const char *const VFH_TAB_NAMES[VFH_TAB_COUNT] = {
    "Continue Watching", "Recently Added", "Folders",
};

#define VFH_FOLDER_NAV_MAX 32

typedef struct {
    char relative[VFH_LIBRARY_RELATIVE_PATH_MAX];
    bool recordings;
    cat_list_state list;
} vfh_folder_nav;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool running;
    bool complete;
    bool succeeded;
    bool cancelled;
    atomic_bool cancel;
    vfh_sources sources;
    char recordings_path[VFH_SOURCE_PATH_MAX];
    char error[256];
    vfh_library result;       /* worker-owned until the main thread takes it */
} vfh_rescan_worker;

typedef struct {
    vfh_entry_kind kind;
    int source_index;
    bool available;
    bool has_duration;
    double duration;
    double resume_seconds;
    int64_t first_seen;
    int64_t capture_timestamp;
    int recording_part;
    int catalog_index;
    vfh_content_kind content_kind;
    bool recordings_folder;
    char name[VFH_NAME_MAX];
    char path[VFH_SOURCE_PATH_MAX];
} vfh_entry;

typedef struct {
    vfh_sources sources;
    vfh_library catalog;
    vfh_queue queue;
    vfh_status_monitor audio_status;
    char audio_output[VFH_AUDIO_OUTPUT_MAX];
    vfh_entry *entries;
    int entry_count;
    size_t entry_capacity;
    vfh_library_tab tab;
    cat_list_state tab_lists[VFH_TAB_COUNT];
    char folder_relative[VFH_LIBRARY_RELATIVE_PATH_MAX];
    bool folder_recordings;
    vfh_folder_nav folder_nav[VFH_FOLDER_NAV_MAX];
    int folder_nav_count;
    vfh_rescan_worker rescan;
    bool rescan_needs_resume;
    char recordings_path[VFH_SOURCE_PATH_MAX];
    char message[256];
    cat_list_state list;
    vfh_thumb_worker thumbs;
    bool thumb_worker_ready;
    SDL_Texture *poster;
    char poster_path[VFH_ART_PATH_MAX];
    char poster_requested_path[VFH_ART_PATH_MAX];
    vfh_thumb_state poster_state;
    bool poster_retry_pending;
    vfh_player *player;
    SDL_Texture *video_texture;
    int video_width;
    int video_height;
    Uint32 video_format;
    double pixel_aspect;          /* sample aspect ratio, 1.0 when square */
    char playing_path[VFH_SOURCE_PATH_MAX];
    char playing_name[VFH_NAME_MAX];
    vfh_queue_item playing_identity;
    bool playing_identity_valid;
    int aspect_mode;
    vfh_osd osd;
    vfh_srt *subtitles;
    bool subtitles_on;
    /* One rendered cue, reused until the cue or its layout changes. */
    SDL_Texture *subtitle_texture;
    char subtitle_text[512];
    TTF_Font *subtitle_font;
    int subtitle_max_w;
    int subtitle_w;
    int subtitle_h;
    int subtitle_inset;
    /* Value on entry to an OSD submenu, so B can cancel a live preview. */
    int submenu_entry_aspect;
    bool submenu_entry_subtitles_on;
    bool seek_back_held;
    bool seek_forward_held;
    Uint32 seek_last_ms;
    Uint32 resume_saved_ms;
    bool playback_finished;
} vfh_browser;

/* Aspect handling for playback. */
enum { VFH_ASPECT_FIT = 0, VFH_ASPECT_FILL, VFH_ASPECT_STRETCH, VFH_ASPECT_COUNT };

/* Hold-to-seek step and cadence for L2/R2. */
#define VFH_SEEK_HOLD_STEP_S 5.0
#define VFH_SEEK_HOLD_EVERY_MS 220
/* Periodic resume checkpoint, so a crash or a flat battery still resumes. */
#define VFH_RESUME_SAVE_EVERY_MS 30000

static void vfh_set_message(vfh_browser *browser, const char *message);
static void vfh_release_subtitle_texture(vfh_browser *browser);
static void vfh_open_osd_submenu(vfh_browser *browser, vfh_osd_submenu submenu);
static bool vfh_osd_submenu_previews(vfh_osd_submenu submenu);

static bool vfh_playback_osd_visible(vfh_browser *browser) {
    vfh_osd_tick(&browser->osd, SDL_GetTicks());
    return vfh_osd_visible(&browser->osd);
}

static void vfh_playback_osd_flash(vfh_browser *browser) {
    vfh_osd_flash(&browser->osd, SDL_GetTicks());
}

static const char *vfh_audio_output_label(const char *output) {
    if (output && strcasecmp(output, "BLUETOOTH") == 0) return "Bluetooth";
    if (output && strcasecmp(output, "HEADSET") == 0) return "Headphones";
    if (output && strcasecmp(output, "HDMI") == 0) return "HDMI";
    if (output && strcasecmp(output, "SPEAKER") == 0) return "Speaker";
    return "System";
}

static const char *vfh_browser_audio_output(const vfh_browser *browser) {
    if (browser && browser->audio_output[0]) return browser->audio_output;
    const char *output = getenv("JAWAKA_AUDIO_OUTPUT");
    if (!output || !output[0]) output = getenv("UMRK_AUDIO_OUTPUT");
    return output;
}

static void vfh_set_audio_output(vfh_browser *browser, const char *output) {
    if (!browser || !output || !output[0]) return;
    bool changed = strcasecmp(browser->audio_output, output) != 0;
    snprintf(browser->audio_output, sizeof(browser->audio_output), "%s", output);
    if (changed && browser->player) vfh_player_set_audio_output(browser->player, output);
}

static void vfh_refresh_audio_status(vfh_browser *browser) {
    char output[VFH_AUDIO_OUTPUT_MAX];
    if (vfh_status_monitor_output(&browser->audio_status, output, sizeof(output)))
        vfh_set_audio_output(browser, output);
    if (browser->player) {
        char notice[128];
        if (vfh_player_take_audio_notice(browser->player, notice, sizeof(notice))) {
            vfh_set_message(browser, notice);
            vfh_playback_osd_flash(browser);
        }
    }
}

static const char *vfh_entry_label(int index, void *opaque) {
    vfh_browser *browser = opaque;
    if (!browser || index < 0 || index >= browser->entry_count) return "";
    return browser->entries[index].name;
}

static const char *vfh_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (path ? path : "");
}

/* The filename without its extension, for telling apart rows whose resolved
 * titles are identical. Truncated from the left so the tail - where release
 * names put the resolution and codec - survives. */
static void vfh_filename_stem(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    const char *name = vfh_basename(path);
    if (!name[0]) return;
    size_t length = strlen(name);
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) length = (size_t)(dot - name);
    if (length >= out_size) {
        size_t kept = out_size - 2;
        snprintf(out, out_size, "…%s", name + length - (kept > 0 ? kept - 1 : 0));
        return;
    }
    memcpy(out, name, length);
    out[length] = '\0';
}

static int vfh_entry_rank(vfh_entry_kind kind) {
    switch (kind) {
        case VFH_ENTRY_PARENT: return 0;
        case VFH_ENTRY_DIRECTORY: return 1;
        case VFH_ENTRY_VIDEO: return 2;
    }
    return 3;
}

static int vfh_entry_compare(const void *a, const void *b) {
    const vfh_entry *left = a;
    const vfh_entry *right = b;
    int rank = vfh_entry_rank(left->kind) - vfh_entry_rank(right->kind);
    if (rank) return rank;
    return strcasecmp(left->name, right->name);
}

static double vfh_resume_for_library_item(const vfh_resume_snapshot *snapshot,
                                          const vfh_library_item *item, const char *path) {
    if (!item) return vfh_resume_snapshot_get(snapshot, path);
    vfh_resume_identity identity = {
        .content_kind = item->content_kind,
        .source_index = item->source_index,
        .relative_path = item->relative_path,
    };
    return vfh_resume_snapshot_get_identity(snapshot, &identity, path);
}

static void vfh_checkpoint_playing(vfh_browser *browser, double position, double duration) {
    if (!browser || !browser->playing_path[0]) return;
    if (browser->playing_identity_valid) {
        vfh_resume_identity identity = {
            .content_kind = browser->playing_identity.content_kind,
            .source_index = browser->playing_identity.source_index,
            .relative_path = browser->playing_identity.relative_path,
        };
        vfh_resume_set_identity(&identity, browser->playing_path, position, duration);
    } else {
        vfh_resume_set(browser->playing_path, position, duration);
    }
}

static void vfh_mark_playing_watched(vfh_browser *browser, double duration) {
    if (!browser || !browser->playing_path[0]) return;
    if (browser->playing_identity_valid) {
        vfh_resume_identity identity = {
            .content_kind = browser->playing_identity.content_kind,
            .source_index = browser->playing_identity.source_index,
            .relative_path = browser->playing_identity.relative_path,
        };
        vfh_resume_mark_identity_watched(&identity, browser->playing_path, duration);
    } else {
        vfh_resume_mark_watched(browser->playing_path, duration);
    }
}

static bool vfh_entry_identity(const vfh_browser *browser, const vfh_entry *entry,
                               vfh_queue_item *identity) {
    if (!browser || !entry || !identity || entry->catalog_index < 0 ||
        (size_t)entry->catalog_index >= browser->catalog.count) return false;
    const vfh_library_item *item = &browser->catalog.items[entry->catalog_index];
    memset(identity, 0, sizeof(*identity));
    identity->content_kind = item->content_kind;
    identity->source_index = item->source_index;
    int length = snprintf(identity->relative_path, sizeof(identity->relative_path), "%s",
                          item->relative_path);
    return length >= 0 && length < (int)sizeof(identity->relative_path);
}

static void vfh_clear_poster(vfh_browser *browser) {
    if (browser->poster) SDL_DestroyTexture(browser->poster);
    browser->poster = NULL;
    browser->poster_path[0] = '\0';
    browser->poster_requested_path[0] = '\0';
    browser->poster_state = VFH_THUMB_IDLE;
}

static void vfh_stop_playback(vfh_browser *browser) {
    /* Released here rather than at each call site: every path out of playback
       (back, finished, quit, starting another file) funnels through this. */
    vfh_inhibit_release();
    /* Checkpoint before the player goes away -- position comes from it. */
    if (browser->player && browser->playing_path[0] && !browser->playback_finished)
        vfh_checkpoint_playing(browser, vfh_player_position(browser->player),
                               vfh_player_duration(browser->player));
    vfh_srt_free(browser->subtitles);
    browser->subtitles = NULL;
    vfh_release_subtitle_texture(browser);
    if (browser->video_texture) SDL_DestroyTexture(browser->video_texture);
    browser->video_texture = NULL;
    browser->video_width = browser->video_height = 0;
    browser->video_format = SDL_PIXELFORMAT_UNKNOWN;
    if (browser->player) vfh_player_destroy(browser->player);
    browser->player = NULL;
    browser->playing_path[0] = '\0';
    browser->playing_name[0] = '\0';
    browser->playing_identity_valid = false;
    browser->playback_finished = false;
}

static void vfh_set_message(vfh_browser *browser, const char *message) {
    snprintf(browser->message, sizeof(browser->message), "%s", message ? message : "");
    if (message && message[0]) cat_log("videofromhell: %s", message);
}

static bool vfh_entries_reserve(vfh_browser *browser, size_t wanted) {
    if (!browser || wanted > VFH_LIBRARY_MAX_RECORDS) return false;
    if (wanted <= browser->entry_capacity) return true;
    size_t capacity = browser->entry_capacity ? browser->entry_capacity * 2u : 32u;
    if (capacity < wanted) capacity = wanted;
    if (capacity > VFH_LIBRARY_MAX_RECORDS) capacity = VFH_LIBRARY_MAX_RECORDS;
    vfh_entry *entries = realloc(browser->entries, capacity * sizeof(*entries));
    if (!entries) return false;
    browser->entries = entries;
    browser->entry_capacity = capacity;
    return true;
}

static bool vfh_add_entry(vfh_browser *browser, vfh_entry_kind kind,
                          const char *name, const char *path, int source_index) {
    if (!browser || !vfh_entries_reserve(browser, (size_t)browser->entry_count + 1u))
        return false;
    vfh_entry *entry = &browser->entries[browser->entry_count++];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    entry->source_index = source_index;
    entry->catalog_index = -1;
    snprintf(entry->name, sizeof(entry->name), "%s", name ? name : "");
    snprintf(entry->path, sizeof(entry->path), "%s", path ? path : "");
    return true;
}

static void vfh_add_catalog_video(vfh_browser *browser,
                                  const vfh_resume_snapshot *resume_snapshot,
                                  size_t catalog_index) {
    if (!browser || catalog_index >= browser->catalog.count) return;
    const vfh_library_item *item = &browser->catalog.items[catalog_index];
    char path[VFH_SOURCE_PATH_MAX];
    if (!vfh_library_resolve_path(item, &browser->sources, browser->recordings_path,
                                  path, sizeof(path))) return;
    if (!vfh_add_entry(browser, VFH_ENTRY_VIDEO, item->display_title, path, item->source_index))
        return;
    vfh_entry *entry = &browser->entries[browser->entry_count - 1];
    entry->available = item->available;
    entry->has_duration = item->duration > 0.0;
    entry->duration = item->duration;
    entry->first_seen = item->first_seen;
    entry->capture_timestamp = item->capture_timestamp;
    entry->recording_part = item->recording_part;
    entry->resume_seconds = vfh_resume_for_library_item(resume_snapshot, item, path);
    entry->catalog_index = (int)catalog_index;
    entry->content_kind = item->content_kind;
}

static bool vfh_folder_starts_with(const char *relative, const char *folder,
                                   const char **rest) {
    if (!relative || !folder) return false;
    if (!folder[0]) {
        if (rest) *rest = relative;
        return true;
    }
    size_t length = strlen(folder);
    if (strncmp(relative, folder, length) != 0 || relative[length] != '/') return false;
    if (rest) *rest = relative + length + 1;
    return true;
}

static bool vfh_folder_already_listed(const vfh_browser *browser, const char *relative,
                                      bool recordings) {
    for (int i = 0; browser && i < browser->entry_count; i++) {
        const vfh_entry *entry = &browser->entries[i];
        if (entry->kind == VFH_ENTRY_DIRECTORY && entry->recordings_folder == recordings &&
            strcmp(entry->path, relative) == 0) return true;
    }
    return false;
}

static void vfh_add_folder(vfh_browser *browser, const char *name,
                           const char *relative, bool recordings) {
    if (!browser || !name || !relative || vfh_folder_already_listed(browser, relative, recordings))
        return;
    if (!vfh_add_entry(browser, VFH_ENTRY_DIRECTORY, name, relative, 0)) return;
    browser->entries[browser->entry_count - 1].recordings_folder = recordings;
}

static bool vfh_recordings_folder_visible(const vfh_browser *browser,
                                          const vfh_resume_snapshot *resume_snapshot) {
    struct stat st;
    if (browser && browser->recordings_path[0] &&
        stat(browser->recordings_path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    for (size_t i = 0; browser && i < browser->catalog.count; i++) {
        const vfh_library_item *item = &browser->catalog.items[i];
        if (item->content_kind != VFH_CONTENT_RECORDING) continue;
        char path[VFH_SOURCE_PATH_MAX];
        if (vfh_library_resolve_path(item, &browser->sources, browser->recordings_path,
                                     path, sizeof(path)) &&
            vfh_resume_for_library_item(resume_snapshot, item, path) > 0.0)
            return true;
    }
    return false;
}

static int vfh_recent_compare(const void *a, const void *b) {
    const vfh_entry *left = a;
    const vfh_entry *right = b;
    if (left->first_seen != right->first_seen)
        return left->first_seen > right->first_seen ? -1 : 1;
    return vfh_entry_compare(a, b);
}

static void vfh_restore_current_list(vfh_browser *browser) {
    if (!browser) return;
    cat_list_state *saved = &browser->tab_lists[browser->tab];
    if (browser->tab == VFH_TAB_FOLDERS) {
        saved = NULL;
        for (int i = 0; i < browser->folder_nav_count; i++) {
            vfh_folder_nav *nav = &browser->folder_nav[i];
            if (nav->recordings == browser->folder_recordings &&
                strcmp(nav->relative, browser->folder_relative) == 0) {
                saved = &nav->list;
                break;
            }
        }
    }
    if (saved && saved->visible_rows > 0) browser->list = *saved;
    else cat_list_state_init(&browser->list, 1);
    if (browser->entry_count <= 0) {
        browser->list.cursor = browser->list.scroll_offset = 0;
    } else {
        if (browser->list.cursor < 0 || browser->list.cursor >= browser->entry_count)
            browser->list.cursor = 0;
        if (browser->list.scroll_offset < 0 || browser->list.scroll_offset > browser->list.cursor)
            browser->list.scroll_offset = 0;
    }
}

static void vfh_store_current_list(vfh_browser *browser) {
    if (!browser) return;
    if (browser->tab != VFH_TAB_FOLDERS) {
        browser->tab_lists[browser->tab] = browser->list;
        return;
    }
    for (int i = 0; i < browser->folder_nav_count; i++) {
        vfh_folder_nav *nav = &browser->folder_nav[i];
        if (nav->recordings == browser->folder_recordings &&
            strcmp(nav->relative, browser->folder_relative) == 0) {
            nav->list = browser->list;
            return;
        }
    }
    if (browser->folder_nav_count >= VFH_FOLDER_NAV_MAX) return;
    vfh_folder_nav *nav = &browser->folder_nav[browser->folder_nav_count++];
    memset(nav, 0, sizeof(*nav));
    nav->recordings = browser->folder_recordings;
    snprintf(nav->relative, sizeof(nav->relative), "%s", browser->folder_relative);
    nav->list = browser->list;
}

static void vfh_build_continue_view(vfh_browser *browser,
                                    const vfh_resume_snapshot *resume_snapshot) {
    for (size_t i = 0; i < browser->catalog.count; i++) {
        const vfh_library_item *item = &browser->catalog.items[i];
        char path[VFH_SOURCE_PATH_MAX];
        if (!vfh_library_resolve_path(item, &browser->sources, browser->recordings_path,
                                      path, sizeof(path)) ||
            vfh_resume_for_library_item(resume_snapshot, item, path) <= 0.0)
            continue;
        vfh_add_catalog_video(browser, resume_snapshot, i);
    }
}

static void vfh_build_recent_view(vfh_browser *browser,
                                  const vfh_resume_snapshot *resume_snapshot) {
    for (size_t i = 0; i < browser->catalog.count; i++)
        if (browser->catalog.items[i].available)
            vfh_add_catalog_video(browser, resume_snapshot, i);
    qsort(browser->entries, (size_t)browser->entry_count, sizeof(browser->entries[0]),
          vfh_recent_compare);
}

static void vfh_build_folders_view(vfh_browser *browser,
                                   const vfh_resume_snapshot *resume_snapshot) {
    if (!browser) return;
    if (!browser->folder_recordings && !browser->folder_relative[0] &&
        vfh_recordings_folder_visible(browser, resume_snapshot))
        vfh_add_folder(browser, "Recorded Gameplay", "", true);
    for (size_t i = 0; i < browser->catalog.count; i++) {
        const vfh_library_item *item = &browser->catalog.items[i];
        if (!item->available || (item->content_kind == VFH_CONTENT_RECORDING) !=
                                browser->folder_recordings) continue;
        const char *rest = NULL;
        if (!vfh_folder_starts_with(item->relative_path, browser->folder_relative, &rest) ||
            !rest || !rest[0]) continue;
        const char *slash = strchr(rest, '/');
        if (slash) {
            char child[VFH_LIBRARY_RELATIVE_PATH_MAX];
            if (browser->folder_relative[0])
                snprintf(child, sizeof(child), "%s/%.*s", browser->folder_relative,
                         (int)(slash - rest), rest);
            else
                snprintf(child, sizeof(child), "%.*s", (int)(slash - rest), rest);
            char name[VFH_NAME_MAX];
            snprintf(name, sizeof(name), "%.*s", (int)(slash - rest), rest);
            vfh_add_folder(browser, name, child, browser->folder_recordings);
        } else {
            vfh_add_catalog_video(browser, resume_snapshot, i);
        }
    }
    qsort(browser->entries, (size_t)browser->entry_count, sizeof(browser->entries[0]),
          vfh_entry_compare);
}

static const char *vfh_library_empty_message(const vfh_browser *browser) {
    if (!browser || browser->sources.count <= 0)
        return "No video roots are configured.";
    int available = 0;
    for (int i = 0; i < browser->sources.count; i++)
        if (browser->sources.items[i].available) available++;
    if (!available) return "All configured video roots are unavailable.";
    return "No supported videos in the available Videos folders.";
}

static void vfh_scan_catalog(vfh_browser *browser) {
    browser->entry_count = 0;
    vfh_clear_poster(browser);
    vfh_resume_snapshot resume_snapshot;
    vfh_resume_snapshot_init(&resume_snapshot);
    (void)vfh_resume_snapshot_load(&resume_snapshot);
    switch (browser->tab) {
        case VFH_TAB_CONTINUE: vfh_build_continue_view(browser, &resume_snapshot); break;
        case VFH_TAB_RECENT:   vfh_build_recent_view(browser, &resume_snapshot); break;
        case VFH_TAB_FOLDERS:  vfh_build_folders_view(browser, &resume_snapshot); break;
        default: break;
    }
    vfh_resume_snapshot_destroy(&resume_snapshot);
    vfh_restore_current_list(browser);
    if (browser->entry_count == 0) {
        if (browser->tab == VFH_TAB_CONTINUE)
            vfh_set_message(browser, "Nothing to continue watching yet.");
        else if (browser->tab == VFH_TAB_FOLDERS && browser->folder_recordings)
            vfh_set_message(browser, "No gameplay recordings yet.");
        else
            vfh_set_message(browser, vfh_library_empty_message(browser));
    }
    else
        browser->message[0] = '\0';
}

/* A catalog scan only walks the filesystem, so the first browser frame never
 * waits on FFmpeg.  Probe cache misses in the one existing catalog worker and
 * publish the completed copy on the UI thread.  The worker owns `library`,
 * which keeps render state free of cross-thread mutation. */
static void vfh_catalog_probe_missing_metadata(vfh_library *library,
                                               const vfh_sources *sources,
                                               const char *recordings_path,
                                               const atomic_bool *cancel) {
    if (!library || !sources) return;
    for (size_t i = 0; i < library->count; i++) {
        if (cancel && atomic_load(cancel)) return;
        vfh_library_item *item = &library->items[i];
        char path[VFH_SOURCE_PATH_MAX];
        if (!item->available ||
            !vfh_library_resolve_path(item, sources, recordings_path, path, sizeof(path)))
            continue;
        char sidecar[VFH_ART_PATH_MAX];
        bool has_sidecar = vfh_art_find_sidecar(path, sidecar, sizeof(sidecar));
        if (!item->metadata_ready) {
            vfh_media_metadata metadata;
            if (vfh_media_probe_metadata(path, &metadata)) {
                item->duration = metadata.duration;
                if (!item->nfo_title && metadata.title[0])
                    snprintf(item->display_title, sizeof(item->display_title), "%s", metadata.title);
                if (!item->nfo_year && metadata.year > 0) item->year = metadata.year;
                snprintf(item->container, sizeof(item->container), "%s", metadata.container);
                snprintf(item->video_codec, sizeof(item->video_codec), "%s", metadata.video_codec);
                snprintf(item->audio_codec, sizeof(item->audio_codec), "%s", metadata.audio_codec);
                item->has_embedded_art = metadata.has_embedded_art;
                item->metadata_ready = true;
            }
        }
        if (cancel && atomic_load(cancel)) return;
        if (has_sidecar) item->art_source = VFH_LIBRARY_ART_SIDECAR;
        else if (item->metadata_ready)
            item->art_source = item->has_embedded_art ? VFH_LIBRARY_ART_EMBEDDED
                                                       : VFH_LIBRARY_ART_GENERATED;
    }
}

static bool vfh_rescan_cancelled(void *opaque) {
    const vfh_rescan_worker *worker = opaque;
    return worker && atomic_load(&worker->cancel);
}

static void *vfh_rescan_thread(void *opaque) {
    vfh_rescan_worker *worker = opaque;
    if (!worker) return NULL;
    vfh_library_init(&worker->result);
    (void)vfh_library_load(&worker->result);
    char error[sizeof(worker->error)] = "";
    vfh_library_scan_result scan = vfh_library_scan_cancellable(
        &worker->result, &worker->sources, worker->recordings_path,
        vfh_rescan_cancelled, worker, error, sizeof(error));
    bool cancelled = scan == VFH_LIBRARY_SCAN_CANCELLED || vfh_rescan_cancelled(worker);
    /* A partial scan is a usable library plus a warning, not a failure: one
       unreadable folder must not leave the browser empty. */
    bool succeeded = (scan == VFH_LIBRARY_SCAN_COMPLETE ||
                      scan == VFH_LIBRARY_SCAN_PARTIAL) && !cancelled;
    if (succeeded) {
        vfh_catalog_probe_missing_metadata(&worker->result, &worker->sources,
                                           worker->recordings_path, &worker->cancel);
        cancelled = vfh_rescan_cancelled(worker);
        if (!cancelled) (void)vfh_library_save(&worker->result);
        else succeeded = false;
    }
    pthread_mutex_lock(&worker->mutex);
    worker->succeeded = succeeded;
    worker->cancelled = cancelled;
    snprintf(worker->error, sizeof(worker->error), "%s", error);
    worker->complete = true;
    pthread_mutex_unlock(&worker->mutex);
    return NULL;
}

static void vfh_rescan_start(vfh_browser *browser) {
    if (!browser || browser->rescan.running) return;
    vfh_rescan_worker *worker = &browser->rescan;
    vfh_sources_refresh(&browser->sources);
    memset(&worker->sources, 0, sizeof(worker->sources));
    worker->sources = browser->sources;
    snprintf(worker->recordings_path, sizeof(worker->recordings_path), "%s",
             browser->recordings_path);
    worker->complete = worker->succeeded = worker->cancelled = false;
    worker->error[0] = '\0';
    atomic_store(&worker->cancel, false);
    vfh_library_destroy(&worker->result);
    worker->running = pthread_create(&worker->thread, NULL, vfh_rescan_thread, worker) == 0;
    if (!worker->running) vfh_set_message(browser, "Unable to start a library rescan.");
    else {
        browser->rescan_needs_resume = false;
        vfh_set_message(browser, "Rescanning library…");
    }
}

static bool vfh_rescan_finish(vfh_browser *browser, bool wait) {
    if (!browser || !browser->rescan.running) return false;
    vfh_rescan_worker *worker = &browser->rescan;
    pthread_mutex_lock(&worker->mutex);
    bool complete = worker->complete;
    pthread_mutex_unlock(&worker->mutex);
    if (!complete && !wait) return false;
    if (pthread_join(worker->thread, NULL) != 0) return false;
    worker->running = false;
    if (worker->succeeded) {
        vfh_library_destroy(&browser->catalog);
        browser->catalog = worker->result;
        memset(&worker->result, 0, sizeof(worker->result));
        vfh_scan_catalog(browser);
        /* A rescan is the explicit retry affordance for a previously terminal
           thumbnail request. Invalidate the current selection too: waiting
           for a cursor move would make a recovered poster look permanently
           failed to the person who requested the rescan. */
        vfh_clear_poster(browser);
        browser->poster_retry_pending = true;
        vfh_set_message(browser, worker->error[0] ? worker->error : "Library rescan complete.");
        return true;
    }
    vfh_library_destroy(&worker->result);
    if (worker->cancelled) return false;
    vfh_set_message(browser, worker->error[0] ? worker->error : "Library rescan failed.");
    return false;
}

/* Playback must not compete with the card scanner or FFmpeg metadata probes.
 * Signalling the transactional worker makes the join prompt and discards only
 * its unpublished result; return to browsing resumes the unfinished refresh. */
static void vfh_rescan_cancel(vfh_browser *browser, bool resume_when_browsing) {
    if (!browser || !browser->rescan.running) return;
    vfh_rescan_worker *worker = &browser->rescan;
    atomic_store(&worker->cancel, true);
    if (pthread_join(worker->thread, NULL) != 0) return;
    worker->running = false;
    vfh_library_destroy(&worker->result);
    if (resume_when_browsing) browser->rescan_needs_resume = true;
}

static void vfh_parent_directory(vfh_browser *browser) {
    if (!browser || browser->tab != VFH_TAB_FOLDERS) return;
    vfh_store_current_list(browser);
    if (browser->folder_recordings && !browser->folder_relative[0]) {
        browser->folder_recordings = false;
    } else if (browser->folder_relative[0]) {
        char *slash = strrchr(browser->folder_relative, '/');
        if (slash) *slash = '\0';
        else browser->folder_relative[0] = '\0';
    } else {
        return;
    }
    vfh_scan_catalog(browser);
}

static bool vfh_folder_art_path(const vfh_browser *browser, const vfh_entry *entry,
                                char *out, size_t out_size) {
    if (!browser || !entry || !out || out_size == 0 || entry->kind != VFH_ENTRY_DIRECTORY)
        return false;
    out[0] = '\0';
    if (entry->recordings_folder) {
        char directory[VFH_SOURCE_PATH_MAX];
        int length = entry->path[0]
            ? snprintf(directory, sizeof(directory), "%s/%s", browser->recordings_path, entry->path)
            : snprintf(directory, sizeof(directory), "%s", browser->recordings_path);
        return length >= 0 && length < (int)sizeof(directory) &&
               vfh_art_find_folder_art(directory, out, (int)out_size);
    }
    for (int i = 0; i < browser->sources.count; i++) {
        const vfh_source *source = &browser->sources.items[i];
        if (!source->available) continue;
        char directory[VFH_SOURCE_PATH_MAX];
        int length = entry->path[0]
            ? snprintf(directory, sizeof(directory), "%s/%s", source->root, entry->path)
            : snprintf(directory, sizeof(directory), "%s", source->root);
        if (length >= 0 && length < (int)sizeof(directory) &&
            vfh_art_find_folder_art(directory, out, (int)out_size)) return true;
    }
    return false;
}

static void vfh_sync_poster(vfh_browser *browser) {
    if (browser->list.cursor < 0 || browser->list.cursor >= browser->entry_count) {
        vfh_clear_poster(browser);
        return;
    }
    const vfh_entry *entry = &browser->entries[browser->list.cursor];
    if (entry->kind == VFH_ENTRY_DIRECTORY) {
        char art[VFH_ART_PATH_MAX], key[VFH_ART_PATH_MAX];
        int key_length = snprintf(key, sizeof(key), "folder:%c:%s",
                                  entry->recordings_folder ? 'r' : 'v', entry->path);
        if (key_length < 0 || key_length >= (int)sizeof(key) ||
            !vfh_folder_art_path(browser, entry, art, sizeof(art))) {
            vfh_clear_poster(browser);
            return;
        }
        if (strcmp(browser->poster_requested_path, key) != 0) {
            vfh_clear_poster(browser);
            snprintf(browser->poster_requested_path, sizeof(browser->poster_requested_path), "%s", key);
            browser->poster = cat_load_image(art);
            browser->poster_state = browser->poster ? VFH_THUMB_READY : VFH_THUMB_ERROR;
            if (browser->poster)
                snprintf(browser->poster_path, sizeof(browser->poster_path), "%s", entry->path);
        }
        return;
    }
    if (entry->kind != VFH_ENTRY_VIDEO) {
        vfh_clear_poster(browser);
        return;
    }
    if (strcmp(browser->poster_requested_path, entry->path) != 0) {
        vfh_clear_poster(browser);
        snprintf(browser->poster_requested_path, sizeof(browser->poster_requested_path), "%s",
                 entry->path);
        if (browser->thumb_worker_ready) {
            if (browser->poster_retry_pending) {
                vfh_thumb_worker_retry(&browser->thumbs, entry->path);
                browser->poster_retry_pending = false;
            } else {
                vfh_thumb_worker_request(&browser->thumbs, entry->path);
            }
            browser->poster_state = vfh_thumb_worker_state(&browser->thumbs, entry->path);
        }
    }
    if (browser->poster && strcmp(browser->poster_path, entry->path) == 0) return;
    SDL_Surface *surface = browser->thumb_worker_ready
        ? vfh_thumb_worker_take(&browser->thumbs, entry->path) : NULL;
    if (surface) {
        browser->poster = cat_texture_from_surface(surface);
        SDL_FreeSurface(surface);
        if (!browser->poster) browser->poster_state = VFH_THUMB_ERROR;
    }
    if (browser->thumb_worker_ready)
        browser->poster_state = vfh_thumb_worker_state(&browser->thumbs, entry->path);
    if (browser->poster_state == VFH_THUMB_PENDING) {
        char cache[VFH_THUMB_PATH_MAX];
        bool failure_is_error = false;
        vfh_thumb_cache_path(entry->path, cache, sizeof(cache));
        if (vfh_art_failure_exists(cache, &failure_is_error))
            browser->poster_state = failure_is_error ? VFH_THUMB_ERROR : VFH_THUMB_NOT_FOUND;
    }
    /* The worker hands its result over exactly once, and keeps the path as its
       current request afterwards. Leaving a row and coming back therefore drops
       the texture and then asks for a path the worker considers finished, so
       nothing arrives and the preview sits on "Creating poster..." forever. Ask
       again in that state; on a cache hit it is a file load, not a decode. */
    if (!browser->poster && browser->thumb_worker_ready &&
        browser->poster_state == VFH_THUMB_READY) {
        vfh_thumb_worker_retry(&browser->thumbs, entry->path);
        browser->poster_state = VFH_THUMB_PENDING;
    }
    if (browser->poster) snprintf(browser->poster_path, sizeof(browser->poster_path), "%s", entry->path);
}

static void vfh_format_capture_date(int64_t timestamp, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    out[0] = '\0';
    if (timestamp < INT64_C(20000101000000)) return;
    int year = (int)(timestamp / INT64_C(10000000000));
    int month = (int)((timestamp / INT64_C(100000000)) % 100);
    int day = (int)((timestamp / INT64_C(1000000)) % 100);
    if (month < 1 || month > 12 || day < 1 || day > 31) return;
    snprintf(out, (size_t)out_size, "%04d-%02d-%02d", year, month, day);
}

static void vfh_draw_entry(int index, int x, int y, int w, int h,
                           bool selected, void *opaque) {
    vfh_browser *browser = opaque;
    vfh_entry *entry = &browser->entries[index];
    cat_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(10);
    int text_x = x + pad;
    if (selected) {
        int pill_h = TTF_FontHeight(body) + cat_scale(8);
        cat_draw_pill(x, y + (h - pill_h) / 2, w - cat_scale(4), pill_h, theme->highlight);
    }
    const char *kind = "";
    char meta[48] = "";
    if (entry->kind == VFH_ENTRY_PARENT) {
        kind = "Up";
    } else if (entry->kind == VFH_ENTRY_DIRECTORY) {
        kind = entry->recordings_folder ? "Gameplay" : "Folder";
    } else {
        if (!entry->available) {
            snprintf(meta, sizeof(meta), "SD%d unavailable", entry->source_index + 1);
            kind = meta;
        } else if (browser->tab == VFH_TAB_CONTINUE && entry->resume_seconds > 0.0) {
            char elapsed[24];
            vfh_media_format_duration(entry->resume_seconds, elapsed, (int)sizeof(elapsed));
            if (entry->has_duration && entry->duration > 0.0) {
                int percent = (int)(entry->resume_seconds * 100.0 / entry->duration + 0.5);
                if (percent > 99) percent = 99;
                snprintf(meta, sizeof(meta), "%d%% · %s", percent, elapsed);
            } else {
                snprintf(meta, sizeof(meta), "Resume %s", elapsed);
            }
            kind = meta;
        } else {
            vfh_media_format_duration(entry->has_duration ? entry->duration : 0.0,
                                      meta, (int)sizeof(meta));
            /* Two rows reading the same are two rows the user cannot choose
               between. A shared card is answered with the SD badge; a shared
               title inside one card has only the filename left to tell them
               apart, which is exactly what the embedded-title precedence threw
               away. Video Information still carries the full original name. */
            bool collision = false, same_source_collision = false;
            for (int i = 0; i < browser->entry_count; i++) {
                const vfh_entry *other = &browser->entries[i];
                if (other == entry || other->kind != VFH_ENTRY_VIDEO ||
                    strcasecmp(other->name, entry->name) != 0) continue;
                collision = true;
                if (other->source_index == entry->source_index) {
                    same_source_collision = true;
                    break;
                }
            }
            if (entry->content_kind == VFH_CONTENT_RECORDING) {
                char capture_date[16];
                vfh_format_capture_date(entry->capture_timestamp, capture_date, sizeof(capture_date));
                if (capture_date[0] && entry->recording_part > 0)
                    snprintf(meta, sizeof(meta), "Gameplay · %s · Part %d", capture_date,
                             entry->recording_part);
                else if (capture_date[0])
                    snprintf(meta, sizeof(meta), "Gameplay · %s", capture_date);
                else if (entry->recording_part > 0)
                    snprintf(meta, sizeof(meta), "Gameplay · Part %d", entry->recording_part);
                else {
                    char duration_text[sizeof(meta)];
                    snprintf(duration_text, sizeof(duration_text), "%s", meta);
                    snprintf(meta, sizeof(meta), "Gameplay · %.24s", duration_text);
                }
            } else if (same_source_collision) {
                char stem[40];
                vfh_filename_stem(entry->path, stem, sizeof(stem));
                char duration_text[sizeof(meta)];
                snprintf(duration_text, sizeof(duration_text), "%s", meta);
                snprintf(meta, sizeof(meta), "%s · %.12s", stem, duration_text);
            } else if (collision) {
                char duration_text[sizeof(meta)];
                snprintf(duration_text, sizeof(duration_text), "%s", meta);
                snprintf(meta, sizeof(meta), "SD%d · %.28s", entry->source_index + 1,
                         duration_text);
            }
            kind = meta;
        }
        int thumb = h - cat_scale(8);
        int thumb_y = y + (h - thumb) / 2;
        cat_draw_color placeholder = theme->hint;
        placeholder.a = 38;
        cat_draw_rounded_rect(x + pad, thumb_y, thumb, thumb, cat_scale(4), placeholder);
        if (selected && browser->poster && strcmp(browser->poster_path, entry->path) == 0)
            cat_draw_image_rounded_ex(browser->poster, x + pad, thumb_y, thumb, thumb,
                                      cat_scale(4), CAT_CORNER_ALL);
        text_x += thumb + pad;
    }
    int meta_w = cat_measure_text(small, kind) + pad;
    cat_draw_text_ellipsized(body, entry->name, text_x,
                             y + (h - TTF_FontHeight(body)) / 2,
                             selected ? theme->highlighted_text : theme->text,
                             w - (text_x - x) - meta_w - pad * 2);
    cat_draw_text(small, kind, x + w - meta_w - pad,
                  y + (h - TTF_FontHeight(small)) / 2,
                  selected ? theme->highlighted_text : theme->hint);
}

static void vfh_draw_tabbar(const vfh_browser *browser, SDL_Rect rect) {
    if (!browser) return;
    cat_theme *theme = cat_get_theme();
    TTF_Font *font = cat_get_font(CAT_FONT_MEDIUM);
    int segment = rect.w / VFH_TAB_COUNT;
    int text_pad = cat_scale(6);
    int max_text_w = segment - text_pad * 2;
    for (int i = 0; i < VFH_TAB_COUNT; i++) {
        if (cat_measure_text(font, VFH_TAB_NAMES[i]) > max_text_w) {
            font = cat_get_font(CAT_FONT_SMALL);
            break;
        }
    }
    int pill_h = TTF_FontHeight(font) + cat_scale(8);
    for (int i = 0; i < VFH_TAB_COUNT; i++) {
        int x = rect.x + i * segment;
        bool active = i == (int)browser->tab;
        if (active)
            cat_draw_pill(x + cat_scale(3), rect.y + (rect.h - pill_h) / 2,
                          segment - cat_scale(6), pill_h, theme->highlight);
        cat_draw_text_ellipsized(font, VFH_TAB_NAMES[i], x + text_pad,
                                 rect.y + (rect.h - TTF_FontHeight(font)) / 2,
                                 active ? theme->highlighted_text : theme->hint,
                                 max_text_w);
    }
}

static void vfh_draw_preview(vfh_browser *browser, SDL_Rect preview) {
    cat_theme *theme = cat_get_theme();
    int pad = cat_scale(12);
    cat_draw_color panel = theme->text;
    panel.a = 18;
    cat_draw_rounded_rect(preview.x, preview.y, preview.w, preview.h, cat_scale(10), panel);
    if (browser->list.cursor < 0 || browser->list.cursor >= browser->entry_count) return;
    vfh_entry *entry = &browser->entries[browser->list.cursor];
    if (entry->kind != VFH_ENTRY_VIDEO) {
        bool folder_art = entry->kind == VFH_ENTRY_DIRECTORY && browser->poster &&
                          strcmp(browser->poster_path, entry->path) == 0;
        int text_y = preview.y + pad;
        if (folder_art) {
            int side = preview.w - pad * 2;
            if (side > preview.h / 2) side = preview.h / 2;
            cat_draw_image_rounded_ex(browser->poster, preview.x + (preview.w - side) / 2,
                                      preview.y + pad, side, side, cat_scale(8), CAT_CORNER_ALL);
            text_y = preview.y + preview.h / 2 + pad;
        }
        cat_draw_text(cat_get_font(CAT_FONT_LARGE), entry->kind == VFH_ENTRY_DIRECTORY ? "Folder" : "Videos",
                      preview.x + pad, text_y, theme->text);
        cat_draw_text_wrapped(cat_get_font(CAT_FONT_SMALL),
                              entry->kind == VFH_ENTRY_DIRECTORY
                                  ? "Press A to browse this folder."
                                  : "Press B to return to the parent folder.",
                              preview.x + pad, text_y + TTF_FontHeight(cat_get_font(CAT_FONT_LARGE)) + cat_scale(8),
                              preview.w - pad * 2, theme->hint, CAT_ALIGN_LEFT);
        return;
    }
    if (browser->poster) {
        int side = preview.w - pad * 2;
        if (side > preview.h / 2) side = preview.h / 2;
        cat_draw_image_rounded_ex(browser->poster, preview.x + (preview.w - side) / 2,
                                  preview.y + pad, side, side, cat_scale(8), CAT_CORNER_ALL);
    } else {
        const char *poster_status = "Creating poster…";
        if (!browser->thumb_worker_ready) poster_status = "Poster service unavailable";
        else if (browser->poster_state == VFH_THUMB_NOT_FOUND) poster_status = "Poster unavailable";
        else if (browser->poster_state == VFH_THUMB_ERROR) poster_status = "Poster generation failed";
        cat_draw_text(cat_get_font(CAT_FONT_SMALL), poster_status,
                      preview.x + pad, preview.y + pad, theme->hint);
    }
    int text_y = preview.y + preview.h / 2 + pad;
    char duration[48];
    vfh_media_format_duration(entry->has_duration ? entry->duration : 0.0,
                              duration, (int)sizeof(duration));
    cat_draw_text_ellipsized(cat_get_font(CAT_FONT_MEDIUM), entry->name,
                             preview.x + pad, text_y, theme->text, preview.w - pad * 2);
    cat_draw_text(cat_get_font(CAT_FONT_SMALL), duration, preview.x + pad,
                  text_y + TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(6), theme->hint);
}

/* A footer hint that knows how to shrink.
 *
 * Catastrophe decides what fits while it renders, folding the overflow into a
 * `+N` marker that VFH deliberately never wants to produce: a hint the user
 * cannot read is an action they cannot find. So the row is fitted here first,
 * and only labels that survive are handed over. */
typedef struct {
    cat_button button;
    const char *button_text;    /* always explicit, so the estimate is exact */
    const char *narrow_button_text;
    const char *label;
    const char *narrow_label;   /* NULL keeps the full label under pressure */
    bool is_confirm;
} vfh_footer_hint;

/* Mirrors Catastrophe's footer geometry closely enough to predict its fit, and
 * rounds against itself where it cannot: multi-character button text is
 * measured in the small tier where Catastrophe renders it in the tiny one, so
 * the estimate is never under the truth. Erring this way costs an occasional
 * unnecessary abbreviation; erring the other way costs an unreachable action. */
static int vfh_footer_group_width(const vfh_footer_hint *hints, int count, bool confirm,
                                  bool narrow) {
    TTF_Font *hint_font = cat_get_font(CAT_FONT_SMALL);
    int margin = cat_device_scale(5);   /* CAT__BUTTON_MARGIN */
    int badge = cat_device_scale(20);   /* CAT__BUTTON_SIZE */
    int inner = 0, shown = 0;
    for (int i = 0; i < count; i++) {
        if (hints[i].is_confirm != confirm) continue;
        const char *button = narrow && hints[i].narrow_button_text ? hints[i].narrow_button_text
                                                                   : hints[i].button_text;
        const char *label = narrow && hints[i].narrow_label ? hints[i].narrow_label
                                                            : hints[i].label;
        /* Catastrophe gives a single-codepoint badge a fixed circle and grows
           only for longer overrides. */
        int badge_w = button && button[0] && !button[1] ? badge
                                                        : badge / 2 + cat_measure_text(hint_font, button);
        if (shown++) inner += margin;
        inner += badge_w + margin + cat_measure_text(hint_font, label) + margin;
    }
    return shown ? margin + inner + margin : 0;
}

static bool vfh_footer_fits(const vfh_footer_hint *hints, int count, bool narrow,
                            int available) {
    return vfh_footer_group_width(hints, count, false, narrow) +
           vfh_footer_group_width(hints, count, true, narrow) <= available;
}

/* Fit the row by abbreviating first and, only if that is not enough, by
 * dropping the lowest-priority hints outright.
 *
 * Dropping beats letting Catastrophe render its `+N` marker: the marker costs
 * width of its own and, unless an app opts into the reveal chord, names
 * actions the user has no way to see. A shorter row where every hint shown is
 * real is more honest than a longer one ending in a number. Callers order
 * hints by importance, so what goes is always the most expendable. */
static void vfh_draw_footer_hints(const vfh_footer_hint *hints, int count) {
    if (!hints || count <= 0) return;
    /* Footer height is padding + pill, so the pill height recovers the padding
       Catastrophe reserves at each screen edge without reaching into it. */
    int padding = cat_get_footer_height() - cat_device_scale(30);
    if (padding < 0) padding = 0;
    int available = cat_get_screen_width() - padding * 2;

    bool narrow = !vfh_footer_fits(hints, count, false, available);
    vfh_footer_hint fitted[16];
    if (count > (int)(sizeof(fitted) / sizeof(fitted[0])))
        count = (int)(sizeof(fitted) / sizeof(fitted[0]));
    memcpy(fitted, hints, (size_t)count * sizeof(*fitted));

    while (count > 0 && !vfh_footer_fits(fitted, count, narrow, available)) {
        int victim = -1;
        for (int i = count - 1; i >= 0; i--) {
            if (!fitted[i].is_confirm) { victim = i; break; }
        }
        if (victim < 0) break;  /* only the confirm action left; let it clip */
        memmove(&fitted[victim], &fitted[victim + 1],
                (size_t)(count - victim - 1) * sizeof(*fitted));
        count--;
    }

    cat_footer_item items[16];
    for (int i = 0; i < count; i++) {
        items[i] = (cat_footer_item){
            .button = fitted[i].button,
            .label = narrow && fitted[i].narrow_label ? fitted[i].narrow_label : fitted[i].label,
            .is_confirm = fitted[i].is_confirm,
            .button_text = narrow && fitted[i].narrow_button_text ? fitted[i].narrow_button_text
                                                                  : fitted[i].button_text,
        };
    }
    cat_draw_footer(items, count);
}

/* Footer hints are context-sensitive and ordered by importance.
 *
 * Two rules keep every offered action reachable. Only hints that would do
 * something right now are listed at all, so a fixed row never spends width on
 * a no-op. And because Catastrophe keeps a *prefix* of the action group and
 * folds the rest into its `+N` marker, the order here is the priority order:
 * whatever cannot fit is the least useful hint, not the most useful one. */
static void vfh_draw_browser_footer(vfh_browser *browser) {
    const vfh_entry *selected =
        browser->list.cursor >= 0 && browser->list.cursor < browser->entry_count
            ? &browser->entries[browser->list.cursor] : NULL;
    bool on_video = selected && selected->kind == VFH_ENTRY_VIDEO;
    /* B is a no-op at the virtual root; MENU stays the explicit app exit. */
    bool can_go_up = browser->tab == VFH_TAB_FOLDERS &&
                     (browser->folder_recordings || browser->folder_relative[0]);
    /* Letter jumping only earns its place once the list outgrows one screen. */
    bool can_jump = browser->entry_count > browser->list.visible_rows;

    vfh_footer_hint hints[8];
    int count = 0;
    if (on_video)
        hints[count++] = (vfh_footer_hint){ .button = CAT_BTN_X, .button_text = "X",
                                            .label = "Actions" };
    hints[count++] = (vfh_footer_hint){ .button = CAT_BTN_L1, .button_text = "L/R",
                                        .label = "Tabs" };
    if (can_go_up)
        hints[count++] = (vfh_footer_hint){ .button = CAT_BTN_B, .button_text = "B",
                                            .label = "Back" };
    if (!browser->rescan.running)
        hints[count++] = (vfh_footer_hint){ .button = CAT_BTN_SELECT, .button_text = "SELECT",
                                            .narrow_button_text = "SEL", .label = "Rescan",
                                            .narrow_label = "Scan" };
    hints[count++] = (vfh_footer_hint){ .button = CAT_BTN_MENU, .button_text = "MENU",
                                        .narrow_button_text = "M", .label = "Quit" };
    /* Ranked last deliberately. Letter jumping is the one hint whose loss costs
       nothing but discoverability - Up/Down still walks the list - and its
       multi-character badge is the widest of the set, so surrendering it buys
       back the most room for the actions that cannot be guessed. */
    if (can_jump)
        hints[count++] = (vfh_footer_hint){ .button = CAT_BTN_LEFT, .button_text = "<->",
                                            .label = "Jump" };
    if (selected)
        hints[count++] = (vfh_footer_hint){ .button = CAT_BTN_A, .button_text = "A",
                                            .label = on_video ? "Play" : "Open",
                                            .is_confirm = true };
    vfh_draw_footer_hints(hints, count);
}

static void vfh_draw_browser(vfh_browser *browser) {
    cat_draw_background();
    cat_draw_screen_title("Video Library", NULL);
    SDL_Rect content = cat_get_content_rect(true, true, false);
    int tab_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(14);
    SDL_Rect tabs = { content.x, content.y, content.w, tab_h };
    vfh_draw_tabbar(browser, tabs);
    content.y += tab_h + cat_scale(4);
    content.h -= tab_h + cat_scale(4);
    int gap = cat_scale(10);
    bool has_preview = content.w >= cat_scale(430);
    SDL_Rect list_rect = content;
    SDL_Rect preview = {0, 0, 0, 0};
    if (has_preview) {
        preview.w = content.w * 34 / 100;
        preview.x = content.x + content.w - preview.w;
        preview.y = content.y;
        preview.h = content.h;
        list_rect.w = preview.x - content.x - gap;
    }
    if (browser->entry_count > 0) {
        int base_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
        int rows = list_rect.h / base_h;
        if (rows < 1) rows = 1;
        browser->list.visible_rows = rows;
        int item_h = list_rect.h / rows;
        cat_draw_list_pane(list_rect.x, list_rect.y, list_rect.w, list_rect.h,
                           browser->entry_count, &browser->list, item_h,
                           vfh_draw_entry, browser);
    } else {
        cat_draw_text(cat_get_font(CAT_FONT_LARGE), "No videos found",
                      list_rect.x + cat_scale(12), list_rect.y + cat_scale(12), cat_get_theme()->text);
        if (browser->message[0])
            cat_draw_text_wrapped(cat_get_font(CAT_FONT_SMALL), browser->message,
                                  list_rect.x + cat_scale(12), list_rect.y + cat_scale(48),
                                  list_rect.w - cat_scale(24), cat_get_theme()->hint, CAT_ALIGN_LEFT);
    }
    if (browser->message[0] && browser->entry_count > 0)
        cat_draw_text_ellipsized(cat_get_font(CAT_FONT_SMALL), browser->message,
                                 content.x, content.y + content.h - TTF_FontHeight(cat_get_font(CAT_FONT_SMALL)),
                                 cat_get_theme()->hint, list_rect.w);
    if (has_preview) vfh_draw_preview(browser, preview);
    vfh_draw_browser_footer(browser);
}

static void vfh_upload_due_video(vfh_browser *browser) {
    if (!browser->player) return;
    AVFrame *frame = NULL;
    if (!vfh_player_take_due_video_frame(browser->player, &frame, NULL)) return;
    if (!frame) return;
    Uint32 texture_format = frame->format == AV_PIX_FMT_NV12
                          ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
    if (!browser->video_texture || browser->video_width != frame->width ||
        browser->video_height != frame->height || browser->video_format != texture_format) {
        if (browser->video_texture) SDL_DestroyTexture(browser->video_texture);
        browser->video_texture = SDL_CreateTexture(cat_get_renderer(), texture_format,
                                                   SDL_TEXTUREACCESS_STREAMING,
                                                   frame->width, frame->height);
        browser->video_width = frame->width;
        browser->video_height = frame->height;
        browser->video_format = texture_format;
        /* Anamorphic sources (DVD rips at 720x576 meant for 16:9) carry a
           non-square sample aspect. Taken from the frame so it also covers the
           MPP path, which does not go through an AVCodecContext. */
        browser->pixel_aspect = 1.0;
        if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
            browser->pixel_aspect = (double)frame->sample_aspect_ratio.num /
                                    (double)frame->sample_aspect_ratio.den;
    }
    if (browser->video_texture && frame->format == AV_PIX_FMT_NV12)
        (void)SDL_UpdateNVTexture(browser->video_texture, NULL,
                                  frame->data[0], frame->linesize[0],
                                  frame->data[1], frame->linesize[1]);
    else if (browser->video_texture)
        (void)SDL_UpdateYUVTexture(browser->video_texture, NULL,
                                   frame->data[0], frame->linesize[0],
                                   frame->data[1], frame->linesize[1],
                                   frame->data[2], frame->linesize[2]);
    av_frame_free(&frame);
}

/* Where the video lands on screen, per aspect mode. FIT letterboxes (correct
   geometry, bars); FILL crops to cover the screen (no bars, edges lost);
   STRETCH ignores aspect entirely. Anamorphic content is honoured via the
   stream's sample aspect ratio, so 16:9-in-720x576 does not render squashed. */
static SDL_Rect vfh_video_rect(const vfh_browser *browser) {
    int screen_w = cat_get_screen_width();
    int screen_h = cat_get_screen_height();
    double display_w = (double)browser->video_width * browser->pixel_aspect;
    double display_h = (double)browser->video_height;
    if (display_w <= 0.0 || display_h <= 0.0)
        return (SDL_Rect){ 0, 0, screen_w, screen_h };

    if (browser->aspect_mode == VFH_ASPECT_STRETCH)
        return (SDL_Rect){ 0, 0, screen_w, screen_h };

    double scale_w = (double)screen_w / display_w;
    double scale_h = (double)screen_h / display_h;
    double scale = browser->aspect_mode == VFH_ASPECT_FILL
                 ? (scale_w > scale_h ? scale_w : scale_h)
                 : (scale_w < scale_h ? scale_w : scale_h);
    int width = (int)(display_w * scale + 0.5);
    int height = (int)(display_h * scale + 0.5);
    return (SDL_Rect){ (screen_w - width) / 2, (screen_h - height) / 2, width, height };
}

static const char *vfh_aspect_label(int mode) {
    switch (mode) {
        case VFH_ASPECT_FILL:    return "Fill";
        case VFH_ASPECT_STRETCH: return "Stretch";
        default:                 return "Fit";
    }
}

/* One solid triangle, scanline-filled. `left_x` is the left edge of its bounding
   box and `h` its full height; the apex is on the right when `right` is true.
   Written once and reused: hand-rolling the direction per glyph is what got
   rewind and fast-forward pointing the wrong way. */
static void vfh_draw_triangle(int left_x, int cy, int w, int h, bool right,
                              cat_draw_color color) {
    for (int i = 0; i < w; i++) {
        /* Column height shrinks toward the apex. */
        double t = (double)i / (double)(w > 1 ? w - 1 : 1);
        double along = right ? t : 1.0 - t;      /* 0 at the base, 1 at the apex */
        int col_h = (int)(h * (1.0 - along) + 0.5);
        if (col_h < 1) col_h = 1;
        cat_draw_rect(left_x + i, cy - col_h / 2, 1, col_h, color);
    }
}

/* A focused OSD control gets a compact, high-contrast halo instead of relying
   on colour alone. It intentionally matches the focused Disco Boy pills. */
static void vfh_draw_osd_focus_halo(SDL_Rect rect) {
    cat_theme *theme = cat_get_theme();
    int border = cat_scale(2) > 0 ? cat_scale(2) : 1;
    int radius = cat_scale(8) > 0 ? cat_scale(8) : 2;
    cat_draw_rounded_rect(rect.x - border, rect.y - border,
                          rect.w + border * 2, rect.h + border * 2,
                          radius + border, theme->highlight);
}

/* Transport row: skip-back / rewind / play-pause / forward / skip-next, drawn
   as glyphs rather than text so it reads at a glance and needs no extra font. */
static void vfh_draw_transport(const vfh_browser *browser, int cx, int cy, int size,
                               bool show_focus) {
    cat_draw_color on = { 255, 255, 255, 235 };
    int gap = size * 2;
    int bar = size / 5 > 0 ? size / 5 : 1;
    int half = size / 2;
    bool paused = vfh_player_is_paused(browser->player);
    static const vfh_osd_focus controls[] = {
        VFH_OSD_FOCUS_PREVIOUS, VFH_OSD_FOCUS_REWIND, VFH_OSD_FOCUS_PLAY_PAUSE,
        VFH_OSD_FOCUS_FORWARD, VFH_OSD_FOCUS_NEXT,
    };
    int centers[] = { cx - gap * 2, cx - gap, cx, cx + gap, cx + gap * 2 };
    for (int i = 0; i < 5; i++) {
        if (show_focus && browser->osd.focus == controls[i]) {
            SDL_Rect halo = { centers[i] - size, cy - size, size * 2, size * 2 };
            vfh_draw_osd_focus_halo(halo);
        }
    }

    /* |< skip-previous: bar on the left, triangle pointing left */
    cat_draw_rect(cx - gap * 2 - half, cy - half, bar, size, on);
    vfh_draw_triangle(cx - gap * 2 - half + bar + 1, cy, size, size, false, on);

    /* << rewind: two triangles pointing left */
    for (int b = 0; b < 2; b++)
        vfh_draw_triangle(cx - gap - half + b * (half + 1), cy, half, size, false, on);

    /* play / pause */
    if (paused)
        vfh_draw_triangle(cx - size / 3, cy, size, size, true, on);
    else {
        cat_draw_rect(cx - size / 3, cy - half, size / 4, size, on);
        cat_draw_rect(cx + size / 12, cy - half, size / 4, size, on);
    }

    /* >> fast-forward: two triangles pointing right */
    for (int b = 0; b < 2; b++)
        vfh_draw_triangle(cx + gap - half + b * (half + 1), cy, half, size, true, on);

    /* >| skip-next: triangle pointing right, bar on the right */
    vfh_draw_triangle(cx + gap * 2 - half, cy, size, size, true, on);
    cat_draw_rect(cx + gap * 2 - half + size + 1, cy - half, bar, size, on);
}

static void vfh_draw_osd_choice(int x, int y, int w, int h, const char *label, bool selected) {
    cat_theme *theme = cat_get_theme();
    if (selected) vfh_draw_osd_focus_halo((SDL_Rect){ x, y, w, h });
    cat_draw_rounded_rect(x, y, w, h, h / 2,
                          selected ? (cat_draw_color){ 28, 30, 38, 245 }
                                   : (cat_draw_color){ 20, 20, 26, 225 });
    TTF_Font *font = cat_get_font(CAT_FONT_SMALL);
    cat_draw_text_ellipsized(font, label, x + cat_scale(7),
                             y + (h - TTF_FontHeight(font)) / 2,
                             selected ? theme->text : theme->hint, w - cat_scale(14));
}

/* Draws into `popup`, which the caller has already cleared of other controls.
 * The submenu used to be centred over the panel, so the transport glyphs, the
 * second row and the output pill all bled out around its edges. It now takes
 * over the lower half of the panel instead of floating above it, and the
 * caller skips whatever it replaces. */
static void vfh_draw_osd_submenu(vfh_browser *browser, SDL_Rect popup) {
    const vfh_osd_submenu submenu = browser->osd.submenu;
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    cat_theme *theme = cat_get_theme();
    int inset = cat_scale(12);
    if (popup.w <= inset * 2 || popup.h <= inset * 2) return;
    cat_draw_rounded_rect(popup.x, popup.y, popup.w, popup.h, cat_scale(9),
                          (cat_draw_color){ 9, 10, 14, 248 });

    const char *title = "";
    switch (submenu) {
        case VFH_OSD_SUBMENU_QUEUE:     title = "Queue"; break;
        case VFH_OSD_SUBMENU_SUBTITLES: title = "Subtitles"; break;
        case VFH_OSD_SUBMENU_ASPECT:    title = "Aspect"; break;
        case VFH_OSD_SUBMENU_MORE:      title = "More"; break;
        default:                         title = "Controls"; break;
    }
    /* Header row: title left, exit hint right. Deriving the option row from the
       measured title height rather than a fixed offset is what stops the two
       colliding, and pairing them on one row leaves the sheet tall enough for a
       full-height option row underneath. */
    int header_h = TTF_FontHeight(body);
    cat_draw_text(body, title, popup.x + inset, popup.y + inset, theme->text);
    const char *back = vfh_osd_submenu_previews(submenu) ? "A Done  •  B Cancel" : "B Back";
    int back_w = cat_measure_text(small, back);
    cat_draw_text(small, back, popup.x + popup.w - inset - back_w,
                  popup.y + inset + (header_h - TTF_FontHeight(small)) / 2, theme->hint);

    int option_y = popup.y + inset + header_h + cat_scale(8);
    int option_h = cat_scale(30);
    int option_room = popup.y + popup.h - inset - option_y;
    if (option_h > option_room) option_h = option_room;
    if (option_h <= 0) return;
    if (submenu == VFH_OSD_SUBMENU_SUBTITLES) {
        if (!browser->subtitles) {
            cat_draw_text(small, "No external subtitles for this video.", popup.x + inset, option_y,
                          theme->hint);
        } else {
            int gap = cat_scale(8);
            int option_w = (popup.w - inset * 2 - gap) / 2;
            vfh_draw_osd_choice(popup.x + inset, option_y, option_w, option_h, "Off",
                                !browser->subtitles_on);
            vfh_draw_osd_choice(popup.x + inset + option_w + gap, option_y, option_w, option_h,
                                "On", browser->subtitles_on);
        }
    } else if (submenu == VFH_OSD_SUBMENU_ASPECT) {
        static const char *const modes[] = { "Fit", "Fill", "Stretch" };
        int gap = cat_scale(6);
        int option_w = (popup.w - inset * 2 - gap * 2) / 3;
        for (int i = 0; i < VFH_ASPECT_COUNT; i++)
            vfh_draw_osd_choice(popup.x + inset + i * (option_w + gap), option_y,
                                option_w, option_h, modes[i], browser->aspect_mode == i);
    } else if (submenu == VFH_OSD_SUBMENU_QUEUE) {
        if (browser->queue.count) {
            char summary[64];
            snprintf(summary, sizeof(summary), "%zu queued video%s — A opens queue.",
                     browser->queue.count, browser->queue.count == 1 ? "" : "s");
            cat_draw_text(small, summary, popup.x + inset, option_y, theme->hint);
        } else {
            cat_draw_text(small, "Queue is empty.", popup.x + inset, option_y, theme->hint);
        }
    } else if (submenu == VFH_OSD_SUBMENU_MORE) {
        int chapters = vfh_player_chapter_count(browser->player);
        if (chapters > 0) {
            char summary[64];
            snprintf(summary, sizeof(summary), "%d chapter%s — A opens chapters.", chapters,
                     chapters == 1 ? "" : "s");
            cat_draw_text(small, summary, popup.x + inset, option_y, theme->hint);
        } else {
            cat_draw_text(small, "No additional controls are available for this video.",
                          popup.x + inset, option_y, theme->hint);
        }
    } else {
        cat_draw_text(small, "No additional controls are available for this video.",
                      popup.x + inset, option_y, theme->hint);
    }

}

static void vfh_refresh_osd_capabilities(vfh_browser *browser) {
    if (!browser || !browser->player) return;
    vfh_osd_set_capabilities(&browser->osd, browser->queue.count > 0,
                             browser->subtitles != NULL,
                             vfh_player_chapter_count(browser->player) > 0);
}

/* Vertical layout of the playback panel, measured rather than guessed.
 *
 * Every row used to carry a hand-tuned offset (+8, +43, +88, +117, +145, +178)
 * that happened to suit one font size. Any change to one row silently pushed
 * into the next: the title clipped the output chip, the chip clipped the seek
 * rail, the hint fell off the panel entirely. Stacking the rows from their own
 * measured heights makes those collisions impossible to reintroduce, and lets
 * the panel size itself to its contents. */
typedef struct {
    int title_h;        /* title and clock share this row */
    int chip_h;         /* audio-output chip */
    int rail_h;         /* seek rail, tall enough for its handle */
    int bar_h;          /* the rail itself, centred in rail_h */
    int transport_h;    /* glyph row */
    int transport_size;
    int row_h;          /* second row of choices */
    int footer_h;       /* status and hint */
    int gap;
    int inner_pad;
    int height;         /* total panel height */
} vfh_osd_metrics;

static vfh_osd_metrics vfh_osd_layout(void) {
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    vfh_osd_metrics m;
    m.gap = cat_scale(8);
    m.inner_pad = cat_scale(10);
    m.title_h = body ? TTF_FontHeight(body) : cat_scale(24);
    m.footer_h = small ? TTF_FontHeight(small) : cat_scale(18);
    m.chip_h = m.footer_h + cat_scale(6);
    m.bar_h = cat_scale(6);
    /* The scrub handle is a circle centred on the rail; reserve its diameter so
       it cannot clip the rows above or below. */
    int handle = cat_scale(7) * 2;
    m.rail_h = handle > m.bar_h ? handle : m.bar_h;
    m.transport_size = cat_scale(16);
    m.transport_h = m.transport_size * 2;
    m.row_h = cat_scale(28);
    m.height = m.inner_pad + m.title_h + m.gap + m.chip_h + m.gap + m.rail_h +
               m.gap + m.transport_h + m.gap + m.row_h + m.gap + m.footer_h +
               m.inner_pad;
    return m;
}

static void vfh_draw_osd(vfh_browser *browser) {
    vfh_refresh_osd_capabilities(browser);
    double position = vfh_player_position(browser->player);
    double duration = vfh_player_duration(browser->player);
    int screen_w = cat_get_screen_width();
    int screen_h = cat_get_screen_height();
    int pad = cat_scale(16);
    vfh_osd_metrics metrics = vfh_osd_layout();
    int panel_h = metrics.height;
    int panel_y = screen_h - panel_h - pad;
    SDL_Rect panel = { pad, panel_y, screen_w - pad * 2, panel_h };
    bool pinned = browser->osd.state == VFH_OSD_PINNED;

    /* Scrim behind the panel: subtitles and light scenes both need the text to
       stay legible without dimming the whole frame. */
    cat_draw_rounded_rect(panel.x, panel.y, panel.w, panel.h, cat_scale(10),
                          (cat_draw_color){ 0, 0, 0, 185 });

    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int cursor = panel_y + metrics.inner_pad;   /* walks down the rows */
    int title_y = cursor;
    cat_draw_text_ellipsized(body, browser->playing_name, pad * 2, title_y,
                             (cat_draw_color){ 255, 255, 255, 245 },
                             screen_w - pad * 4 - cat_scale(90));

    char clock[80], elapsed[32], total[32];
    vfh_media_format_duration(position, elapsed, (int)sizeof(elapsed));
    vfh_media_format_duration(duration, total, (int)sizeof(total));
    if (duration > 0.0) snprintf(clock, sizeof(clock), "%s / %s", elapsed, total);
    else                snprintf(clock, sizeof(clock), "%s", elapsed);
    int clock_w = cat_measure_text(small, clock);
    /* Baseline-aligned with the title rather than offset into it. */
    cat_draw_text(small, clock, screen_w - pad * 2 - clock_w,
                  title_y + (metrics.title_h - metrics.footer_h) / 2,
                  (cat_draw_color){ 210, 210, 210, 230 });
    cursor += metrics.title_h + metrics.gap;

    char output_label[64];
    snprintf(output_label, sizeof(output_label), "Output: %s",
             vfh_audio_output_label(vfh_browser_audio_output(browser)));
    int output_w = cat_measure_text(small, output_label) + cat_scale(16);
    cat_draw_rounded_rect(pad * 2, cursor, output_w, metrics.chip_h,
                          metrics.chip_h / 2, (cat_draw_color){ 42, 45, 56, 220 });
    cat_draw_text(small, output_label, pad * 2 + cat_scale(8),
                  cursor + (metrics.chip_h - metrics.footer_h) / 2,
                  (cat_draw_color){ 220, 220, 230, 230 });
    cursor += metrics.chip_h + metrics.gap;

    /* Scrub bar, centred in a band tall enough for its handle. */
    int bar_x = pad * 2;
    int bar_w = screen_w - pad * 4;
    int rail_top = cursor;
    int bar_h = metrics.bar_h;
    int bar_y = rail_top + (metrics.rail_h - bar_h) / 2;
    cursor += metrics.rail_h + metrics.gap;
    if (pinned && browser->osd.focus == VFH_OSD_FOCUS_PROGRESS)
        vfh_draw_osd_focus_halo((SDL_Rect){ bar_x - cat_scale(3), bar_y - cat_scale(4),
                                             bar_w + cat_scale(6), bar_h + cat_scale(8) });
    cat_draw_rounded_rect(bar_x, bar_y, bar_w, bar_h, bar_h / 2,
                          (cat_draw_color){ 255, 255, 255, 60 });
    if (duration > 0.0) {
        double fraction = position / duration;
        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;
        int filled = (int)(bar_w * fraction);
        cat_theme *theme = cat_get_theme();
        cat_draw_rounded_rect(bar_x, bar_y, filled, bar_h, bar_h / 2, theme->highlight);
        cat_draw_circle(bar_x + filled, bar_y + bar_h / 2, cat_scale(7),
                        (cat_draw_color){ 255, 255, 255, 240 });
    }

    /* Title, clock, output and the seek rail stay visible under a submenu:
       they are the context you need while changing a setting. Everything below
       the rail is replaced rather than covered. */
    if (browser->osd.state == VFH_OSD_SUBMENU) {
        SDL_Rect sheet = { panel.x + metrics.gap, cursor, panel.w - metrics.gap * 2,
                           (panel.y + panel.h - metrics.inner_pad) - cursor };
        vfh_draw_osd_submenu(browser, sheet);
        return;
    }

    vfh_draw_transport(browser, screen_w / 2, cursor + metrics.transport_h / 2,
                       metrics.transport_size, pinned);
    cursor += metrics.transport_h + metrics.gap;

    vfh_osd_focus row_two[5];
    int row_count = vfh_osd_row_two_focuses(&browser->osd, row_two, 5);
    int row_y = cursor;
    int row_h = metrics.row_h;
    int row_gap = cat_scale(5);
    int row_w = (bar_w - row_gap * (row_count - 1)) / row_count;
    for (int i = 0; i < row_count; i++) {
        int x = bar_x + i * (row_w + row_gap);
        vfh_draw_osd_choice(x, row_y, row_w, row_h, vfh_osd_focus_label(row_two[i]),
                            pinned && browser->osd.focus == row_two[i]);
    }

    /* Status and hint share one baseline, measured up from the panel's bottom
       edge so both stay on the scrim. They used to sit 12 units apart, which
       read as misaligned and pushed the hint off the panel onto the video. */
    int bottom_y = panel.y + panel.h - metrics.inner_pad - metrics.footer_h;
    const char *trailing = NULL;
    cat_draw_color trailing_color = { 214, 214, 226, 235 };
    if (!vfh_player_has_audio(browser->player)) {
        trailing = "No audio track";
    } else if (pinned) {
        trailing = "A Activate  •  B Close";
    }
    int trailing_w = trailing ? cat_measure_text(small, trailing) : 0;
    if (trailing)
        cat_draw_text(small, trailing, screen_w - pad * 2 - trailing_w, bottom_y,
                      trailing_color);

    const char *status = browser->message[0] ? browser->message :
                         pinned ? vfh_osd_focus_label(browser->osd.focus) : "Y Controls";
    int status_w = bar_w - (trailing ? trailing_w + cat_scale(12) : 0);
    if (status_w > 0)
        cat_draw_text_ellipsized(small, status, bar_x, bottom_y,
                                 (cat_draw_color){ 205, 205, 215, 220 }, status_w);
}

/* Subtitles sit above the OSD when it is up, and just above the bottom edge
   otherwise, so the two never overlap. Drawn with an outline pass because a
   plain white line vanishes over a bright frame -- snow, sky, credits. */
static void vfh_release_subtitle_texture(vfh_browser *browser) {
    if (!browser) return;
    if (browser->subtitle_texture) SDL_DestroyTexture(browser->subtitle_texture);
    browser->subtitle_texture = NULL;
    browser->subtitle_text[0] = '\0';
    browser->subtitle_font = NULL;
    browser->subtitle_max_w = browser->subtitle_w = browser->subtitle_h = 0;
    browser->subtitle_inset = 0;
}

/* Rasterise one cue into a single texture.
 *
 * The outline is eight offset copies of a wrapped block, and Catastrophe
 * re-runs its word wrap on every call - measuring a growing prefix per word -
 * so drawing this straight to the screen cost ten wrap passes per frame. That
 * is enough main-thread work, next to the frame upload, to miss the vsync
 * deadline and halve the presented frame rate. Cues change every few seconds,
 * so the whole block is rendered once and then blitted. */
static bool vfh_build_subtitle_texture(vfh_browser *browser, const char *text,
                                       TTF_Font *font, int max_w) {
    vfh_release_subtitle_texture(browser);
    if (!text || !text[0] || !font || max_w <= 0) return false;
    int height = cat_measure_wrapped_text_height(font, text, max_w);
    if (height <= 0) return false;
    int inset = cat_scale(2) > 0 ? cat_scale(2) : 1;
    int width = max_w + inset * 2;
    height += inset * 2;

    SDL_Renderer *renderer = cat_get_renderer();
    SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_TARGET, width, height);
    if (!target) return false;
    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);

    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    if (SDL_SetRenderTarget(renderer, target) != 0) {
        SDL_DestroyTexture(target);
        return false;
    }
    Uint8 r, g, b, a;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    /* Paid once per cue, so the full eight-direction outline stays: it is what
       keeps captions legible over snow, sky and credits. */
    cat_draw_color shadow = { 0, 0, 0, 210 };
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            if (!dx && !dy) continue;
            cat_draw_text_wrapped(font, text, inset + dx * inset, inset + dy * inset,
                                  max_w, shadow, CAT_ALIGN_CENTER);
        }
    cat_draw_text_wrapped(font, text, inset, inset, max_w,
                          (cat_draw_color){ 255, 255, 255, 255 }, CAT_ALIGN_CENTER);
    SDL_SetRenderTarget(renderer, previous);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);

    browser->subtitle_texture = target;
    snprintf(browser->subtitle_text, sizeof(browser->subtitle_text), "%s", text);
    browser->subtitle_font = font;
    browser->subtitle_max_w = max_w;
    browser->subtitle_w = width;
    browser->subtitle_h = height;
    browser->subtitle_inset = inset;
    return true;
}

static void vfh_draw_subtitles(vfh_browser *browser) {
    if (!browser->subtitles || !browser->subtitles_on) return;
    const char *text = vfh_srt_text_at(browser->subtitles,
                                       vfh_player_position(browser->player));
    if (!text || !text[0]) return;

    TTF_Font *font = cat_get_font(CAT_FONT_MEDIUM);
    int screen_w = cat_get_screen_width();
    int screen_h = cat_get_screen_height();
    int margin = cat_scale(28);
    int max_w = screen_w - margin * 2;
    if (!browser->subtitle_texture || browser->subtitle_font != font ||
        browser->subtitle_max_w != max_w ||
        strcmp(browser->subtitle_text, text) != 0) {
        if (!vfh_build_subtitle_texture(browser, text, font, max_w)) return;
    }

    /* Only the vertical placement follows the OSD, so a moving caption still
       reuses the same texture. */
    /* Clear the panel itself rather than a copy of its height, so the caption
       keeps its gap if the panel ever grows. */
    int bottom = vfh_playback_osd_visible(browser)
        ? screen_h - vfh_osd_layout().height - cat_scale(16 + 8)
        : screen_h - cat_scale(24);
    int y = bottom - (browser->subtitle_h - browser->subtitle_inset * 2);
    if (y < browser->subtitle_inset) y = browser->subtitle_inset;
    SDL_Rect destination = { margin - browser->subtitle_inset, y - browser->subtitle_inset,
                             browser->subtitle_w, browser->subtitle_h };
    SDL_RenderCopy(cat_get_renderer(), browser->subtitle_texture, NULL, &destination);
}

static void vfh_draw_playback(vfh_browser *browser) {
    SDL_Renderer *renderer = cat_get_renderer();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    vfh_upload_due_video(browser);
    if (browser->video_texture && browser->video_width > 0 && browser->video_height > 0) {
        SDL_Rect destination = vfh_video_rect(browser);
        SDL_RenderCopy(renderer, browser->video_texture, NULL, &destination);
    } else {
        cat_draw_text(cat_get_font(CAT_FONT_LARGE), "Buffering video…",
                      cat_scale(18), cat_scale(18), (cat_draw_color){ 220, 220, 220, 255 });
    }
    vfh_draw_subtitles(browser);
    if (vfh_playback_osd_visible(browser)) vfh_draw_osd(browser);
    else if (vfh_player_is_paused(browser->player))
        cat_draw_text(cat_get_font(CAT_FONT_LARGE), "Paused",
                      cat_scale(18), cat_scale(18), (cat_draw_color){ 255, 255, 255, 255 });
    cat_request_frame_in(16);
}

static void vfh_start_playback(vfh_browser *browser, const vfh_entry *entry) {
    if (!entry || entry->kind != VFH_ENTRY_VIDEO) return;
    vfh_entry requested = *entry;
    vfh_queue_item requested_identity = { 0 };
    bool has_requested_identity = vfh_entry_identity(browser, entry, &requested_identity);
    double resume_at = has_requested_identity
        ? vfh_resume_get_identity(&(vfh_resume_identity){
              .content_kind = requested_identity.content_kind,
              .source_index = requested_identity.source_index,
              .relative_path = requested_identity.relative_path,
          }, entry->path)
        : vfh_resume_get(entry->path);
    /* Catalog I/O stays out of playback so it cannot contend with MPP/audio.
       Cancellation is transactional, so this only waits for the worker to
       reach its next directory/probe boundary rather than completing a whole
       cold-card scan before the selected film can start. */
    vfh_rescan_cancel(browser, true);
    entry = &requested;
    /* Read before stop_playback, which checkpoints the *outgoing* film. */
    vfh_stop_playback(browser);
    browser->player = vfh_player_create();
    if (!browser->player) {
        vfh_set_message(browser, "Unable to allocate the playback core.");
        return;
    }
    if (browser->audio_output[0])
        vfh_player_set_audio_output(browser->player, browser->audio_output);
    char error[256];
    if (!vfh_player_open(browser->player, entry->path, error, (int)sizeof(error))) {
        vfh_player_destroy(browser->player);
        browser->player = NULL;
        vfh_set_message(browser, error[0] ? error : "Unable to start playback.");
        return;
    }
    vfh_clear_poster(browser);
    snprintf(browser->playing_path, sizeof(browser->playing_path), "%.*s",
             (int)sizeof(browser->playing_path) - 1, entry->path);
    snprintf(browser->playing_name, sizeof(browser->playing_name), "%.*s",
             (int)sizeof(browser->playing_name) - 1, entry->name);
    browser->playing_identity = requested_identity;
    browser->playing_identity_valid = has_requested_identity;
    browser->playback_finished = false;
    browser->message[0] = '\0';
    browser->pixel_aspect = 1.0;
    vfh_osd_init(&browser->osd);
    browser->resume_saved_ms = SDL_GetTicks();
    /* Subtitles default to on when a sidecar exists: someone who put an .srt
       next to the film wants to see it without hunting for a toggle. */
    browser->subtitles = vfh_srt_load_for(entry->path);
    browser->subtitles_on = browser->subtitles != NULL;
    if (browser->subtitles)
        cat_log("videofromhell: subtitles: %d cues from %s",
                vfh_srt_count(browser->subtitles), vfh_srt_source(browser->subtitles));
    vfh_refresh_osd_capabilities(browser);
    /* Resume is automatic.  A watched film has already had its stored point
       cleared at the 90% checkpoint, so only a usable continuation reaches
       this path. */
    if (resume_at > 0.0) vfh_player_seek_relative(browser->player, resume_at);
    /* Hold the backlight for the length of the film; jawakad's auto-sleep keys
       off input idle, which a viewer does not generate. */
    vfh_inhibit_acquire(entry->name);
    vfh_playback_osd_flash(browser);
    cat_log("videofromhell: playback started: %s", entry->path);
}

static bool vfh_queue_entry(vfh_browser *browser, const vfh_entry *entry, bool play_next) {
    if (!browser || !entry || entry->kind != VFH_ENTRY_VIDEO || entry->catalog_index < 0 ||
        (size_t)entry->catalog_index >= browser->catalog.count) return false;
    const vfh_library_item *library_item = &browser->catalog.items[entry->catalog_index];
    vfh_queue_item item = {
        .content_kind = library_item->content_kind,
        .source_index = library_item->source_index,
    };
    snprintf(item.relative_path, sizeof(item.relative_path), "%s", library_item->relative_path);
    bool ok = play_next ? vfh_queue_play_next(&browser->queue, &item, 0)
                        : vfh_queue_append(&browser->queue, &item);
    if (ok) (void)vfh_queue_save(&browser->queue);
    return ok;
}

static bool vfh_queue_item_equal(const vfh_queue_item *queue_item,
                                 const vfh_library_item *library_item) {
    return queue_item && library_item && queue_item->content_kind == library_item->content_kind &&
           queue_item->source_index == library_item->source_index &&
           strcmp(queue_item->relative_path, library_item->relative_path) == 0;
}

static const vfh_library_item *vfh_current_library_item(const vfh_browser *browser) {
    if (!browser || !browser->playing_path[0]) return NULL;
    for (size_t i = 0; i < browser->catalog.count; i++) {
        char path[VFH_SOURCE_PATH_MAX];
        if (vfh_library_resolve_path(&browser->catalog.items[i], &browser->sources,
                                     browser->recordings_path, path, sizeof(path)) &&
            strcmp(path, browser->playing_path) == 0) return &browser->catalog.items[i];
    }
    return NULL;
}

static bool vfh_entry_from_library(const vfh_browser *browser, const vfh_library_item *item,
                                   vfh_entry *entry) {
    if (!browser || !item || !entry || !item->available) return false;
    vfh_entry result = {
        .kind = VFH_ENTRY_VIDEO,
        .source_index = item->source_index,
        .available = true,
        .catalog_index = (int)(item - browser->catalog.items),
        .content_kind = item->content_kind,
        .duration = item->duration,
        .has_duration = item->duration > 0.0,
    };
    if (!vfh_library_resolve_path(item, &browser->sources, browser->recordings_path,
                                  result.path, sizeof(result.path))) return false;
    snprintf(result.name, sizeof(result.name), "%s", item->display_title);
    *entry = result;
    return true;
}

typedef enum {
    VFH_QUEUE_NEIGHBOR_NONE,
    VFH_QUEUE_NEIGHBOR_UNAVAILABLE,
    VFH_QUEUE_NEIGHBOR_READY,
} vfh_queue_neighbor_status;

static vfh_queue_neighbor_status vfh_queue_neighbor(const vfh_browser *browser, int delta,
                                                     vfh_entry *entry,
                                                     const vfh_queue_item **queued_item) {
    if (queued_item) *queued_item = NULL;
    if (!browser || !browser->queue.count) return VFH_QUEUE_NEIGHBOR_NONE;
    const vfh_library_item *current = vfh_current_library_item(browser);
    if (!current) return VFH_QUEUE_NEIGHBOR_NONE;

    long target = -1;
    for (size_t index = 0; index < browser->queue.count; index++) {
        if (vfh_queue_item_equal(&browser->queue.items[index], current)) {
            target = (long)index + delta;
            break;
        }
    }
    if (target < 0 || (size_t)target >= browser->queue.count) return VFH_QUEUE_NEIGHBOR_NONE;

    const vfh_queue_item *queued = &browser->queue.items[target];
    if (queued_item) *queued_item = queued;
    const vfh_library_item *item = vfh_library_find(&browser->catalog, queued->content_kind,
                                                     queued->source_index, queued->relative_path);
    if (!item || !vfh_entry_from_library(browser, item, entry))
        return VFH_QUEUE_NEIGHBOR_UNAVAILABLE;
    return VFH_QUEUE_NEIGHBOR_READY;
}

static void vfh_queue_unavailable_message(const vfh_browser *browser,
                                          const vfh_queue_item *queued_item,
                                          char *out, size_t out_size) {
    const char *source = "its source";
    if (queued_item && queued_item->content_kind == VFH_CONTENT_RECORDING) {
        source = "Recorded Gameplay";
    } else if (queued_item && queued_item->source_index >= 0 &&
               queued_item->source_index < browser->sources.count) {
        source = browser->sources.items[queued_item->source_index].label;
    }
    snprintf(out, out_size, "Next queued video is unavailable — %s is missing.", source);
}

static bool vfh_play_queue_neighbor(vfh_browser *browser, int delta) {
    vfh_entry entry;
    const vfh_queue_item *queued_item = NULL;
    vfh_queue_neighbor_status status = vfh_queue_neighbor(browser, delta, &entry, &queued_item);
    if (status == VFH_QUEUE_NEIGHBOR_NONE) return false;
    if (status == VFH_QUEUE_NEIGHBOR_UNAVAILABLE) {
        char message[160];
        vfh_queue_unavailable_message(browser, queued_item, message, sizeof(message));
        vfh_set_message(browser, message);
        vfh_playback_osd_flash(browser);
        return true;
    }
    vfh_start_playback(browser, &entry);
    return true;
}

static bool vfh_confirm_clear_queue(void) {
    cat_selection_option options[] = {
        { .label = "Keep queue", .value = "keep" },
        { .label = "Clear queue", .value = "clear" },
    };
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = "Choose", .is_confirm = true },
        { .button = CAT_BTN_B, .label = "Keep" },
    };
    cat_selection_result result = { 0 };
    int selection = cat_selection("Clear every queued video?", options, 2, footer, 2, &result) == CAT_OK
                  ? result.selected_index : 0;
    return selection == 1;
}

static void vfh_show_queue(vfh_browser *browser) {
    if (!browser || !browser->queue.count) { vfh_set_message(browser, "Queue is empty."); return; }
    cat_list_item rows[VFH_QUEUE_MAX_ITEMS];
    char labels[VFH_QUEUE_MAX_ITEMS][VFH_NAME_MAX];
    for (size_t i = 0; i < browser->queue.count; i++) {
        const vfh_library_item *item = vfh_library_find(&browser->catalog,
            browser->queue.items[i].content_kind, browser->queue.items[i].source_index,
            browser->queue.items[i].relative_path);
        snprintf(labels[i], sizeof(labels[i]), "%s", item ? item->display_title : browser->queue.items[i].relative_path);
        rows[i] = (cat_list_item){ .label = labels[i], .metadata = (const char *)&browser->queue.items[i], .disabled = !item || !item->available };
    }
    cat_footer_item footer[] = { { .button = CAT_BTN_A, .label = "Play", .is_confirm = true },
                                 { .button = CAT_BTN_X, .label = "Remove" },
                                 { .button = CAT_BTN_Y, .label = "Reorder" },
                                 { .button = CAT_BTN_SELECT, .label = "Clear" },
                                 { .button = CAT_BTN_B, .label = "Back" } };
    cat_list_opts opts = cat_list_default_opts("Queue", rows, (int)browser->queue.count);
    opts.footer = footer; opts.footer_count = 5; opts.reorder_button = CAT_BTN_Y;
    opts.action_button = CAT_BTN_X; opts.secondary_action_button = CAT_BTN_SELECT;
    cat_list_result result = { 0 };
    (void)cat_list(&opts, &result);
    vfh_queue reordered; vfh_queue_init(&reordered);
    for (size_t i = 0; i < browser->queue.count; i++) (void)vfh_queue_append(&reordered, (const vfh_queue_item *)rows[i].metadata);
    browser->queue = reordered;
    if (result.action == CAT_ACTION_TRIGGERED && result.selected_index >= 0) {
        (void)vfh_queue_remove(&browser->queue, (size_t)result.selected_index);
    } else if (result.action == CAT_ACTION_SECONDARY_TRIGGERED && vfh_confirm_clear_queue()) {
        vfh_queue_clear(&browser->queue);
    }
    (void)vfh_queue_save(&browser->queue);
    if (result.action == CAT_ACTION_SELECTED && result.selected_index >= 0 &&
        (size_t)result.selected_index < browser->queue.count) {
        const vfh_queue_item *queued = &browser->queue.items[result.selected_index];
        const vfh_library_item *item = vfh_library_find(&browser->catalog, queued->content_kind,
                                                         queued->source_index, queued->relative_path);
        if (!item || !item->available) {
            vfh_set_message(browser, "Queued video is unavailable.");
            return;
        }
        vfh_entry entry;
        if (vfh_entry_from_library(browser, item, &entry)) vfh_start_playback(browser, &entry);
    }
}

static void vfh_activate_entry(vfh_browser *browser) {
    if (browser->list.cursor < 0 || browser->list.cursor >= browser->entry_count) return;
    vfh_entry *entry = &browser->entries[browser->list.cursor];
    if (entry->kind == VFH_ENTRY_PARENT) {
        vfh_parent_directory(browser);
    } else if (entry->kind == VFH_ENTRY_DIRECTORY) {
        vfh_store_current_list(browser);
        browser->folder_recordings = entry->recordings_folder;
        snprintf(browser->folder_relative, sizeof(browser->folder_relative), "%.767s",
                 entry->path);
        vfh_scan_catalog(browser);
    } else {
        if (!entry->available) {
            const char *label = entry->content_kind == VFH_CONTENT_RECORDING ? "Recorded Gameplay"
                              : (entry->source_index >= 0 && entry->source_index < browser->sources.count
                                 ? browser->sources.items[entry->source_index].label : "This source");
            char problem[128];
            snprintf(problem, sizeof(problem), "%s is unavailable.", label);
            vfh_set_message(browser, problem);
            return;
        }
        vfh_start_playback(browser, entry);
    }
}

static void vfh_set_tab(vfh_browser *browser, int delta) {
    if (!browser) return;
    vfh_store_current_list(browser);
    browser->tab = (vfh_library_tab)(((int)browser->tab + delta + VFH_TAB_COUNT) %
                                     VFH_TAB_COUNT);
    vfh_scan_catalog(browser);
}

/* Step to the next/previous video in the folder the current one came from.
   Directories and the parent row are skipped; the ends do not wrap, because
   silently looping back to the first file reads as a bug during playback. */
static void vfh_play_adjacent(vfh_browser *browser, int delta) {
    if (!browser->player || !browser->playing_path[0]) return;
    if (vfh_play_queue_neighbor(browser, delta)) return;
    int current = -1;
    for (int i = 0; i < browser->entry_count; i++) {
        if (browser->entries[i].kind == VFH_ENTRY_VIDEO &&
            strcmp(browser->entries[i].path, browser->playing_path) == 0) {
            current = i;
            break;
        }
    }
    if (current < 0) return;
    for (int i = current + delta; i >= 0 && i < browser->entry_count; i += delta) {
        if (browser->entries[i].kind != VFH_ENTRY_VIDEO) continue;
        browser->list.cursor = i;
        vfh_start_playback(browser, &browser->entries[i]);
        return;
    }
    vfh_playback_osd_flash(browser);   /* at an end: show where we are rather than nothing */
}

static void vfh_toggle_play_pause(vfh_browser *browser) {
    bool pausing = !vfh_player_is_paused(browser->player);
    vfh_player_set_paused(browser->player, pausing);
    /* A paused film is not being watched: let the normal idle timeout blank
       the screen again. */
    if (pausing) {
        vfh_checkpoint_playing(browser, vfh_player_position(browser->player),
                               vfh_player_duration(browser->player));
        browser->resume_saved_ms = SDL_GetTicks();
        vfh_inhibit_release();
    } else {
        vfh_inhibit_acquire(browser->playing_name);
    }
    vfh_playback_osd_flash(browser);
}

static void vfh_handle_eof(vfh_browser *browser) {
    if (!browser || !browser->player) return;
    /* EOF is definitive even if the decoder's final timestamp falls short of
       the nominal duration. Checkpoint it before presenting a blocking choice. */
    browser->playback_finished = true;
    vfh_mark_playing_watched(browser, vfh_player_duration(browser->player));

    cat_selection_option options[3];
    char next_label[VFH_NAME_MAX + 16];
    char heading[192] = "Playback finished.";
    int play_next_index = -1;
    int replay_index;
    vfh_entry next_entry;
    const vfh_queue_item *queued_item = NULL;
    vfh_queue_neighbor_status next_status = vfh_queue_neighbor(browser, +1, &next_entry,
                                                                 &queued_item);
    int option_count = 0;
    if (next_status == VFH_QUEUE_NEIGHBOR_READY) {
        snprintf(next_label, sizeof(next_label), "Play Next: %s", next_entry.name);
        options[option_count] = (cat_selection_option){ .label = next_label, .value = "next" };
        play_next_index = option_count++;
    } else if (next_status == VFH_QUEUE_NEIGHBOR_UNAVAILABLE) {
        vfh_queue_unavailable_message(browser, queued_item, heading, sizeof(heading));
    }
    replay_index = option_count;
    options[option_count++] = (cat_selection_option){ .label = "Replay", .value = "replay" };
    options[option_count++] = (cat_selection_option){ .label = "Return to Library", .value = "return" };
    cat_footer_item footer[] = { { .button = CAT_BTN_A, .label = "Choose", .is_confirm = true },
                                 { .button = CAT_BTN_B, .label = "Return" } };
    cat_selection_result result = { 0 };
    int selected = cat_selection(heading, options, option_count, footer, 2, &result) == CAT_OK
        ? result.selected_index : option_count - 1;
    if (selected == play_next_index) {
        vfh_start_playback(browser, &next_entry);
        return;
    }
    if (selected == replay_index) {
        browser->playback_finished = false;
        vfh_player_seek_relative(browser->player, -vfh_player_position(browser->player));
        vfh_player_set_paused(browser->player, false);
        browser->resume_saved_ms = SDL_GetTicks();
        vfh_playback_osd_flash(browser);
        return;
    }
    vfh_stop_playback(browser);
    vfh_set_message(browser, "Playback finished.");
}

static void vfh_set_subtitles_enabled(vfh_browser *browser, bool enabled) {
    if (!browser->subtitles) {
        vfh_set_message(browser, "No external subtitles for this video.");
        vfh_playback_osd_flash(browser);
        return;
    }
    browser->subtitles_on = enabled;
    vfh_set_message(browser, enabled ? "Subtitles on" : "Subtitles off");
    vfh_playback_osd_flash(browser);
}

static void vfh_change_aspect(vfh_browser *browser, int delta) {
    int mode = browser->aspect_mode + delta;
    while (mode < 0) mode += VFH_ASPECT_COUNT;
    browser->aspect_mode = mode % VFH_ASPECT_COUNT;
    char message[64];
    snprintf(message, sizeof(message), "Aspect: %s", vfh_aspect_label(browser->aspect_mode));
    vfh_set_message(browser, message);
    vfh_playback_osd_flash(browser);
}

static const vfh_library_item *vfh_catalog_item_for_path(const vfh_browser *browser,
                                                         const char *path) {
    if (!browser || !path || !path[0]) return NULL;
    for (size_t i = 0; i < browser->catalog.count; i++) {
        const vfh_library_item *item = &browser->catalog.items[i];
        char item_path[VFH_SOURCE_PATH_MAX];
        if (vfh_library_resolve_path(item, &browser->sources, browser->recordings_path,
                                     item_path, sizeof(item_path)) && strcmp(item_path, path) == 0)
            return item;
    }
    return NULL;
}

static void vfh_show_video_information(vfh_browser *browser,
                                       const vfh_library_item *item,
                                       const char *path, bool playing) {
    if (!browser || !path || !path[0]) return;
    char duration[32];
    char source[64] = "Unknown source";
    char relative[VFH_LIBRARY_RELATIVE_PATH_MAX] = "Not indexed";
    char resolution[32] = "Available during playback";
    vfh_player_media_info media = { 0 };
    double seconds = item ? item->duration : 0.0;
    if (playing && browser->player) {
        seconds = vfh_player_duration(browser->player);
        (void)vfh_player_get_media_info(browser->player, &media);
    }
    vfh_media_format_duration(seconds, duration, (int)sizeof(duration));
    if (playing && media.video_width > 0 && media.video_height > 0)
        snprintf(resolution, sizeof(resolution), "%d × %d", media.video_width, media.video_height);
    if (item) {
        snprintf(relative, sizeof(relative), "%s", item->relative_path);
        if (item->content_kind == VFH_CONTENT_RECORDING) {
            snprintf(source, sizeof(source), "%s", "Recorded Gameplay");
        } else if (item->source_index >= 0 && item->source_index < browser->sources.count) {
            snprintf(source, sizeof(source), "%s", browser->sources.items[item->source_index].label);
        }
    }
    const char *title = item ? item->display_title : browser->playing_name;
    const char *container = item && item->container[0] ? item->container :
                            media.container[0] ? media.container : "Unknown";
    const char *video = item && item->video_codec[0] ? item->video_codec :
                        media.video_codec[0] ? media.video_codec : "Unknown";
    const char *audio = item && item->audio_codec[0] ? item->audio_codec :
                        media.audio_codec[0] ? media.audio_codec : "None";
    cat_detail_info_pair pairs[] = {
        { "Title", title },
        { "File", vfh_basename(path) },
        { "Source", source },
        { "Relative path", relative },
        { "Path", path },
        { "Duration", duration },
        { "Container", container },
        { "Video", video },
        { "Resolution", resolution },
        { "Audio", audio },
        { "Subtitles", playing && browser->subtitles ?
              (browser->subtitles_on ? "On" : "Off") : "None" },
        { "Aspect", playing ? vfh_aspect_label(browser->aspect_mode) : "Playback only" },
        { "Output", playing ? vfh_audio_output_label(vfh_browser_audio_output(browser)) :
              "Playback only" },
    };
    cat_detail_section sections[] = {
        { .type = CAT_SECTION_INFO, .title = "Media", .info_pairs = pairs,
          .info_count = (int)(sizeof(pairs) / sizeof(pairs[0])) },
    };
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Back" },
    };
    cat_detail_opts opts = {
        .title = "Video Information",
        .sections = sections,
        .section_count = (int)(sizeof(sections) / sizeof(sections[0])),
        .footer = footer,
        .footer_count = (int)(sizeof(footer) / sizeof(footer[0])),
        .show_section_separator = true,
    };
    cat_detail_result result = { 0 };
    (void)cat_detail_screen(&opts, &result);
}

static void vfh_show_browser_item_actions(vfh_browser *browser) {
    if (!browser || browser->list.cursor < 0 || browser->list.cursor >= browser->entry_count)
        return;
    vfh_entry *entry = &browser->entries[browser->list.cursor];
    if (entry->kind != VFH_ENTRY_VIDEO || entry->catalog_index < 0 ||
        (size_t)entry->catalog_index >= browser->catalog.count) {
        vfh_set_message(browser, "Actions are available for videos only.");
        return;
    }
    const vfh_library_item *item = &browser->catalog.items[entry->catalog_index];
    cat_selection_option options[5] = {
        { .label = "Play Now", .value = "play" },
        { .label = "Play Next", .value = "next" },
        { .label = "Add to Queue", .value = "queue" },
        { .label = "Video Information", .value = "info" },
    };
    int option_count = 4;
    int remove_history = -1;
    if (browser->tab == VFH_TAB_CONTINUE && entry->resume_seconds > 0.0) {
        remove_history = option_count;
        options[option_count++] = (cat_selection_option){
            .label = "Remove from History", .value = "remove-history",
        };
    }
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = "Choose", .is_confirm = true },
        { .button = CAT_BTN_B, .label = "Back" },
    };
    cat_selection_result result = { 0 };
    int selected = cat_selection(item->display_title, options, option_count, footer, 2, &result) == CAT_OK
                 ? result.selected_index : -1;
    if (selected == 0) {
        vfh_activate_entry(browser);
    } else if (selected == 1) {
        if (vfh_queue_entry(browser, entry, true)) vfh_set_message(browser, "Added to play next.");
        else vfh_set_message(browser, "Unable to add this video to play next.");
    } else if (selected == 2) {
        if (vfh_queue_entry(browser, entry, false)) vfh_set_message(browser, "Added to queue.");
        else vfh_set_message(browser, "Unable to add this video to the queue.");
    } else if (selected == 3) {
        vfh_show_video_information(browser, item, entry->path, false);
    } else if (selected == remove_history) {
        vfh_resume_identity identity = {
            .content_kind = item->content_kind,
            .source_index = item->source_index,
            .relative_path = item->relative_path,
        };
        if (vfh_resume_remove_identity(&identity, entry->path)) {
            vfh_scan_catalog(browser);
            vfh_set_message(browser, "Removed from Continue Watching.");
        } else {
            vfh_set_message(browser, "Unable to update playback history.");
        }
    }
}

static void vfh_show_chapters(vfh_browser *browser) {
    if (!browser || !browser->player) return;
    int count = vfh_player_chapter_count(browser->player);
    if (count <= 0) {
        vfh_set_message(browser, "This video has no chapters.");
        return;
    }
    cat_selection_option options[64];
    char labels[64][VFH_PLAYER_CHAPTER_TITLE_MAX + 24];
    for (int index = 0; index < count; index++) {
        vfh_player_chapter chapter;
        if (!vfh_player_chapter_get(browser->player, index, &chapter)) {
            count = index;
            break;
        }
        char timestamp[32];
        vfh_media_format_duration(chapter.start_seconds, timestamp, (int)sizeof(timestamp));
        snprintf(labels[index], sizeof(labels[index]), "%.20s — %.120s", timestamp, chapter.title);
        options[index] = (cat_selection_option){ .label = labels[index], .value = labels[index] };
    }
    if (count <= 0) {
        vfh_set_message(browser, "This video has no usable chapters.");
        return;
    }
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = "Seek", .is_confirm = true },
        { .button = CAT_BTN_B, .label = "Back" },
    };
    cat_selection_result result = { 0 };
    int selected = cat_selection("Chapters", options, count, footer, 2, &result) == CAT_OK
                 ? result.selected_index : -1;
    if (selected < 0 || selected >= count) return;
    vfh_player_chapter chapter;
    if (!vfh_player_chapter_get(browser->player, selected, &chapter)) return;
    vfh_player_seek_relative(browser->player,
                             chapter.start_seconds - vfh_player_position(browser->player));
    char message[160];
    snprintf(message, sizeof(message), "Chapter: %s", chapter.title);
    vfh_set_message(browser, message);
    vfh_playback_osd_flash(browser);
}

static void vfh_activate_osd_focus(vfh_browser *browser) {
    if (browser->osd.state != VFH_OSD_PINNED) return;
    switch (browser->osd.focus) {
        case VFH_OSD_FOCUS_PREVIOUS:
            vfh_play_adjacent(browser, -1);
            break;
        case VFH_OSD_FOCUS_REWIND:
            vfh_player_seek_relative(browser->player, -10.0);
            vfh_playback_osd_flash(browser);
            break;
        case VFH_OSD_FOCUS_PLAY_PAUSE:
            vfh_toggle_play_pause(browser);
            break;
        case VFH_OSD_FOCUS_FORWARD:
            vfh_player_seek_relative(browser->player, +10.0);
            vfh_playback_osd_flash(browser);
            break;
        case VFH_OSD_FOCUS_NEXT:
            vfh_play_adjacent(browser, +1);
            break;
        case VFH_OSD_FOCUS_QUEUE:
            vfh_open_osd_submenu(browser, VFH_OSD_SUBMENU_QUEUE);
            break;
        case VFH_OSD_FOCUS_SUBTITLES:
            vfh_open_osd_submenu(browser, VFH_OSD_SUBMENU_SUBTITLES);
            break;
        case VFH_OSD_FOCUS_ASPECT:
            vfh_open_osd_submenu(browser, VFH_OSD_SUBMENU_ASPECT);
            break;
        case VFH_OSD_FOCUS_MORE:
            vfh_open_osd_submenu(browser, VFH_OSD_SUBMENU_MORE);
            break;
        case VFH_OSD_FOCUS_INFORMATION:
            vfh_show_video_information(browser,
                                       vfh_catalog_item_for_path(browser,
                                                                 browser->playing_path),
                                       browser->playing_path, true);
            break;
        case VFH_OSD_FOCUS_PROGRESS:
            vfh_set_message(browser, "Use Left/Right to seek.");
            break;
    }
}

/* Aspect and Subtitles apply live, because seeing the change is the whole
 * point of them - a deferred form cannot show you the framing. So the submenu
 * is a preview, and the two exits mean different things: A keeps what you are
 * looking at, B puts back what you started with. Anything else made A a second
 * way to cycle the options, which is what made the menu confusing. */
static void vfh_open_osd_submenu(vfh_browser *browser, vfh_osd_submenu submenu) {
    if (!vfh_osd_open_submenu(&browser->osd, submenu)) return;
    browser->submenu_entry_aspect = browser->aspect_mode;
    browser->submenu_entry_subtitles_on = browser->subtitles_on;
}

/* True when the submenu previews a value rather than launching a screen. */
static bool vfh_osd_submenu_previews(vfh_osd_submenu submenu) {
    return submenu == VFH_OSD_SUBMENU_SUBTITLES || submenu == VFH_OSD_SUBMENU_ASPECT;
}

static void vfh_cancel_osd_submenu(vfh_browser *browser) {
    if (browser->osd.state != VFH_OSD_SUBMENU) return;
    if (browser->osd.submenu == VFH_OSD_SUBMENU_ASPECT &&
        browser->aspect_mode != browser->submenu_entry_aspect) {
        browser->aspect_mode = browser->submenu_entry_aspect;
        vfh_set_message(browser, "Aspect unchanged");
    } else if (browser->osd.submenu == VFH_OSD_SUBMENU_SUBTITLES &&
               browser->subtitles_on != browser->submenu_entry_subtitles_on) {
        browser->subtitles_on = browser->submenu_entry_subtitles_on;
        vfh_set_message(browser, "Subtitles unchanged");
    }
}

static void vfh_activate_osd_submenu(vfh_browser *browser) {
    switch (browser->osd.submenu) {
        case VFH_OSD_SUBMENU_SUBTITLES:
        case VFH_OSD_SUBMENU_ASPECT:
            /* Already applied; A only accepts and leaves. */
            (void)vfh_osd_back(&browser->osd);
            vfh_playback_osd_flash(browser);
            break;
        case VFH_OSD_SUBMENU_QUEUE:
            vfh_show_queue(browser);
            break;
        case VFH_OSD_SUBMENU_MORE:
            vfh_show_chapters(browser);
            break;
        default:
            break;
    }
}

static bool vfh_move_osd_submenu(vfh_browser *browser, int horizontal) {
    if (browser->osd.state != VFH_OSD_SUBMENU || !horizontal) return false;
    switch (browser->osd.submenu) {
        case VFH_OSD_SUBMENU_SUBTITLES:
            vfh_set_subtitles_enabled(browser, horizontal > 0);
            return true;
        case VFH_OSD_SUBMENU_ASPECT:
            vfh_change_aspect(browser, horizontal);
            return true;
        default:
            return true;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    cat_config config = {
        .window_title = "Video From Hell",
        .log_path = cat_resolve_log_path("videofromhell"),
        .cpu_speed = CAT_CPU_SPEED_PERFORMANCE,
    };
    if (cat_init(&config) != CAT_OK) {
        fprintf(stderr, "Video From Hell: failed to initialise Catastrophe\n");
        return 1;
    }

    vfh_browser browser;
    memset(&browser, 0, sizeof(browser));
    browser.tab = VFH_TAB_RECENT;
    vfh_osd_init(&browser.osd);
    for (int i = 0; i < VFH_TAB_COUNT; i++)
        cat_list_state_init(&browser.tab_lists[i], 1);
    pthread_mutex_init(&browser.rescan.mutex, NULL);
    atomic_init(&browser.rescan.cancel, false);
    char source_error[256];
    if (!vfh_sources_resolve(&browser.sources, source_error, sizeof(source_error))) {
        vfh_sources_single_fallback(&browser.sources);
        vfh_set_message(&browser, source_error);
    }
    /* G1's catalog is intentionally independent of the legacy source-local
       browser. It gives warm launches a durable, merged identity cache while
       the FFmpeg duration probe runs only in the catalog worker. */
    vfh_library_init(&browser.catalog);
    vfh_queue_init(&browser.queue);
    vfh_status_monitor_init(&browser.audio_status);
    vfh_set_audio_output(&browser, vfh_browser_audio_output(&browser));
    if (!vfh_status_monitor_start(&browser.audio_status))
        vfh_set_message(&browser, "Live audio-output updates are unavailable.");
    (void)vfh_queue_load(&browser.queue);
    (void)vfh_library_load(&browser.catalog);
    if (!vfh_recordings_path_resolve(browser.recordings_path, sizeof(browser.recordings_path)))
        vfh_set_message(&browser, "Gameplay recordings path is unavailable.");
    browser.thumb_worker_ready = vfh_thumb_worker_init(&browser.thumbs);
    if (!browser.thumb_worker_ready)
        vfh_set_message(&browser, "Poster worker unavailable; browsing continues.");
    vfh_scan_catalog(&browser);
    /* Publish cached rows first. The worker performs the one filesystem scan
       and metadata refresh without blocking first interaction. */
    vfh_rescan_start(&browser);

    bool running = true;
    while (running) {
        vfh_refresh_audio_status(&browser);
        cat_input_event event;
        while (cat_poll_input(&event)) {
            if (browser.player) {
                /* L2/R2 are hold-to-seek, so they need the release edge too;
                   every OSD state keeps these direct transport shortcuts. */
                if (event.button == CAT_BTN_L2 || event.button == CAT_BTN_R2) {
                    bool *held = event.button == CAT_BTN_L2 ? &browser.seek_back_held
                                                            : &browser.seek_forward_held;
                    if (event.pressed && !event.repeated) {
                        *held = true;
                        browser.seek_last_ms = 0;   /* seek immediately on press */
                        vfh_playback_osd_flash(&browser);
                    } else if (!event.pressed) {
                        *held = false;
                    }
                    continue;
                }
                if (!event.pressed) continue;
                if (event.repeated && event.button != CAT_BTN_UP && event.button != CAT_BTN_DOWN &&
                    event.button != CAT_BTN_LEFT && event.button != CAT_BTN_RIGHT) continue;
                switch (event.button) {
                    case CAT_BTN_A:
                        if (browser.osd.state == VFH_OSD_SUBMENU)
                            vfh_activate_osd_submenu(&browser);
                        else if (browser.osd.state == VFH_OSD_PINNED)
                            vfh_activate_osd_focus(&browser);
                        else
                            vfh_toggle_play_pause(&browser);
                        break;
                    case CAT_BTN_X:
                        vfh_toggle_play_pause(&browser);
                        break;
                    case CAT_BTN_LEFT:
                        if (browser.osd.state == VFH_OSD_SUBMENU) {
                            (void)vfh_move_osd_submenu(&browser, -1);
                        } else if (browser.osd.state == VFH_OSD_PINNED) {
                            if (browser.osd.focus == VFH_OSD_FOCUS_PROGRESS) {
                                vfh_player_seek_relative(browser.player, -10.0);
                                vfh_playback_osd_flash(&browser);
                            } else {
                                vfh_osd_move(&browser.osd, -1, 0);
                            }
                        } else {
                            vfh_player_seek_relative(browser.player, -10.0);
                            vfh_playback_osd_flash(&browser);
                        }
                        break;
                    case CAT_BTN_RIGHT:
                        if (browser.osd.state == VFH_OSD_SUBMENU) {
                            (void)vfh_move_osd_submenu(&browser, +1);
                        } else if (browser.osd.state == VFH_OSD_PINNED) {
                            if (browser.osd.focus == VFH_OSD_FOCUS_PROGRESS) {
                                vfh_player_seek_relative(browser.player, +10.0);
                                vfh_playback_osd_flash(&browser);
                            } else {
                                vfh_osd_move(&browser.osd, +1, 0);
                            }
                        } else {
                            vfh_player_seek_relative(browser.player, +10.0);
                            vfh_playback_osd_flash(&browser);
                        }
                        break;
                    case CAT_BTN_UP:
                        if (browser.osd.state == VFH_OSD_PINNED)
                            vfh_osd_move(&browser.osd, 0, -1);
                        break;
                    case CAT_BTN_DOWN:
                        if (browser.osd.state == VFH_OSD_PINNED)
                            vfh_osd_move(&browser.osd, 0, +1);
                        break;
                    case CAT_BTN_Y:
                        vfh_osd_toggle_pinned(&browser.osd);
                        break;
                    case CAT_BTN_STICK:
                        vfh_change_aspect(&browser, +1);
                        break;
                    case CAT_BTN_SELECT:
                        vfh_set_subtitles_enabled(&browser, !browser.subtitles_on);
                        break;
                    case CAT_BTN_L1:
                        vfh_play_adjacent(&browser, -1);
                        break;
                    case CAT_BTN_R1:
                        vfh_play_adjacent(&browser, +1);
                        break;
                    case CAT_BTN_B:
                        vfh_cancel_osd_submenu(&browser);
                        if (!vfh_osd_back(&browser.osd)) {
                            vfh_stop_playback(&browser);
                            vfh_set_message(&browser, "Returned to the video library.");
                        }
                        break;
                    case CAT_BTN_MENU:
                        running = false;
                        break;
                    default:
                        break;
                }
                continue;
            }
            if (!event.pressed || event.repeated) continue;
            switch (event.button) {
                case CAT_BTN_L1:
                    vfh_set_tab(&browser, -1);
                    break;
                case CAT_BTN_R1:
                    vfh_set_tab(&browser, +1);
                    break;
                case CAT_BTN_UP:
                    cat_list_state_move(&browser.list, -1, browser.entry_count);
                    break;
                case CAT_BTN_DOWN:
                    cat_list_state_move(&browser.list, +1, browser.entry_count);
                    break;
                case CAT_BTN_LEFT:
                    cat_list_state_jump_letter(&browser.list, vfh_entry_label, &browser,
                                               browser.entry_count, -1);
                    break;
                case CAT_BTN_RIGHT:
                    cat_list_state_jump_letter(&browser.list, vfh_entry_label, &browser,
                                               browser.entry_count, +1);
                    break;
                case CAT_BTN_A:
                    vfh_activate_entry(&browser);
                    break;
                case CAT_BTN_X:
                    vfh_show_browser_item_actions(&browser);
                    break;
                case CAT_BTN_SELECT:
                    vfh_rescan_start(&browser);
                    break;
                case CAT_BTN_B:
                    vfh_parent_directory(&browser);
                    break;
                case CAT_BTN_MENU:
                    running = false;
                    break;
                default:
                    break;
            }
        }
        if (!browser.player) {
            (void)vfh_rescan_finish(&browser, false);
            if (browser.rescan_needs_resume && !browser.rescan.running)
                vfh_rescan_start(&browser);
        }
        if (browser.player) {
            Uint32 now = SDL_GetTicks();
            /* Hold-to-seek. Stepped on a timer rather than per frame so the
               rate is the same whether or not the decoder is keeping up. */
            if ((browser.seek_back_held || browser.seek_forward_held) &&
                now - browser.seek_last_ms >= VFH_SEEK_HOLD_EVERY_MS) {
                double step = browser.seek_forward_held ? VFH_SEEK_HOLD_STEP_S
                                                        : -VFH_SEEK_HOLD_STEP_S;
                vfh_player_seek_relative(browser.player, step);
                browser.seek_last_ms = now;
                vfh_playback_osd_flash(&browser);
            }
            /* Checkpoint periodically: stop_playback covers a clean exit, this
               covers a crash, a yanked card, or a flat battery. */
            if (now - browser.resume_saved_ms >= VFH_RESUME_SAVE_EVERY_MS) {
                browser.resume_saved_ms = now;
                vfh_checkpoint_playing(&browser, vfh_player_position(browser.player),
                                       vfh_player_duration(browser.player));
            }
            if (vfh_player_is_finished(browser.player)) {
                vfh_handle_eof(&browser);
                if (!browser.player) {
                    vfh_sync_poster(&browser);
                    vfh_draw_browser(&browser);
                }
            } else {
                vfh_draw_playback(&browser);
            }
        } else {
            vfh_sync_poster(&browser);
            vfh_draw_browser(&browser);
        }
        /* Catastrophe intentionally idles until input (or a clock redraw).
         * Keep that power-saving behaviour for a settled browser, but wake
         * often enough to publish a completed scan or lazy poster without
         * making the user press a button first. */
        if (!browser.player && (browser.rescan.running ||
                                browser.poster_state == VFH_THUMB_PENDING))
            cat_request_frame_in(100);
        cat_present();
    }

    vfh_stop_playback(&browser);
    vfh_rescan_cancel(&browser, false);
    pthread_mutex_destroy(&browser.rescan.mutex);
    vfh_status_monitor_destroy(&browser.audio_status);
    vfh_clear_poster(&browser);
    if (browser.thumb_worker_ready) vfh_thumb_worker_destroy(&browser.thumbs);
    (void)vfh_queue_save(&browser.queue);
    vfh_library_destroy(&browser.catalog);
    free(browser.entries);
    cat_quit();
    return 0;
}
