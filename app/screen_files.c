// A pull request's description and changed files, and one file's diff.
#include "diff.h"
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - Files

enum { ACT_OPEN_FILE = 1000, ACT_RETRY, ACT_WRAP, ACT_OPEN_FILE_URL };
enum { TIMER_NEXT_PAGE = 5 };

typedef struct {
    Screen base;
    Project project; int number;
    PullFileList list;
    bool loading, changed, confirmed, retried;
    char *error;
    Request *req;
} FilesScreen;

static char *files_key(FilesScreen *s) { return xstrfmt("files:%s#%d", s->project.repo, s->number); }
static void files_load(FilesScreen *s);

static void files_done(void *owner, Request *req) {
    FilesScreen *s = owner;
    s->loading = false;
    if (!req->ok) {
        // A push or rebase invalidates the pages already read; never mix two revisions.
        if (req->error.kind == API_HTTP && req->error.status == 409 && !s->retried) {
            pull_file_list_free(&s->list); pull_file_list_init(&s->list);
            s->changed = true; s->confirmed = true; s->retried = true;
            files_load(s);
            return;
        }
        char *text = request_error_text(req); set_string(&s->error, text); free(text);
        pane_relayout(s->base.pane);
        return;
    }
    PullFilesPage page;
    if (!pull_files_page_parse(req->result, &page)) { set_string(&s->error, "The server returned an unexpected response."); pane_relayout(s->base.pane); return; }
    set_string(&s->error, NULL);
    s->retried = false;
    if (!s->confirmed) {
        s->confirmed = true;
        // The same revision has the same files; only a push or rebase makes them worth reading again.
        if (pull_file_list_confirm(&s->list, &page)) { pull_files_page_free(&page); pane_relayout(s->base.pane); return; }
        pull_file_list_free(&s->list); pull_file_list_init(&s->list);
    }
    pull_file_list_append(&s->list, &page);
    pull_files_page_free(&page);
    // Only whole lists are saved, so a saved one never waits on a page.
    if (!s->list.next_page) { char *key = files_key(s); Json *j = pull_file_list_json(&s->list); cache_store(g_store.cache, j, key); json_free(j); free(key); }
    pane_relayout(s->base.pane);
}
static void files_load(FilesScreen *s) {
    if (s->loading) return;
    if (!s->confirmed && json_is_null(s->list.pr)) {
        char *key = files_key(s);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { PullFileList list; if (pull_file_list_parse(saved, &list)) { pull_file_list_free(&s->list); s->list = list; } json_free(saved); }
        free(key);
    }
    PullFileList fresh; pull_file_list_init(&fresh);
    Json *args = pull_file_list_arguments(s->confirmed ? &s->list : &fresh, s->project.repo, s->number);
    pull_file_list_free(&fresh);
    if (!args) return;
    s->loading = true;
    store_call("pull_files", args, 0, s, files_done, 0, &s->req);
}

static void files_destroy(Screen *base) {
    FilesScreen *s = (FilesScreen *)base;
    request_cancel(&s->req);
    pull_file_list_free(&s->list); project_free(&s->project); free(s->error);
    screen_release(base);
}

typedef struct { wchar_t glyph; COLORREF color; } MarkData;
static void paint_mark(Doc *doc, Item *it, HDC hdc, const RECT *rc) { MarkData *d = it->data; draw_glyph(hdc, d->glyph, rc, FONT_ICON, d->color); }

