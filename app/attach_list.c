// A composer's files: read by attach.c, uploaded through the store, painted as the dashboard's `.attach-item` chips.
#include "attach_list_internal.h"
#include "attach.h"
#include "screens.h"
#include "str.h"
#include <commdlg.h>
#include <stdlib.h>
#include <string.h>

static HWND owner(AttachList *l) { return l->screen->pane ? pane_hwnd(l->screen->pane) : NULL; }

static void unsupported(void) { app_alert("Attachments", "This server does not take files with a message. Update Briareus to a version whose client API accepts uploads."); }

bool attach_list_paste(AttachList *l) {
    if (!attach_clipboard_has_files()) return false;
    if (!attach_list_supported(l)) {
        if (attach_clipboard_has_text()) return false;
        unsupported();
        return true;
    }
    Attacher *a = attach_list_attacher(l); HWND hwnd = owner(l);
    attach_list_changed(l);
    return attacher_from_clipboard(a, hwnd);
}
void attach_list_drop(AttachList *l, HDROP drop) {
    if (!attach_list_supported(l)) { DragFinish(drop); unsupported(); return; }
    Attacher *a = attach_list_attacher(l); attach_list_changed(l);
    attacher_from_drop(a, drop);
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
    if (!*name) { attach_list_paths(l, &dir, 1); free(buf); return; }
    wchar_t **paths = NULL; size_t n = 0;
    for (; *name; name += wcslen(name) + 1) {
        size_t len = wcslen(dir) + 1 + wcslen(name) + 1;
        wchar_t *p = xmalloc(len * sizeof *p);
        _snwprintf(p, len, L"%ls\\%ls", dir, name); p[len - 1] = 0;
        paths = xrealloc(paths, (n + 1) * sizeof *paths);
        paths[n++] = p;
    }
    attach_list_paths(l, (const wchar_t *const *)paths, n);
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths); free(buf);
}

// MARK: - Painting

static char *chip_label(const Attachment *a) {
    if (!a->size) return xstrfmt("\xE2\x80\xA6 %s", a->name);
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
        if (PtInRect(&l->items[i]->remove_rc, pt)) { attach_list_remove(l, i); return true; }
    return false;
}

void attach_button_paint(Canvas *cv, const RECT *rc, bool enabled) {
    RECT r = *rc;
    fill_round_rect(cv, &r, px(8), theme.raise, theme.line);
    draw_text(cv, "\xF0\x9F\x93\x8E", &r, FONT_EMOJI_LARGE, enabled ? theme.muted : theme.line, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
