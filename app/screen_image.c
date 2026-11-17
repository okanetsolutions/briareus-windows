// One picture from a conversation, opened on its own: fitted to the pane, or at a zoom, where the pane scrolls it.
#include "screens.h"
#include "attach.h"
#include "image_cache.h"
#include "picture.h"
#include "remote_image.h"
#include "str.h"
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACT_ZOOM_OUT = 1000, ACT_ZOOM_IN, ACT_SIZE, ACT_SAVE, ACT_BROWSER };

typedef struct ImageScreen ImageScreen;
/// The file read into pixels on a worker. It owns the bytes meanwhile, and outlives a screen closed before it ends.
typedef struct { ImageScreen *screen; void *bytes; size_t len; Picture picture; int width, height; bool ok; } DecodeJob;
struct ImageScreen {
    Screen base;
    char *url, *name;
    void *bytes; size_t len;     // the file as downloaded, for saving; with the job while it decodes
    Picture picture;             // at most PICTURE_MAX_PIXELS of it
    int width, height;           // the file's own size
    DecodeJob *job;
    char *error;
    double zoom;                 // of the file's own pixels; 0 fits the pane
};

static const double ZOOM_STEP = 1.25, ZOOM_MIN = 0.05, ZOOM_MAX = 8;

static void decode_work(void *p);
static void decode_done(void *p);
static double fit_zoom(ImageScreen *s, int width);
static double shown_zoom(ImageScreen *s, int width);
static void set_zoom(ImageScreen *s, double zoom);
static void save(ImageScreen *s);

static void decode_work(void *p) {
    DecodeJob *job = p;
    job->ok = picture_decode(job->bytes, job->len, 0, 0, &job->picture, &job->width, &job->height);
}

static void decode_done(void *p) {
    DecodeJob *job = p;
    ImageScreen *s = job->screen;
    if (!s) { picture_free(&job->picture); free(job->bytes); free(job); return; }
    s->job = NULL; s->bytes = job->bytes; s->len = job->len;
    if (job->ok) { s->picture = job->picture; s->width = job->width; s->height = job->height; }
    else s->error = xstrdup("Windows could not read this picture.");
    free(job);
    if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}

// MARK: - Zoom

/// The margin around the picture.
static int margin(void) { return px(16); }

/// The zoom at which the whole picture is in view, never above its own size.
static double fit_zoom(ImageScreen *s, int width) {
    RECT view = { 0, 0, 0, 0 };
    if (s->base.pane) view = pane_content_rect(s->base.pane);
    int room_w = width - 2 * margin(), room_h = view.bottom - view.top - 2 * margin();
    double zoom = 1;
    if (room_w > 0 && s->width * zoom > room_w) zoom = (double)room_w / s->width;
    if (room_h > 0 && s->height * zoom > room_h) zoom = (double)room_h / s->height;
    return zoom;
}
static double shown_zoom(ImageScreen *s, int width) { return s->zoom > 0 ? s->zoom : fit_zoom(s, width); }

static void set_zoom(ImageScreen *s, double zoom) {
    if (zoom > 0 && zoom < ZOOM_MIN) zoom = ZOOM_MIN;
    if (zoom > ZOOM_MAX) zoom = ZOOM_MAX;
    s->zoom = zoom;
    if (!s->base.pane) return;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

// MARK: - Saving

static bool write_file(const wchar_t *path, const void *bytes, size_t len) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (size_t done = 0; ok && done < len;) {
        DWORD chunk = (DWORD)(len - done > (1u << 20) ? (1u << 20) : len - done), written = 0;
        ok = WriteFile(file, (const char *)bytes + done, chunk, &written, NULL) && written;
        done += written;
    }
    CloseHandle(file);
    if (!ok) DeleteFileW(path);
    return ok;
}

static void save(ImageScreen *s) {
    if (!s->bytes) return;
    // The dialog runs its own message loop, in which the screen may go: it works on copies.
    size_t len = s->len;
    void *bytes = xmalloc(len); memcpy(bytes, s->bytes, len);
    wchar_t file[MAX_PATH * 2], *wname = utf8_to_wide(s->name);
    _snwprintf(file, MAX_PATH * 2, L"%ls", wname); file[MAX_PATH * 2 - 1] = 0;
    free(wname);
    for (wchar_t *q = file; *q; q++) if (wcschr(L"<>:\"/\\|?*", *q) || *q < 32) *q = L'_';
    wchar_t *downloads = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &downloads))) downloads = NULL;
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = app_dialog_owner();
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrInitialDir = downloads;
    ofn.lpstrTitle = L"Save picture";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    app_modal_begin(ofn.hwndOwner);
    bool picked = GetSaveFileNameW(&ofn);
    app_modal_end();
    if (picked && !write_file(file, bytes, len)) app_alert("Save picture", "The picture could not be written there.");
    if (downloads) CoTaskMemFree(downloads);
    free(bytes);
}

// MARK: - Screen

static void paint_image(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ImageScreen *s = it->data;
    (void)doc;
    // What shows through a picture with transparency.
    fill_rect(cv, rc, theme.sunken);
    draw_picture(cv, &s->picture, rc);
}

