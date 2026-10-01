// A composer's files: read by attach.c, uploaded through the store, painted as the dashboard's `.attach-item` chips.
#include "attach_list.h"
#include "attach.h"
#include "screens.h"
#include "str.h"
#include <commdlg.h>
#include <stdlib.h>
#include <string.h>

/// A file for the next message: uploading until `id` is set, as the dashboard's `.attach-item`.
typedef struct { char *name; size_t size; char *id; Request *req; RECT remove_rc; } Attachment;

struct AttachList {
    Screen *screen;
    char *call;
    Attachment **items; size_t count;   // each on the heap: an upload's slot points into it
    Attacher *attacher;
};

static void changed(AttachList *l) { if (l->screen->pane) pane_footer_changed(l->screen->pane); }
static HWND owner(AttachList *l) { return l->screen->pane ? pane_hwnd(l->screen->pane) : NULL; }

static void drop_at(AttachList *l, size_t i) {
    Attachment *a = l->items[i];
    request_cancel(&a->req); free(a->name); free(a->id); free(a);
    memmove(&l->items[i], &l->items[i + 1], (l->count - i - 1) * sizeof *l->items);
    l->count--;
}
static void upload_done(void *ctx, Request *req) {
    AttachList *l = ctx;
    Attachment *a = (Attachment *)req->arg;
    size_t i = 0;
    while (i < l->count && l->items[i] != a) i++;
    if (i == l->count) return;   // taken off the message meanwhile
    if (req->ok) a->id = xstrdup(req->text ? req->text : "");
    else {
        char *why = request_error_text(req);
        char *text = xstrfmt("%s could not be attached: %s", a->name, why);
        drop_at(l, i);
        app_alert("Attachment", text);
        free(text); free(why);
    }
    changed(l);
}
// The files read from the clipboard, a drop or the dialog: each goes to the server at once, and the call carries their ids.
static void arrived(void *ctx, AttachFile *files, size_t count, char *error) {
    AttachList *l = ctx;
    Str refused; str_init(&refused);
    if (error) str_appendz(&refused, error);
    for (size_t i = 0; i < count; i++) {
        if (l->count >= ATTACHMENTS_MAX) { str_appendf(&refused, "%sAt most %d files go with one message.", refused.len ? "\n" : "", ATTACHMENTS_MAX); break; }
        Attachment *a = xcalloc(1, sizeof *a);
        a->name = files[i].name; a->size = files[i].len; files[i].name = NULL;
        l->items = xrealloc(l->items, (l->count + 1) * sizeof *l->items);
        l->items[l->count++] = a;
        Request *r = store_upload(a->name, files[i].bytes, files[i].len, l, upload_done, 0, &a->req);
        files[i].bytes = NULL;
        r->arg = (intptr_t)a;
    }
    attach_files_free(files, count);
    free(error);
    if (refused.len) app_alert("Attachments", refused.data);
    str_free(&refused);
    changed(l);
}
static Attacher *attacher(AttachList *l) { if (!l->attacher) l->attacher = attacher_new(arrived, l); return l->attacher; }
static void unsupported(void) { app_alert("Attachments", "This server does not take files with a message. Update Briareus to a version whose client API accepts uploads."); }

AttachList *attach_list_new(Screen *screen, const char *call) {
    AttachList *l = xcalloc(1, sizeof *l);
    l->screen = screen; l->call = xstrdup(call);
    return l;
}
void attach_list_free(AttachList *l) {
    if (!l) return;
    while (l->count) drop_at(l, l->count - 1);
    if (l->attacher) attacher_free(l->attacher);
    free(l->items); free(l->call); free(l);
}
bool attach_list_supported(const AttachList *l) { return store_supports_attachments_on(l->call); }

