/*
 * Video From Hell — Phase 3 library browser.
 *
 * Scanning is deliberately source-local: a multi-card device starts at an SD
 * source chooser, and navigation never merges two cards into one directory.
 * Poster decoding and playback workers never touch the renderer. Uploading
 * decoded video frames stays in this main thread.
 */
#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <libavutil/pixfmt.h>

#include "vfh_inhibit.h"
#include "vfh_media.h"
#include "vfh_player.h"
#include "vfh_resume.h"
#include "vfh_sources.h"
#include "vfh_srt.h"
#include "vfh_thumb.h"

#define VFH_ENTRY_MAX 768
#define VFH_NAME_MAX 256

typedef enum {
    VFH_ENTRY_SOURCE,
    VFH_ENTRY_PARENT,
    VFH_ENTRY_DIRECTORY,
    VFH_ENTRY_VIDEO,
} vfh_entry_kind;

typedef struct {
    vfh_entry_kind kind;
    int source_index;
    bool available;
    bool has_duration;
    double duration;
    char name[VFH_NAME_MAX];
    char path[VFH_SOURCE_PATH_MAX];
} vfh_entry;

typedef struct {
    vfh_sources sources;
    vfh_entry entries[VFH_ENTRY_MAX];
    int entry_count;
    int active_source;
    bool source_menu;
    char directory[VFH_SOURCE_PATH_MAX];
    char message[256];
    cat_list_state list;
    vfh_thumb_worker thumbs;
    bool thumb_worker_ready;
    SDL_Texture *poster;
    char poster_path[VFH_SOURCE_PATH_MAX];
    vfh_player *player;
    SDL_Texture *video_texture;
    int video_width;
    int video_height;
    Uint32 video_format;
    double pixel_aspect;          /* sample aspect ratio, 1.0 when square */
    char playing_path[VFH_SOURCE_PATH_MAX];
    char playing_name[VFH_NAME_MAX];
    int aspect_mode;
    Uint32 osd_until_ms;          /* auto-hide deadline; 0 = not auto-showing */
    bool osd_pinned;              /* Y toggled it on until toggled off */
    vfh_srt *subtitles;
    bool subtitles_on;
    bool seek_back_held;
    bool seek_forward_held;
    Uint32 seek_last_ms;
    Uint32 resume_saved_ms;
} vfh_browser;

/* Aspect handling for playback. */
enum { VFH_ASPECT_FIT = 0, VFH_ASPECT_FILL, VFH_ASPECT_STRETCH, VFH_ASPECT_COUNT };

/* OSD auto-hide window after a transport action. */
#define VFH_OSD_LINGER_MS 3000
/* Hold-to-seek step and cadence for L2/R2. */
#define VFH_SEEK_HOLD_STEP_S 5.0
#define VFH_SEEK_HOLD_EVERY_MS 220
/* Periodic resume checkpoint, so a crash or a flat battery still resumes. */
#define VFH_RESUME_SAVE_EVERY_MS 15000

static bool vfh_osd_visible(const vfh_browser *browser) {
    return browser->osd_pinned || SDL_GetTicks() < browser->osd_until_ms;
}