static void image_layout(Screen *base, Doc *doc) {
    ImageScreen *s = (ImageScreen *)base;
    int m = margin();
    if (s->error) { doc_space(doc, m); doc_notice_box(doc, m, doc->width - 2 * m, s->error); return; }
    if (!s->picture.pixels) { doc_space(doc, px(40)); doc_loading(doc, 0, doc->width, "Opening the picture\xE2\x80\xA6"); return; }
    double zoom = shown_zoom(s, doc->width);
    int w = (int)(s->width * zoom + 0.5), h = (int)(s->height * zoom + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // A picture smaller than the pane sits in its middle; a larger one starts at the margin and scrolls.
    RECT view = { 0, 0, 0, 0 };
    if (base->pane) view = pane_content_rect(base->pane);
    int x = w < doc->width - 2 * m ? (doc->width - w) / 2 : m, room_h = view.bottom - view.top;
    doc_space(doc, h < room_h - 2 * m ? (room_h - h) / 2 : m);
    int item = doc_custom(doc, x, w, h, paint_image, s, NULL, ACT_SIZE, 0);
    doc_item(doc, item)->hover_fill = false;
    if (x + w + m > doc->content_width) doc->content_width = x + w + m;
    doc_space(doc, m);
}

static void header_button(HeaderInfo *info, wchar_t glyph, const char *label, int action, bool enabled) {
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = glyph; b->action = action; b->enabled = enabled;
    snprintf(b->label, sizeof b->label, "%s", label);
}

static void image_header(Screen *base, HeaderInfo *info) {
    ImageScreen *s = (ImageScreen *)base;
    snprintf(info->title, sizeof info->title, "%s", s->name);
    bool shown = s->picture.pixels != NULL;
    if (shown) {
        char *size = format_file_size(s->len);
        int percent = (int)(shown_zoom(s, base->pane ? pane_content_width(base->pane) : 0) * 100 + 0.5);
        snprintf(info->subtitle, sizeof info->subtitle, "%d \xC3\x97 %d \xC2\xB7 %s \xC2\xB7 %d%%", s->width, s->height, size, percent);
        free(size);
    }
    header_button(info, 0xE71F, "\xE2\x88\x92", ACT_ZOOM_OUT, shown);
    header_button(info, 0xE8A3, "+", ACT_ZOOM_IN, shown);
    header_button(info, 0xE9A6, s->zoom > 0 ? "Fit" : "Actual size", ACT_SIZE, shown);
    header_button(info, 0xE896, "Save", ACT_SAVE, s->bytes != NULL);
    header_button(info, 0xE8A7, "Open in browser", ACT_BROWSER, true);
}

static void image_action(Screen *base, int action, intptr_t arg, POINT pt) {
    ImageScreen *s = (ImageScreen *)base;
    (void)arg; (void)pt;
    double zoom = shown_zoom(s, base->pane ? pane_content_width(base->pane) : 0);
    switch (action) {
    case ACT_ZOOM_OUT: set_zoom(s, zoom / ZOOM_STEP); break;
    case ACT_ZOOM_IN: set_zoom(s, zoom * ZOOM_STEP); break;
    case ACT_SIZE: set_zoom(s, s->zoom > 0 ? 0 : 1); break;
    case ACT_SAVE: save(s); break;
    case ACT_BROWSER: open_web_url(s->url); break;
    }
}

static bool image_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    POINT none = { 0, 0 };
    if (ctrl && vk == 'S') { image_action(base, ACT_SAVE, 0, none); return true; }
    if (vk == VK_ADD || vk == VK_OEM_PLUS) { image_action(base, ACT_ZOOM_IN, 0, none); return true; }
    if (vk == VK_SUBTRACT || vk == VK_OEM_MINUS) { image_action(base, ACT_ZOOM_OUT, 0, none); return true; }
    if (vk == '0' || vk == VK_NUMPAD0) { set_zoom((ImageScreen *)base, 0); return true; }
    if (vk == '1' || vk == VK_NUMPAD1) { set_zoom((ImageScreen *)base, 1); return true; }
    return false;
}

static void image_destroy(Screen *base) {
    ImageScreen *s = (ImageScreen *)base;
    if (s->job) s->job->screen = NULL;
    picture_free(&s->picture);
    free(s->bytes); free(s->url); free(s->name); free(s->error);
    screen_release(base);
}

static const ScreenVTable image_vt = {
    .destroy = image_destroy, .layout = image_layout, .header = image_header, .action = image_action, .key = image_key,
};

Screen *image_screen_new(const char *url) {
    ImageScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &image_vt; s->base.id = xstrfmt("image:%s", url);
    s->url = xstrdup(url); s->name = remote_image_name(url);
    size_t len = 0;
    void *bytes = image_cache_bytes(url, &len);
    if (!bytes) { s->error = xstrdup("This picture is no longer loaded. Go back and open it again."); return &s->base; }
    DecodeJob *job = xcalloc(1, sizeof *job);
    job->screen = s; job->bytes = bytes; job->len = len;
    s->job = job;
    async_run(decode_work, decode_done, job);
    return &s->base;
}