bool attach_list_paste(AttachList *l) {
    if (!attach_clipboard_has_files()) return false;
    if (!attach_list_supported(l)) {
        if (attach_clipboard_has_text()) return false;
        unsupported();
        return true;
    }
    return attacher_from_clipboard(attacher(l), owner(l));
}
void attach_list_drop(AttachList *l, HDROP drop) {
    if (!attach_list_supported(l)) { DragFinish(drop); unsupported(); return; }
    attacher_from_drop(attacher(l), drop);
}
void attach_list_pick(AttachList *l) {
    if (!attach_list_supported(l)) { unsupported(); return; }
    // Several files come back as the folder, then each name, NUL-separated, ending in two NULs; one as its full path.
    enum { CHARS = 32768 };
    wchar_t *buf = xcalloc(CHARS, sizeof *buf);
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = owner(l);
    ofn.lpstrFilter = L"All files\0*.*\0Images\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = CHARS;
    ofn.lpstrTitle = L"Attach files";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) app_alert("Attachments", "Too many files at once. Pick fewer.");
        free(buf); return;
    }
    const wchar_t *dir = buf, *name = buf + wcslen(buf) + 1;
    if (!*name) { attacher_from_paths(attacher(l), &dir, 1); free(buf); return; }
    wchar_t **paths = NULL; size_t n = 0;
    for (; *name; name += wcslen(name) + 1) {
        size_t len = wcslen(dir) + 1 + wcslen(name) + 1;
        wchar_t *p = xmalloc(len * sizeof *p);
        _snwprintf(p, len, L"%ls\\%ls", dir, name); p[len - 1] = 0;
        paths = xrealloc(paths, (n + 1) * sizeof *paths);
        paths[n++] = p;
    }
    attacher_from_paths(attacher(l), (const wchar_t *const *)paths, n);
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths); free(buf);
}

size_t attach_list_count(const AttachList *l) { return l->count; }
bool attach_list_uploading(const AttachList *l) { for (size_t i = 0; i < l->count; i++) if (!l->items[i]->id) return true; return false; }
Json *attach_list_ids(const AttachList *l) {
    if (!l->count) return NULL;
    Json *ids = json_array();
    for (size_t i = 0; i < l->count; i++) if (l->items[i]->id) json_array_push(ids, json_string(l->items[i]->id));
    return ids;
}
void attach_list_sent(AttachList *l, const Json *ids) {
    for (size_t i = 0; i < json_count(ids); i++)
        for (size_t j = 0; j < l->count; j++)
            if (l->items[j]->id && str_eq(l->items[j]->id, json_str(json_at(ids, i)))) { drop_at(l, j); break; }
    changed(l);
}

// MARK: - Painting

static char *chip_label(const Attachment *a) {
    char *size = format_file_size(a->size);
    char *label = xstrfmt("%s %s \xC2\xB7 %s", a->id ? "\xF0\x9F\x93\x8E" : "\xE2\x80\xA6", a->name, size);
    free(size);
    return label;
}
/// Places the chips in rows 4px apart, wrapping at `width`; returns their height with the 6px under them.
static int layout(AttachList *l, Canvas *cv, int width, RECT *out) {
    if (!l->count) return 0;
    int x = 0, y = 0, h = px(24), gap = px(4);
    for (size_t i = 0; i < l->count; i++) {
        char *label = chip_label(l->items[i]);
        int w = px(8) + text_width(cv, label, FONT_CAPTION) + px(6) + px(16) + px(6);
        if (w > px(280)) w = px(280);
        free(label);
        if (x > 0 && x + w > width) { x = 0; y += h + gap; }
        if (out) { RECT r = { x, y, x + w, y + h }; out[i] = r; }
        x += w + gap;
    }
    return y + h + px(6);
}
int attach_list_height(AttachList *l, Canvas *cv, int width) { return layout(l, cv, width, NULL); }
void attach_list_paint(AttachList *l, Canvas *cv, int left, int top, int width) {
    if (!l->count) return;
    RECT *rects = xcalloc(l->count, sizeof *rects);
    layout(l, cv, width, rects);
    for (size_t i = 0; i < l->count; i++) {
        Attachment *a = l->items[i];
        RECT r = { left + rects[i].left, top + rects[i].top, left + rects[i].right, top + rects[i].bottom };
        fill_round_rect(cv, &r, px(6), theme.field, theme.line);
        char *label = chip_label(a);
        RECT t = { r.left + px(8), r.top, r.right - px(6) - px(16), r.bottom };
        draw_text(cv, label, &t, FONT_CAPTION, a->id ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        free(label);
        RECT x = { r.right - px(6) - px(16), r.top, r.right - px(6), r.bottom };
        a->remove_rc = x;
        draw_text(cv, "\xE2\x9C\x95", &x, FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    free(rects);
}
bool attach_list_click(AttachList *l, POINT pt) {
    for (size_t i = 0; i < l->count; i++)
        if (PtInRect(&l->items[i]->remove_rc, pt)) { drop_at(l, i); changed(l); return true; }
    return false;
}

void attach_button_paint(Canvas *cv, const RECT *rc, bool enabled) {
    RECT r = *rc;
    fill_round_rect(cv, &r, px(8), theme.raise, theme.line);
    draw_text(cv, "\xF0\x9F\x93\x8E", &r, FONT_EMOJI_LARGE, enabled ? theme.muted : theme.line, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