static void vfh_osd_flash(vfh_browser *browser) {
    browser->osd_until_ms = SDL_GetTicks() + VFH_OSD_LINGER_MS;
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

static int vfh_entry_rank(vfh_entry_kind kind) {
    switch (kind) {
        case VFH_ENTRY_PARENT: return 0;
        case VFH_ENTRY_DIRECTORY: return 1;
        case VFH_ENTRY_VIDEO: return 2;
        case VFH_ENTRY_SOURCE: return 3;
    }
    return 4;
}

static int vfh_entry_compare(const void *a, const void *b) {
    const vfh_entry *left = a;
    const vfh_entry *right = b;
    int rank = vfh_entry_rank(left->kind) - vfh_entry_rank(right->kind);
    if (rank) return rank;
    return strcasecmp(left->name, right->name);
}

static void vfh_clear_poster(vfh_browser *browser) {
    if (browser->poster) SDL_DestroyTexture(browser->poster);
    browser->poster = NULL;
    browser->poster_path[0] = '\0';
}

static void vfh_stop_playback(vfh_browser *browser) {
    /* Released here rather than at each call site: every path out of playback
       (back, finished, quit, starting another file) funnels through this. */
    vfh_inhibit_release();
    /* Checkpoint before the player goes away -- position comes from it. */
    if (browser->player && browser->playing_path[0])
        vfh_resume_set(browser->playing_path,
                       vfh_player_position(browser->player),
                       vfh_player_duration(browser->player));
    vfh_srt_free(browser->subtitles);
    browser->subtitles = NULL;
    if (browser->video_texture) SDL_DestroyTexture(browser->video_texture);
    browser->video_texture = NULL;
    browser->video_width = browser->video_height = 0;
    browser->video_format = SDL_PIXELFORMAT_UNKNOWN;
    if (browser->player) vfh_player_destroy(browser->player);
    browser->player = NULL;
    browser->playing_path[0] = '\0';
    browser->playing_name[0] = '\0';
}

static void vfh_set_message(vfh_browser *browser, const char *message) {
    snprintf(browser->message, sizeof(browser->message), "%s", message ? message : "");
    if (message && message[0]) cat_log("videofromhell: %s", message);
}

static void vfh_add_entry(vfh_browser *browser, vfh_entry_kind kind,
                          const char *name, const char *path, int source_index) {
    if (browser->entry_count >= VFH_ENTRY_MAX) return;
    vfh_entry *entry = &browser->entries[browser->entry_count++];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    entry->source_index = source_index;
    snprintf(entry->name, sizeof(entry->name), "%s", name ? name : "");
    snprintf(entry->path, sizeof(entry->path), "%s", path ? path : "");
}

static void vfh_scan_directory(vfh_browser *browser) {
    browser->entry_count = 0;
    cat_list_state_init(&browser->list, 1);
    vfh_clear_poster(browser);
    if (browser->active_source < 0 || browser->active_source >= browser->sources.count) return;
    const vfh_source *source = &browser->sources.items[browser->active_source];
    if (!source->available) {
        char problem[256];
        if (source->state == VFH_SOURCE_NO_FOLDER)
            snprintf(problem, sizeof(problem), "No videos yet — create %s", source->root);
        else
            snprintf(problem, sizeof(problem), "%s", vfh_source_state_message(source));
        vfh_set_message(browser, problem);
        return;
    }
    if (strcmp(browser->directory, source->root) != 0)
        vfh_add_entry(browser, VFH_ENTRY_PARENT, "..", source->root, browser->active_source);

    DIR *dir = opendir(browser->directory);
    if (!dir) {
        char problem[256];
        snprintf(problem, sizeof(problem), "Unable to read Videos: %s", strerror(errno));
        vfh_set_message(browser, problem);
        return;
    }
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') continue;
        if (browser->entry_count >= VFH_ENTRY_MAX) break;
        char path[VFH_SOURCE_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", browser->directory, item->d_name);
        if (written < 0 || written >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            vfh_add_entry(browser, VFH_ENTRY_DIRECTORY, item->d_name, path, browser->active_source);
        } else if (S_ISREG(st.st_mode) && vfh_media_file_supported(item->d_name)) {
            vfh_add_entry(browser, VFH_ENTRY_VIDEO, item->d_name, path, browser->active_source);
            vfh_entry *entry = &browser->entries[browser->entry_count - 1];
            entry->has_duration = vfh_media_probe_duration(path, &entry->duration);
        }
    }
    closedir(dir);
    qsort(browser->entries, (size_t)browser->entry_count, sizeof(browser->entries[0]),
          vfh_entry_compare);
    if (browser->entry_count == VFH_ENTRY_MAX)
        vfh_set_message(browser, "Folder is large; showing the first 768 items.");
    else if (browser->entry_count == 0)
        vfh_set_message(browser, "No supported videos in this folder.");
    else
        browser->message[0] = '\0';
}

static void vfh_show_source_menu(vfh_browser *browser) {
    browser->source_menu = true;
    browser->active_source = -1;
    browser->entry_count = 0;
    cat_list_state_init(&browser->list, 1);
    vfh_clear_poster(browser);
    for (int i = 0; i < browser->sources.count; i++) {
        const vfh_source *source = &browser->sources.items[i];
        vfh_add_entry(browser, VFH_ENTRY_SOURCE, source->label, source->root, i);
        browser->entries[browser->entry_count - 1].available = source->available;
    }
}

static void vfh_open_source(vfh_browser *browser, int source_index) {
    if (source_index < 0 || source_index >= browser->sources.count) return;
    browser->source_menu = false;
    browser->active_source = source_index;
    snprintf(browser->directory, sizeof(browser->directory), "%s",
             browser->sources.items[source_index].root);
    vfh_scan_directory(browser);
}

static void vfh_parent_directory(vfh_browser *browser) {
    if (browser->source_menu) return;
    const char *root = browser->sources.items[browser->active_source].root;
    if (strcmp(browser->directory, root) == 0) {
        if (browser->sources.count > 1) vfh_show_source_menu(browser);
        return;
    }
    char *slash = strrchr(browser->directory, '/');
    if (slash && slash != browser->directory) *slash = '\0';
    else snprintf(browser->directory, sizeof(browser->directory), "%s", root);
    vfh_scan_directory(browser);
}

static void vfh_sync_poster(vfh_browser *browser) {
    if (browser->source_menu || browser->list.cursor < 0 ||
        browser->list.cursor >= browser->entry_count) {
        vfh_clear_poster(browser);
        return;
    }
    const vfh_entry *entry = &browser->entries[browser->list.cursor];
    if (entry->kind != VFH_ENTRY_VIDEO) {
        vfh_clear_poster(browser);
        return;
    }
    if (strcmp(browser->poster_path, entry->path) == 0 && browser->poster) return;
    vfh_clear_poster(browser);
    SDL_Surface *surface = browser->thumb_worker_ready
        ? vfh_thumb_worker_take(&browser->thumbs, entry->path) : NULL;
    if (surface) {
        browser->poster = cat_texture_from_surface(surface);
        SDL_FreeSurface(surface);
    }
    if (!browser->poster) {
        char cache[VFH_THUMB_PATH_MAX];
        vfh_thumb_cache_path(entry->path, cache, sizeof(cache));
        if (access(cache, R_OK) == 0) browser->poster = cat_load_image(cache);
        else if (browser->thumb_worker_ready)
            vfh_thumb_worker_request(&browser->thumbs, entry->path);
    }
    if (browser->poster) snprintf(browser->poster_path, sizeof(browser->poster_path), "%s", entry->path);
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
    if (entry->kind == VFH_ENTRY_SOURCE) {
        switch (browser->sources.items[entry->source_index].state) {
            case VFH_SOURCE_OK:        kind = "Videos"; break;
            case VFH_SOURCE_NO_CARD:   kind = "Not mounted"; break;
            case VFH_SOURCE_NO_FOLDER: kind = "No Videos folder"; break;
        }
    } else if (entry->kind == VFH_ENTRY_PARENT) {
        kind = "Up";
    } else if (entry->kind == VFH_ENTRY_DIRECTORY) {
        kind = "Folder";
    } else {
        vfh_media_format_duration(entry->has_duration ? entry->duration : 0.0,
                                  meta, (int)sizeof(meta));
        kind = meta;
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

static void vfh_draw_preview(vfh_browser *browser, SDL_Rect preview) {
    cat_theme *theme = cat_get_theme();
    int pad = cat_scale(12);
    cat_draw_color panel = theme->text;
    panel.a = 18;
    cat_draw_rounded_rect(preview.x, preview.y, preview.w, preview.h, cat_scale(10), panel);
    if (browser->source_menu || browser->list.cursor < 0 ||
        browser->list.cursor >= browser->entry_count) return;
    vfh_entry *entry = &browser->entries[browser->list.cursor];
    if (entry->kind != VFH_ENTRY_VIDEO) {
        cat_draw_text(cat_get_font(CAT_FONT_LARGE), entry->kind == VFH_ENTRY_DIRECTORY ? "Folder" : "Videos",
                      preview.x + pad, preview.y + pad, theme->text);
        cat_draw_text_wrapped(cat_get_font(CAT_FONT_SMALL),
                              entry->kind == VFH_ENTRY_DIRECTORY
                                  ? "Press A to browse this folder."
                                  : "Choose an SD source to browse its Videos folder.",
                              preview.x + pad, preview.y + pad + TTF_FontHeight(cat_get_font(CAT_FONT_LARGE)) + cat_scale(8),
                              preview.w - pad * 2, theme->hint, CAT_ALIGN_LEFT);
        return;
    }
    if (browser->poster) {
        int side = preview.w - pad * 2;
        if (side > preview.h / 2) side = preview.h / 2;
        cat_draw_image_rounded_ex(browser->poster, preview.x + (preview.w - side) / 2,
                                  preview.y + pad, side, side, cat_scale(8), CAT_CORNER_ALL);
    } else {
        cat_draw_text(cat_get_font(CAT_FONT_SMALL), "Creating poster…",
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

static void vfh_draw_browser(vfh_browser *browser) {
    cat_draw_background();
    char title[256];
    if (browser->source_menu) snprintf(title, sizeof(title), "Video Sources");
    else snprintf(title, sizeof(title), "%s · %.235s",
                  browser->sources.items[browser->active_source].label,
                  vfh_basename(browser->directory));
    cat_draw_screen_title(title, NULL);
    SDL_Rect content = cat_get_content_rect(true, true, false);
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
        const char *empty = browser->source_menu ? "No Video sources were published." : "No videos found";
        cat_draw_text(cat_get_font(CAT_FONT_LARGE), empty,
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
    cat_footer_item footer[] = {
        { .button = CAT_BTN_MENU, .label = "Quit" },
        { .button = CAT_BTN_B, .label = browser->source_menu ? "Quit" : "Up" },
        { .button = CAT_BTN_LEFT, .label = "Jump", .button_text = "<->" },
        { .button = CAT_BTN_A, .label = browser->source_menu ? "Browse" : "Open", .is_confirm = true },
    };
    cat_draw_footer(footer, 4);
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

/* Transport row: skip-back / rewind / play-pause / forward / skip-next, drawn
   as glyphs rather than text so it reads at a glance and needs no extra font. */
static void vfh_draw_transport(const vfh_browser *browser, int cx, int cy, int size) {
    cat_draw_color on = { 255, 255, 255, 235 };
    int gap = size * 2;
    int bar = size / 5 > 0 ? size / 5 : 1;
    int half = size / 2;
    bool paused = vfh_player_is_paused(browser->player);

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

static void vfh_draw_osd(vfh_browser *browser) {
    double position = vfh_player_position(browser->player);
    double duration = vfh_player_duration(browser->player);
    int screen_w = cat_get_screen_width();
    int screen_h = cat_get_screen_height();
    int pad = cat_scale(16);
    int panel_h = cat_scale(104);
    int panel_y = screen_h - panel_h - pad;

    /* Scrim behind the panel: subtitles and light scenes both need the text to
       stay legible without dimming the whole frame. */
    cat_draw_rounded_rect(pad, panel_y, screen_w - pad * 2, panel_h, cat_scale(10),
                          (cat_draw_color){ 0, 0, 0, 185 });

    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    cat_draw_text_ellipsized(body, browser->playing_name, pad * 2, panel_y + cat_scale(8),
                             (cat_draw_color){ 255, 255, 255, 245 },
                             screen_w - pad * 4 - cat_scale(90));

    char clock[80], elapsed[32], total[32];
    vfh_media_format_duration(position, elapsed, (int)sizeof(elapsed));
    vfh_media_format_duration(duration, total, (int)sizeof(total));
    if (duration > 0.0) snprintf(clock, sizeof(clock), "%s / %s", elapsed, total);
    else                snprintf(clock, sizeof(clock), "%s", elapsed);
    int clock_w = cat_measure_text(small, clock);
    cat_draw_text(small, clock, screen_w - pad * 2 - clock_w, panel_y + cat_scale(12),
                  (cat_draw_color){ 210, 210, 210, 230 });

    /* Scrub bar */
    int bar_x = pad * 2;
    int bar_w = screen_w - pad * 4;
    int bar_y = panel_y + cat_scale(44);
    int bar_h = cat_scale(6);
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

    vfh_draw_transport(browser, screen_w / 2, panel_y + cat_scale(78), cat_scale(16));

    const char *aspect = vfh_aspect_label(browser->aspect_mode);
    cat_draw_text(small, aspect, pad * 2, panel_y + cat_scale(68),
                  (cat_draw_color){ 190, 190, 190, 200 });
    if (!vfh_player_has_audio(browser->player)) {
        const char *silent = "No audio track";
        int w = cat_measure_text(small, silent);
        cat_draw_text(small, silent, screen_w - pad * 2 - w, panel_y + cat_scale(68),
                      (cat_draw_color){ 190, 190, 190, 200 });
    }
}

/* Subtitles sit above the OSD when it is up, and just above the bottom edge
   otherwise, so the two never overlap. Drawn with an outline pass because a
   plain white line vanishes over a bright frame -- snow, sky, credits. */
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
    int height = cat_measure_wrapped_text_height(font, text, max_w);
    int bottom = vfh_osd_visible(browser) ? screen_h - cat_scale(136)
                                          : screen_h - cat_scale(24);
    int y = bottom - height;
    if (y < 0) y = 0;

    const int o = cat_scale(2) > 0 ? cat_scale(2) : 1;
    cat_draw_color shadow = { 0, 0, 0, 210 };
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            if (!dx && !dy) continue;
            cat_draw_text_wrapped(font, text, margin + dx * o, y + dy * o, max_w,
                                  shadow, CAT_ALIGN_CENTER);
        }
    cat_draw_text_wrapped(font, text, margin, y, max_w,
                          (cat_draw_color){ 255, 255, 255, 255 }, CAT_ALIGN_CENTER);
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
    if (vfh_osd_visible(browser)) vfh_draw_osd(browser);
    else if (vfh_player_is_paused(browser->player))
        cat_draw_text(cat_get_font(CAT_FONT_LARGE), "Paused",
                      cat_scale(18), cat_scale(18), (cat_draw_color){ 255, 255, 255, 255 });
    cat_request_frame_in(16);
}

static void vfh_start_playback(vfh_browser *browser, const vfh_entry *entry) {
    if (!entry || entry->kind != VFH_ENTRY_VIDEO) return;
    /* Read before stop_playback, which checkpoints the *outgoing* film. */
    double resume_at = vfh_resume_get(entry->path);
    vfh_stop_playback(browser);
    browser->player = vfh_player_create();
    if (!browser->player) {
        vfh_set_message(browser, "Unable to allocate the playback core.");
        return;
    }
    char error[256];
    if (!vfh_player_open(browser->player, entry->path, error, (int)sizeof(error))) {
        vfh_player_destroy(browser->player);
        browser->player = NULL;
        vfh_set_message(browser, error[0] ? error : "Unable to start playback.");
        return;
    }
    vfh_clear_poster(browser);
    snprintf(browser->playing_path, sizeof(browser->playing_path), "%s", entry->path);
    snprintf(browser->playing_name, sizeof(browser->playing_name), "%s", entry->name);
    browser->message[0] = '\0';
    browser->pixel_aspect = 1.0;
    browser->resume_saved_ms = SDL_GetTicks();
    /* Subtitles default to on when a sidecar exists: someone who put an .srt
       next to the film wants to see it without hunting for a toggle. */
    browser->subtitles = vfh_srt_load_for(entry->path);
    browser->subtitles_on = browser->subtitles != NULL;
    if (browser->subtitles)
        cat_log("videofromhell: subtitles: %d cues from %s",
                vfh_srt_count(browser->subtitles), vfh_srt_source(browser->subtitles));
    /* Offer the resume point before the first frame is shown, so the choice is
       made against a still library screen rather than over live video. */
    if (resume_at > 0.0) {
        char elapsed[32], prompt[VFH_NAME_MAX + 64];
        vfh_media_format_duration(resume_at, elapsed, (int)sizeof(elapsed));
        snprintf(prompt, sizeof(prompt), "Resume %s from %s?", entry->name, elapsed);
        cat_footer_item footer[] = {
            { CAT_BTN_B, "Start over", false, NULL },
            { CAT_BTN_A, "Resume", true, NULL },
        };
        cat_message_opts opts = { .message = prompt, .image_path = NULL,
                                  .footer = footer, .footer_count = 2 };
        cat_confirm_result result = { 0 };
        if (cat_confirmation(&opts, &result) == CAT_OK && result.confirmed)
            vfh_player_seek_relative(browser->player, resume_at);
    }
    /* Hold the backlight for the length of the film; jawakad's auto-sleep keys
       off input idle, which a viewer does not generate. */
    vfh_inhibit_acquire(entry->name);
    vfh_osd_flash(browser);
    cat_log("videofromhell: playback started: %s", entry->path);
}

static void vfh_activate_entry(vfh_browser *browser) {
    if (browser->list.cursor < 0 || browser->list.cursor >= browser->entry_count) return;
    vfh_entry *entry = &browser->entries[browser->list.cursor];
    if (entry->kind == VFH_ENTRY_SOURCE) {
        if (!entry->available) {
            const vfh_source *source = &browser->sources.items[entry->source_index];
            char problem[256];
            if (source->state == VFH_SOURCE_NO_FOLDER)
                snprintf(problem, sizeof(problem), "No videos yet — create %s", source->root);
            else
                snprintf(problem, sizeof(problem), "%s", vfh_source_state_message(source));
            vfh_set_message(browser, problem);
            return;
        }
        vfh_open_source(browser, entry->source_index);
    } else if (entry->kind == VFH_ENTRY_PARENT) {
        vfh_parent_directory(browser);
    } else if (entry->kind == VFH_ENTRY_DIRECTORY) {
        snprintf(browser->directory, sizeof(browser->directory), "%s", entry->path);
        vfh_scan_directory(browser);
    } else {
        vfh_start_playback(browser, entry);
    }
}

/* Step to the next/previous video in the folder the current one came from.
   Directories and the parent row are skipped; the ends do not wrap, because
   silently looping back to the first file reads as a bug during playback. */
static void vfh_play_adjacent(vfh_browser *browser, int delta) {
    if (!browser->player || !browser->playing_path[0]) return;
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
    vfh_osd_flash(browser);   /* at an end: show where we are rather than nothing */
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
    browser.active_source = -1;
    char source_error[256];
    if (!vfh_sources_resolve(&browser.sources, source_error, sizeof(source_error))) {
        vfh_sources_single_fallback(&browser.sources);
        vfh_set_message(&browser, source_error);
    }
    browser.thumb_worker_ready = vfh_thumb_worker_init(&browser.thumbs);
    if (!browser.thumb_worker_ready)
        vfh_set_message(&browser, "Poster worker unavailable; browsing continues.");
    if (browser.sources.count > 1) vfh_show_source_menu(&browser);
    else vfh_open_source(&browser, 0);

    bool running = true;
    while (running) {
        cat_input_event event;
        while (cat_poll_input(&event)) {
            if (browser.player) {
                /* L2/R2 are hold-to-seek, so they need the release edge too;
                   everything else below acts on a fresh press only. */
                if (event.button == CAT_BTN_L2 || event.button == CAT_BTN_R2) {
                    bool *held = event.button == CAT_BTN_L2 ? &browser.seek_back_held
                                                            : &browser.seek_forward_held;
                    if (event.pressed && !event.repeated) {
                        *held = true;
                        browser.seek_last_ms = 0;   /* seek immediately on press */
                    } else if (!event.pressed) {
                        *held = false;
                    }
                    continue;
                }
                if (!event.pressed || event.repeated) continue;
                switch (event.button) {
                    case CAT_BTN_A:
                    case CAT_BTN_X: {
                        bool pausing = !vfh_player_is_paused(browser.player);
                        vfh_player_set_paused(browser.player, pausing);
                        /* A paused film is not being watched: let the normal
                           idle timeout blank the screen again. */
                        if (pausing) vfh_inhibit_release();
                        else vfh_inhibit_acquire(browser.playing_name);
                        vfh_osd_flash(&browser);
                        break;
                    }
                    case CAT_BTN_LEFT:
                        vfh_player_seek_relative(browser.player, -10.0);
                        vfh_osd_flash(&browser);
                        break;
                    case CAT_BTN_RIGHT:
                        vfh_player_seek_relative(browser.player, +10.0);
                        vfh_osd_flash(&browser);
                        break;
                    case CAT_BTN_Y:
                        browser.osd_pinned = !browser.osd_pinned;
                        browser.osd_until_ms = 0;
                        break;
                    case CAT_BTN_STICK:
                        browser.aspect_mode = (browser.aspect_mode + 1) % VFH_ASPECT_COUNT;
                        vfh_osd_flash(&browser);
                        break;
                    case CAT_BTN_SELECT:
                        if (browser.subtitles) {
                            browser.subtitles_on = !browser.subtitles_on;
                            vfh_set_message(&browser, browser.subtitles_on
                                            ? "Subtitles on" : "Subtitles off");
                        } else {
                            vfh_set_message(&browser, "No subtitles for this video");
                        }
                        vfh_osd_flash(&browser);
                        break;
                    case CAT_BTN_L1:
                        vfh_play_adjacent(&browser, -1);
                        break;
                    case CAT_BTN_R1:
                        vfh_play_adjacent(&browser, +1);
                        break;
                    case CAT_BTN_B:
                        vfh_stop_playback(&browser);
                        vfh_set_message(&browser, "Returned to the video library.");
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
                case CAT_BTN_B:
                    if (browser.source_menu) running = false;
                    else vfh_parent_directory(&browser);
                    break;
                case CAT_BTN_MENU:
                    running = false;
                    break;
                default:
                    break;
            }
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
                vfh_osd_flash(&browser);
            }
            /* Checkpoint periodically: stop_playback covers a clean exit, this
               covers a crash, a yanked card, or a flat battery. */
            if (now - browser.resume_saved_ms >= VFH_RESUME_SAVE_EVERY_MS) {
                browser.resume_saved_ms = now;
                if (browser.playing_path[0])
                    vfh_resume_set(browser.playing_path,
                                   vfh_player_position(browser.player),
                                   vfh_player_duration(browser.player));
            }
            if (vfh_player_is_finished(browser.player)) {
                vfh_stop_playback(&browser);
                vfh_set_message(&browser, "Playback finished.");
                vfh_sync_poster(&browser);
                vfh_draw_browser(&browser);
            } else {
                vfh_draw_playback(&browser);
            }
        } else {
            vfh_sync_poster(&browser);
            vfh_draw_browser(&browser);
        }
        cat_present();
    }

    vfh_stop_playback(&browser);
    vfh_clear_poster(&browser);
    if (browser.thumb_worker_ready) vfh_thumb_worker_destroy(&browser.thumbs);
    cat_quit();
    return 0;
}