static void files_layout(Screen *base, Doc *doc) {
    FilesScreen *s = (FilesScreen *)base;
    int w = doc->width;
    doc_space(doc, px(10));
    if (s->changed) { doc_label(doc, px(4), w - px(8), 0xE72C, "This pull request changed while reading. Showing its latest revision.", FONT_FOOTNOTE, theme.secondary); doc_space(doc, px(10)); }
    if (!json_is_null(s->list.pr)) {
        doc_section(doc, 0, w, "Description");
        int box = doc_box_begin(doc, 0, w, px(12), theme.elevated, theme.border, px(12));
        doc_item(doc, box)->hover_fill = false;
        char *body = str_trim(json_str(json_get(s->list.pr, "body")));
        if (!*body) doc_text(doc, px(12), w - px(24), "No description provided", FONT_CALLOUT, theme.secondary, DT_WORDBREAK);
        else doc_markdown(doc, px(12), w - px(24), body, FONT_CALLOUT);
        free(body);
        const char *author = json_str(json_get(s->list.pr, "author"));
        if (author) { doc_space(doc, px(8)); doc_label(doc, px(12), w - px(24), 0xE77B, author, FONT_CAPTION, theme.secondary); }
        doc_box_end(doc, box, px(12));
        // Files header with counts
        doc_space(doc, px(14));
        int y = doc->y;
        double changed, additions, deletions;
        char *title = json_num(json_get(s->list.pr, "changedFiles"), &changed) ? xstrfmt("%d files changed", (int)changed) : xstrdup("Files changed");
        RECT tr = { px(4), y, w * 2 / 3, y + font_height(doc->hdc, FONT_CAPTION_SEMIBOLD) + px(4) };
        doc_text_at(doc, &tr, title, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        free(title);
        if (json_num(json_get(s->list.pr, "additions"), &additions) && json_num(json_get(s->list.pr, "deletions"), &deletions)) {
            char *del = xstrfmt("\xE2\x88\x92%d", (int)deletions); char *add = xstrfmt("+%d", (int)additions);
            int dw = text_width(doc->hdc, del, FONT_MONO_SMALL), aw = text_width(doc->hdc, add, FONT_MONO_SMALL);
            RECT dr = { w - px(4) - dw, y, w - px(4), tr.bottom }; doc_text_at(doc, &dr, del, FONT_MONO_SMALL, theme.danger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            RECT ar = { dr.left - px(8) - aw, y, dr.left - px(8), tr.bottom }; doc_text_at(doc, &ar, add, FONT_MONO_SMALL, theme.success, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            free(del); free(add);
        }
        doc->y = tr.bottom + px(6);
        for (size_t i = 0; i < s->list.file_count; i++) {
            const PullFile *f = &s->list.files[i];
            int row = doc_box_begin(doc, 0, w, px(8), theme.elevated, theme.border, px(10));
            int left = px(12), inner = w - px(24);
            MarkData *m = xcalloc(1, sizeof *m);
            if (str_eq(f->status, "added")) { m->glyph = 0xE710; m->color = theme.success; }
            else if (str_eq(f->status, "removed")) { m->glyph = 0xE738; m->color = theme.danger; }
            else if (str_eq(f->status, "renamed") || str_eq(f->status, "copied")) { m->glyph = 0xE72A; m->color = theme.accent; }
            else { m->glyph = 0xE70F; m->color = theme.warning; }
            RECT mr = { left, doc->y, left + px(20), doc->y + px(20) };
            int mi = doc_add(doc, &mr, paint_mark); doc_item(doc, mi)->data = m; doc_item(doc, mi)->free_data = free;
            char *add = xstrfmt("+%d", f->additions >= 0 ? f->additions : 0), *del = xstrfmt("\xE2\x88\x92%d", f->deletions >= 0 ? f->deletions : 0);
            int cw = text_width(doc->hdc, add, FONT_MONO_SMALL) + text_width(doc->hdc, del, FONT_MONO_SMALL) + px(10);
            int tx = left + px(28), tw = inner - px(28) - cw - px(8);
            int y0 = doc->y;
            doc_text(doc, tx, tw, pull_file_name(f), FONT_MONO, theme.text, DT_SINGLELINE | DT_PATH_ELLIPSIS);
            char *dir = pull_file_directory(f);
            if (*dir) { doc_space(doc, px(2)); doc_text(doc, tx, tw, dir, FONT_MONO_CAPTION2, theme.secondary, DT_SINGLELINE | DT_PATH_ELLIPSIS); }
            free(dir);
            if (f->previous_filename) { char *from = xstrfmt("from %s", f->previous_filename); doc_space(doc, px(2)); doc_text(doc, tx, tw, from, FONT_MONO_CAPTION2, theme.secondary, DT_SINGLELINE | DT_PATH_ELLIPSIS); free(from); }
            int lh = font_height(doc->hdc, FONT_MONO_SMALL) + px(4);
            int dw = text_width(doc->hdc, del, FONT_MONO_SMALL), aw = text_width(doc->hdc, add, FONT_MONO_SMALL);
            RECT dr = { left + inner - dw, y0, left + inner, y0 + lh }; doc_text_at(doc, &dr, del, FONT_MONO_SMALL, theme.danger, DT_RIGHT | DT_SINGLELINE);
            RECT ar = { dr.left - px(6) - aw, y0, dr.left - px(6), y0 + lh }; doc_text_at(doc, &ar, add, FONT_MONO_SMALL, theme.success, DT_RIGHT | DT_SINGLELINE);
            free(add); free(del);
            doc_box_end(doc, row, px(8));
            doc_box_action(doc, row, ACT_OPEN_FILE, (intptr_t)i);
            doc_space(doc, px(4));
        }
        if (s->list.next_page && s->list.file_count && !s->error) {
            doc_loading(doc, 0, w, "Loading more files\xE2\x80\xA6");
            if (!s->loading) SetTimer(pane_hwnd(base->pane), TIMER_NEXT_PAGE, 1, NULL);
        }
        if (!s->list.file_count && !s->list.next_page) doc_text(doc, px(8), w - px(16), "No files changed", FONT_CALLOUT, theme.secondary, DT_WORDBREAK);
        if (s->list.truncated) { doc_space(doc, px(8)); doc_text(doc, px(4), w - px(8), "GitHub lists only the first 3,000 files of this pull request.", FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
    }
    if (s->error) {
        doc_space(doc, px(12));
        doc_notice(doc, px(4), w - px(8), s->error);
        doc_space(doc, px(8));
        doc_button(doc, px(4), 0, "Try again", BUTTON_BORDERED, ACT_RETRY, 0, !s->loading);
    }
    if (json_is_null(s->list.pr) && !s->error) doc_loading(doc, 0, w, "Loading changes\xE2\x80\xA6");
    doc_space(doc, px(16));
}
static void files_header(Screen *base, HeaderInfo *info) { FilesScreen *s = (FilesScreen *)base; snprintf(info->title, sizeof info->title, "#%d", s->number); snprintf(info->subtitle, sizeof info->subtitle, "Description and changes"); }
static void files_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    FilesScreen *s = (FilesScreen *)base;
    if (action == ACT_OPEN_FILE && (size_t)arg < s->list.file_count) app_push_detail(file_diff_screen_new(&s->list.files[arg]));
    if (action == ACT_RETRY) files_load(s);
}
static void files_timer(Screen *base, UINT id) { FilesScreen *s = (FilesScreen *)base; if (id == TIMER_NEXT_PAGE) { KillTimer(pane_hwnd(base->pane), id); files_load(s); } }
static void files_visible(Screen *base, bool shown) { FilesScreen *s = (FilesScreen *)base; if (shown) { if (!s->confirmed) files_load(s); } else { request_cancel(&s->req); s->loading = false; } }
static void files_refresh(Screen *base) {
    FilesScreen *s = (FilesScreen *)base;
    request_cancel(&s->req); s->loading = false;
    pull_file_list_free(&s->list); pull_file_list_init(&s->list);
    s->changed = false; s->confirmed = true;
    files_load(s);
    pane_relayout(base->pane);
}
static const ScreenVTable files_vt = {
    .destroy = files_destroy, .layout = files_layout, .header = files_header, .action = files_action, .timer = files_timer,
    .visible = files_visible, .refresh = files_refresh,
};
Screen *pull_files_screen_new(const Project *project, int number) {
    FilesScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &files_vt; s->base.id = xstrfmt("files:%s#%d", project->repo, number);
    project_copy(&s->project, project); s->number = number;
    pull_file_list_init(&s->list);
    return &s->base;
}

// MARK: - Diff

typedef struct {
    Screen base;
    PullFile file;
    DiffLine *lines; size_t line_count;
    bool wrap;
} DiffScreen;

typedef struct { DiffLine *line; bool wrap; } RowData;
static void paint_diff_row(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    RowData *d = it->data;
    DiffLine *l = d->line;
    if (l->kind == DIFF_HUNK || l->kind == DIFF_NOTE) {
        if (l->kind == DIFF_HUNK) fill_rect(hdc, rc, theme.surface);
        RECT t = { rc->left + px(12), rc->top, rc->right - px(12), rc->bottom };
        draw_text(hdc, l->text, &t, FONT_MONO_CAPTION2, theme.secondary, DT_LEFT | DT_VCENTER | (d->wrap ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE) | DT_NOCLIP);
        return;
    }
    COLORREF tint = l->kind == DIFF_ADDED ? theme.success : l->kind == DIFF_REMOVED ? theme.danger : theme.background;
    if (l->kind != DIFF_CONTEXT) fill_rect(hdc, rc, blend(tint, theme.background, 0.13));
    char number[16] = "";
    int n = l->new_line ? l->new_line : l->old_line;
    if (n) snprintf(number, sizeof number, "%d", n);
    RECT nr = { rc->left, rc->top, rc->left + px(34), rc->bottom };
    draw_text(hdc, number, &nr, FONT_MONO_CAPTION2, theme.tertiary, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    const char *sign = l->kind == DIFF_ADDED ? "+" : l->kind == DIFF_REMOVED ? "\xE2\x88\x92" : " ";
    RECT sr = { rc->left + px(40), rc->top, rc->left + px(52), rc->bottom };
    draw_text(hdc, sign, &sr, FONT_MONO_SMALL, l->kind == DIFF_CONTEXT ? theme.secondary : tint, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT tr = { rc->left + px(54), rc->top, rc->right - px(12), rc->bottom };
    draw_text(hdc, l->text[0] ? l->text : " ", &tr, FONT_MONO_SMALL, theme.text, DT_LEFT | DT_TOP | DT_EXPANDTABS | (d->wrap ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE | DT_NOCLIP));
}

static void diff_layout(Screen *base, Doc *doc) {
    DiffScreen *s = (DiffScreen *)base;
    int w = doc->width;
    doc_space(doc, px(10));
    doc_text(doc, px(4), w - px(8), s->file.filename, FONT_MONO, theme.text, DT_WORDBREAK);
    if (s->file.previous_filename) { char *t = xstrfmt("Renamed from %s", s->file.previous_filename); doc_space(doc, px(4)); doc_text(doc, px(4), w - px(8), t, FONT_MONO_SMALL, theme.secondary, DT_WORDBREAK); free(t); }
    doc_space(doc, px(10));
    if (!s->file.patch) {
        doc_label(doc, px(4), w - px(8), 0xE8A5, "No diff available", FONT_SUBHEADLINE_SEMIBOLD, theme.text);
        doc_space(doc, px(6));
        doc_text(doc, px(4), w - px(8), "GitHub returns no patch for binary files and very large changes.", FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    }
    int widest = w;
    for (size_t i = 0; i < s->line_count; i++) {
        DiffLine *l = &s->lines[i];
        int h;
        bool meta = l->kind == DIFF_HUNK || l->kind == DIFF_NOTE;
        FontId f = meta ? FONT_MONO_CAPTION2 : FONT_MONO_SMALL;
        int text_x = meta ? px(12) : px(54);
        if (s->wrap) h = measure_text(doc->hdc, l->text[0] ? l->text : " ", w - text_x - px(12), f, DT_WORDBREAK | DT_EXPANDTABS);
        else { h = font_height(doc->hdc, f); int tw = text_x + text_width(doc->hdc, l->text, f) + px(24); if (tw > widest) widest = tw; }
        h += meta ? px(l->kind == DIFF_HUNK ? 12 : 4) : px(3);
        RowData *d = xcalloc(1, sizeof *d); d->line = l; d->wrap = s->wrap;
        RECT rc = { 0, doc->y, s->wrap ? w : widest, doc->y + h };
        int ii = doc_add(doc, &rc, paint_diff_row);
        doc_item(doc, ii)->data = d; doc_item(doc, ii)->free_data = free;
        doc->y += h;
    }
    if (!s->wrap) for (size_t i = 0; i < doc->count; i++) if (doc->items[i].paint == paint_diff_row) doc->items[i].rc.right = widest;
    if (!s->wrap && widest > doc->content_width) doc->content_width = widest;
    if (safe_web_url(s->file.url)) {
        doc_space(doc, px(12));
        int li = doc_label(doc, px(4), w - px(8), 0xE8A7, "Open file on GitHub", FONT_FOOTNOTE, theme.accent);
        doc_item(doc, li)->action = ACT_OPEN_FILE_URL; doc_item(doc, li)->hand = true;
    }
    doc_space(doc, px(16));
}
static void diff_header(Screen *base, HeaderInfo *info) {
    DiffScreen *s = (DiffScreen *)base;
    snprintf(info->title, sizeof info->title, "%s", pull_file_name(&s->file));
    info->buttons[0].glyph = s->wrap ? 0xE8E4 : 0xE8E3; info->buttons[0].action = ACT_WRAP; info->buttons[0].enabled = true;
    info->buttons[0].tip = s->wrap ? "Scroll long lines" : "Wrap long lines";
    info->button_count = 1;
}
static void diff_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)arg; (void)pt;
    DiffScreen *s = (DiffScreen *)base;
    if (action == ACT_WRAP) { s->wrap = !s->wrap; pane_relayout(base->pane); }
    if (action == ACT_OPEN_FILE_URL) open_web_url(s->file.url);
}
static void diff_destroy(Screen *base) {
    DiffScreen *s = (DiffScreen *)base;
    diff_free(s->lines, s->line_count); pull_file_free(&s->file);
    screen_release(base);
}
static const ScreenVTable diff_vt = { .destroy = diff_destroy, .layout = diff_layout, .header = diff_header, .action = diff_action };
Screen *file_diff_screen_new(const PullFile *file) {
    DiffScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &diff_vt; s->base.id = xstrfmt("diff:%s", file->filename);
    pull_file_copy(&s->file, file);
    s->wrap = true;
    if (s->file.patch) s->lines = diff_parse(s->file.patch, &s->line_count);
    return &s->base;
}
