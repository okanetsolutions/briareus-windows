// A pull request's changed files as GitHub's Files changed tab: the tree of paths on the left, the chosen file's diff beside it.
// The component is laid out inside the pull request screen, and the standalone screen wraps it for the conversation's menu.
#include "diff.h"
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - Tree

/// A directory or file of the tree. Directories hold only one child directory are shown as one row, `a/b/c`, as GitHub does.
typedef struct {
    char *name, *path;   // the row's text and the full directory path (files: the filename)
    int file;            // index into the list, or -1 for a directory
    int *kids; size_t kid_count, kid_cap;
    int depth;
} TreeNode;

struct PullFiles {
    Screen *host; int action_base; UINT page_timer;
    Project project; int number;
    PullFileList list;
    bool loading, changed, confirmed, retried;
    char *error;
    Request *req;
    // The tree, rebuilt when the list changes.
    TreeNode *nodes; size_t node_count, node_cap; size_t tree_files; bool tree_valid;
    char **collapsed; size_t collapsed_count;   // directory paths folded by the user
    // The chosen file and its parsed patch.
    int selected; char *selected_path;
    DiffLine *lines; size_t line_count; int lines_for;
    bool wrap;
};

enum { ACT_SELECT, ACT_TOGGLE, ACT_RETRY, ACT_WRAP, ACT_OPEN_URL };

static int node_add(PullFiles *f, const char *name, const char *path, int file, int depth) {
    if (f->node_count == f->node_cap) { f->node_cap = f->node_cap ? f->node_cap * 2 : 64; f->nodes = xrealloc(f->nodes, f->node_cap * sizeof *f->nodes); }
    TreeNode *n = &f->nodes[f->node_count];
    memset(n, 0, sizeof *n);
    n->name = xstrdup(name); n->path = xstrdup(path); n->file = file; n->depth = depth;
    return (int)f->node_count++;
}
static void node_link(PullFiles *f, int parent, int kid) {
    TreeNode *p = &f->nodes[parent];
    if (p->kid_count == p->kid_cap) { p->kid_cap = p->kid_cap ? p->kid_cap * 2 : 4; p->kids = xrealloc(p->kids, p->kid_cap * sizeof *p->kids); }
    p->kids[p->kid_count++] = kid;
}
static void tree_free(PullFiles *f) {
    for (size_t i = 0; i < f->node_count; i++) { free(f->nodes[i].name); free(f->nodes[i].path); free(f->nodes[i].kids); }
    free(f->nodes); f->nodes = NULL; f->node_count = f->node_cap = 0; f->tree_valid = false;
}
static PullFiles *sort_owner;
static int node_compare(const void *a, const void *b) {
    const TreeNode *x = &sort_owner->nodes[*(const int *)a], *y = &sort_owner->nodes[*(const int *)b];
    if ((x->file < 0) != (y->file < 0)) return x->file < 0 ? -1 : 1;   // directories first
    return _stricmp(x->name, y->name);
}
static void tree_sort(PullFiles *f, int index) {
    TreeNode *n = &f->nodes[index];
    sort_owner = f;
    if (n->kid_count > 1) qsort(n->kids, n->kid_count, sizeof *n->kids, node_compare);
    for (size_t i = 0; i < n->kid_count; i++) tree_sort(f, n->kids[i]);
}
/// A directory whose only child is a directory takes it in: `src/Services/Export`.
static void tree_compact(PullFiles *f, int index) {
    TreeNode *n = &f->nodes[index];
    while (n->file < 0 && n->kid_count == 1 && f->nodes[n->kids[0]].file < 0 && n->depth > 0) {
        TreeNode *only = &f->nodes[n->kids[0]];
        char *joined = xstrfmt("%s/%s", n->name, only->name);
        free(n->name); n->name = joined;
        free(n->path); n->path = xstrdup(only->path);
        free(n->kids); n->kids = only->kids; n->kid_count = only->kid_count; n->kid_cap = only->kid_cap;
        only->kids = NULL; only->kid_count = only->kid_cap = 0; only->file = -2;   // dropped
    }
    for (size_t i = 0; i < n->kid_count; i++) tree_compact(f, n->kids[i]);
}
static void tree_depths(PullFiles *f, int index, int depth) {
    TreeNode *n = &f->nodes[index];
    n->depth = depth;
    for (size_t i = 0; i < n->kid_count; i++) tree_depths(f, n->kids[i], depth + 1);
}
static void tree_build(PullFiles *f) {
    tree_free(f);
    int root = node_add(f, "", "", -1, 0);
    for (size_t i = 0; i < f->list.file_count; i++) {
        const char *path = f->list.files[i].filename;
        int parent = root;
        for (const char *p = path; *p;) {
            const char *slash = strchr(p, '/');
            size_t n = slash ? (size_t)(slash - p) : strlen(p);
            char *part = xstrndup(p, n), *full = xstrndup(path, (size_t)(p - path) + n);
            int found = -1;
            if (slash) for (size_t k = 0; k < f->nodes[parent].kid_count; k++) { int kid = f->nodes[parent].kids[k]; if (f->nodes[kid].file < 0 && str_eq(f->nodes[kid].name, part)) { found = kid; break; } }
            if (found < 0) { found = node_add(f, part, full, slash ? -1 : (int)i, f->nodes[parent].depth + 1); node_link(f, parent, found); }
            free(part); free(full);
            parent = found;
            p = slash ? slash + 1 : p + n;
        }
    }
    tree_compact(f, root);
    tree_depths(f, root, 0);
    tree_sort(f, root);
    f->tree_files = f->list.file_count; f->tree_valid = true;
}
static bool dir_collapsed(PullFiles *f, const char *path) { for (size_t i = 0; i < f->collapsed_count; i++) if (str_eq(f->collapsed[i], path)) return true; return false; }
static void dir_toggle(PullFiles *f, const char *path) {
    for (size_t i = 0; i < f->collapsed_count; i++) if (str_eq(f->collapsed[i], path)) { free(f->collapsed[i]); f->collapsed[i] = f->collapsed[--f->collapsed_count]; return; }
    f->collapsed = xrealloc(f->collapsed, (f->collapsed_count + 1) * sizeof *f->collapsed);
    f->collapsed[f->collapsed_count++] = xstrdup(path);
}

// MARK: - Reading

static char *files_key(PullFiles *f) { return xstrfmt("files:%s#%d", f->project.repo, f->number); }
static Pane *files_pane(PullFiles *f) { return f->host->pane; }
static void list_changed(PullFiles *f) { f->tree_valid = false; f->lines_for = -1; }

static void files_done(void *owner, Request *req) {
    PullFiles *f = owner;
    f->loading = false;
    if (!req->ok) {
        // A push or rebase invalidates the pages already read; never mix two revisions.
        if (req->error.kind == API_HTTP && req->error.status == 409 && !f->retried) {
            pull_file_list_free(&f->list); pull_file_list_init(&f->list); list_changed(f);
            f->changed = true; f->confirmed = true; f->retried = true;
            pull_files_load(f);
            return;
        }
        request_error_into(&f->error, req);
        pane_relayout(files_pane(f));
        return;
    }
    PullFilesPage page;
    if (!pull_files_page_parse(req->result, &page)) { set_string(&f->error, "The server returned an unexpected response."); pane_relayout(files_pane(f)); return; }
    set_string(&f->error, NULL);
    f->retried = false;
    if (!f->confirmed) {
        f->confirmed = true;
        // The same revision has the same files; only a push or rebase makes them worth reading again.
        if (pull_file_list_confirm(&f->list, &page)) { pull_files_page_free(&page); pane_relayout(files_pane(f)); return; }
        pull_file_list_free(&f->list); pull_file_list_init(&f->list); list_changed(f);
    }
    pull_file_list_append(&f->list, &page); list_changed(f);
    pull_files_page_free(&page);
    // Only whole lists are saved, so a saved one never waits on a page.
    if (!f->list.next_page) { char *key = files_key(f); Json *j = pull_file_list_json(&f->list); cache_store(g_store.cache, j, key); json_free(j); free(key); }
    pane_relayout(files_pane(f));
}
void pull_files_load(PullFiles *f) {
    if (f->loading || !store_supports("pull_files")) return;
    if (!f->confirmed && json_is_null(f->list.pr)) {
        char *key = files_key(f);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { PullFileList list; if (pull_file_list_parse(saved, &list)) { pull_file_list_free(&f->list); f->list = list; list_changed(f); } json_free(saved); }
        free(key);
    }
    PullFileList fresh; pull_file_list_init(&fresh);
    Json *args = pull_file_list_arguments(f->confirmed ? &f->list : &fresh, f->project.repo, f->number);
    pull_file_list_free(&fresh);
    if (!args) return;
    f->loading = true;
    store_call("pull_files", args, 0, f, files_done, 0, &f->req);
}
void pull_files_cancel(PullFiles *f) { request_cancel(&f->req); f->loading = false; }
void pull_files_refresh(PullFiles *f) {
    pull_files_cancel(f);
    pull_file_list_free(&f->list); pull_file_list_init(&f->list); list_changed(f);
    f->changed = false; f->confirmed = true;
    pull_files_load(f);
}
bool pull_files_started(PullFiles *f) { return f->confirmed || !json_is_null(f->list.pr); }
bool pull_files_timer(PullFiles *f, UINT id) {
    if (id != f->page_timer) return false;
    KillTimer(pane_hwnd(files_pane(f)), id);
    pull_files_load(f);
    return true;
}

// MARK: - Painting

typedef struct { wchar_t glyph; COLORREF color; } MarkData;
static void file_mark(const PullFile *file, MarkData *m) {
    if (str_eq(file->status, "added")) { m->glyph = 0xE710; m->color = theme.success; }
    else if (str_eq(file->status, "removed")) { m->glyph = 0xE738; m->color = theme.danger; }
    else if (str_eq(file->status, "renamed") || str_eq(file->status, "copied")) { m->glyph = 0xE72A; m->color = theme.accent; }
    else { m->glyph = 0xE70F; m->color = theme.warning; }
}

/// A row of the tree: a chevron and folder for a directory, the status mark for a file, then the name.
typedef struct { char *name; int depth; bool dir, open, selected; MarkData mark; } RowData;
static void row_free(void *p) { RowData *d = p; free(d->name); free(d); }
static void paint_tree_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (d->selected || hovered) { RECT h = { rc->left, rc->top, rc->right, rc->bottom }; fill_round_rect(cv, &h, px(6), d->selected ? blend(theme.accent, theme.canvas, 0.16) : theme.raise, d->selected ? blend(theme.accent, theme.canvas, 0.16) : theme.raise); }
    int x = rc->left + px(6) + d->depth * px(16);
    if (d->dir) {
        RECT c = { x, rc->top, x + px(14), rc->bottom }; draw_glyph(cv, d->open ? 0xE70D : 0xE76C, &c, FONT_ICON_SMALL, theme.muted); x += px(16);
        RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, 0xE8B7, &g, FONT_ICON_SMALL, theme.accent); x += px(20);
    } else {
        x += px(16);
        RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, d->mark.glyph, &g, FONT_ICON_SMALL, d->mark.color); x += px(20);
    }
    RECT t = { x, rc->top, rc->right - px(6), rc->bottom };
    draw_text(cv, d->name, &t, d->selected ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
}

static void layout_tree_node(PullFiles *f, Doc *doc, int index, int x, int w) {
    TreeNode *n = &f->nodes[index];
    if (n->file == -2) return;
    if (n->depth > 0) {
        RowData *d = xcalloc(1, sizeof *d);
        d->name = xstrdup(n->name); d->depth = n->depth - 1; d->dir = n->file < 0;
        d->open = d->dir && !dir_collapsed(f, n->path);
        d->selected = !d->dir && n->file == f->selected;
        if (!d->dir) file_mark(&f->list.files[n->file], &d->mark);
        int i = doc_custom(doc, x, w, px(26), paint_tree_row, d, row_free, f->action_base + (d->dir ? ACT_TOGGLE : ACT_SELECT), d->dir ? (intptr_t)index : (intptr_t)n->file);
        doc_item(doc, i)->hover_fill = false;
        doc_space(doc, px(1));
        if (d->dir && !d->open) return;
    }
    for (size_t k = 0; k < n->kid_count; k++) layout_tree_node(f, doc, n->kids[k], x, w);
}
static void layout_tree(PullFiles *f, Doc *doc, int x, int w) {
    if (!f->tree_valid || f->tree_files != f->list.file_count) tree_build(f);
    char *title = xstrfmt("%zu file%s", f->list.file_count, f->list.file_count == 1 ? "" : "s");
    doc_text(doc, x + px(6), w - px(12), title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(title);
    doc_space(doc, px(6));
    if (f->node_count) layout_tree_node(f, doc, 0, x, w);
}

typedef struct { DiffLine *line; bool wrap; } DiffRowData;
static void paint_diff_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    DiffRowData *d = it->data;
    DiffLine *l = d->line;
    if (l->kind == DIFF_HUNK || l->kind == DIFF_NOTE) {
        if (l->kind == DIFF_HUNK) fill_rect(cv, rc, blend(theme.accent, theme.raise, 0.08));
        RECT t = { rc->left + px(12), rc->top, rc->right - px(12), rc->bottom };
        draw_text(cv, l->text, &t, FONT_MONO_CAPTION2, theme.secondary, DT_LEFT | DT_VCENTER | (d->wrap ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE) | DT_NOCLIP);
        return;
    }
    COLORREF tint = l->kind == DIFF_ADDED ? theme.success : l->kind == DIFF_REMOVED ? theme.danger : theme.raise;
    if (l->kind != DIFF_CONTEXT) fill_rect(cv, rc, blend(tint, theme.raise, 0.13));
    char number[16] = "";
    int n = l->new_line ? l->new_line : l->old_line;
    if (n) snprintf(number, sizeof number, "%d", n);
    RECT nr = { rc->left, rc->top, rc->left + px(40), rc->bottom };
    draw_text(cv, number, &nr, FONT_MONO_CAPTION2, theme.tertiary, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    const char *sign = l->kind == DIFF_ADDED ? "+" : l->kind == DIFF_REMOVED ? "\xE2\x88\x92" : " ";
    RECT sr = { rc->left + px(48), rc->top, rc->left + px(60), rc->bottom };
    draw_text(cv, sign, &sr, FONT_MONO_SMALL, l->kind == DIFF_CONTEXT ? theme.secondary : tint, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT tr = { rc->left + px(62), rc->top, rc->right - px(12), rc->bottom };
    draw_text(cv, l->text[0] ? l->text : " ", &tr, FONT_MONO_SMALL, theme.text, DT_LEFT | DT_TOP | DT_EXPANDTABS | (d->wrap ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE | DT_NOCLIP));
}

/// `.file-header`: the path in mono type, its diffstat, and the wrap and GitHub buttons.
typedef struct { char *path, *from, *add, *del; } FileHeadData;
static void file_head_free(void *p) { FileHeadData *d = p; free(d->path); free(d->from); free(d->add); free(d->del); free(d); }
static void paint_file_head(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    FileHeadData *d = it->data;
    COLORREF tint = blend(theme.accent, theme.raise, 0.06);
    RECT top = *rc; fill_round_rect(cv, &top, px(7), tint, tint);
    RECT low = { rc->left, (rc->top + rc->bottom) / 2, rc->right, rc->bottom }; fill_rect(cv, &low, tint);
    draw_line(cv, rc->left, rc->bottom - 1, rc->right, rc->bottom - 1, theme.line);
    int right = rc->right - px(12);
    int dw = text_width(cv, d->del, FONT_MONO_SMALL), aw = text_width(cv, d->add, FONT_MONO_SMALL);
    RECT dr = { right - dw, rc->top, right, rc->bottom }; draw_text(cv, d->del, &dr, FONT_MONO_SMALL, theme.danger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT ar = { dr.left - px(6) - aw, rc->top, dr.left - px(6), rc->bottom }; draw_text(cv, d->add, &ar, FONT_MONO_SMALL, theme.success, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    int x = rc->left + px(12), mid = (rc->top + rc->bottom) / 2;
    RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, 0xE8A5, &g, FONT_ICON_SMALL, theme.muted); x += px(22);
    if (d->from) {
        RECT p = { x, rc->top, ar.left - px(12), mid + px(1) }; draw_text(cv, d->path, &p, FONT_MONO_SMALL, theme.ink, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        RECT q = { x, mid + px(1), ar.left - px(12), rc->bottom }; draw_text(cv, d->from, &q, FONT_MONO_CAPTION2, theme.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    } else {
        RECT p = { x, rc->top, ar.left - px(12), rc->bottom }; draw_text(cv, d->path, &p, FONT_MONO_SMALL, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    }
}

static void layout_diff(PullFiles *f, Doc *doc, int x, int w) {
    if (f->selected < 0 || (size_t)f->selected >= f->list.file_count) return;
    const PullFile *file = &f->list.files[f->selected];
    if (f->lines_for != f->selected) {
        diff_free(f->lines, f->line_count); f->lines = NULL; f->line_count = 0;
        if (file->patch) f->lines = diff_parse(file->patch, &f->line_count);
        f->lines_for = f->selected;
    }
    int box = doc_box_begin(doc, x, w, 0, theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    FileHeadData *head = xcalloc(1, sizeof *head);
    head->path = xstrdup(file->filename);
    if (file->previous_filename) head->from = xstrfmt("renamed from %s", file->previous_filename);
    head->add = xstrfmt("+%d", file->additions >= 0 ? file->additions : 0); head->del = xstrfmt("\xE2\x88\x92%d", file->deletions >= 0 ? file->deletions : 0);
    doc_custom(doc, x + 1, w - 2, px(40), paint_file_head, head, file_head_free, 0, 0);
    // The buttons under the header: wrap or scroll long lines, and the file on GitHub.
    doc_space(doc, px(8));
    ButtonSpec buttons[2]; size_t bc = 0;
    if (file->patch) buttons[bc++] = (ButtonSpec){ f->wrap ? 0xE8E4 : 0xE8E3, f->wrap ? "Scroll long lines" : "Wrap long lines", BUTTON_PLAIN, f->action_base + ACT_WRAP, 0, true };
    if (safe_web_url(file->url)) buttons[bc++] = (ButtonSpec){ 0xE8A7, "Open on GitHub", BUTTON_PLAIN, f->action_base + ACT_OPEN_URL, 0, true };
    if (bc) { doc_button_row(doc, x + px(8), w - px(16), buttons, bc); doc_space(doc, px(8)); }
    if (!file->patch) {
        doc_label(doc, x + px(12), w - px(24), 0xE8A5, "No diff available", FONT_SUBHEADLINE_SEMIBOLD, theme.text);
        doc_space(doc, px(6));
        doc_text(doc, x + px(12), w - px(24), "GitHub returns no patch for binary files and very large changes.", FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
        doc_space(doc, px(8));
    }
    int widest = x + w, first = (int)doc->count;
    for (size_t i = 0; i < f->line_count; i++) {
        DiffLine *l = &f->lines[i];
        int h;
        bool meta = l->kind == DIFF_HUNK || l->kind == DIFF_NOTE;
        FontId font = meta ? FONT_MONO_CAPTION2 : FONT_MONO_SMALL;
        int text_x = meta ? px(12) : px(62);
        if (f->wrap) h = measure_text(doc->cv, l->text[0] ? l->text : " ", w - 2 - text_x - px(12), font, DT_WORDBREAK | DT_EXPANDTABS);
        else { h = font_height(doc->cv, font); int tw = x + 1 + text_x + text_width(doc->cv, l->text, font) + px(24); if (tw > widest) widest = tw; }
        h += meta ? px(l->kind == DIFF_HUNK ? 12 : 4) : px(3);
        DiffRowData *d = xcalloc(1, sizeof *d); d->line = l; d->wrap = f->wrap;
        RECT rc = { x + 1, doc->y, x + w - 1, doc->y + h };
        int ii = doc_add(doc, &rc, paint_diff_row);
        doc_item(doc, ii)->data = d; doc_item(doc, ii)->free_data = free;
        doc->y += h;
    }
    if (f->line_count) doc_space(doc, px(6));
    doc_box_end(doc, box, 0);
    if (!f->wrap && widest > x + w) {
        // Long lines scroll sideways: the rows and their box grow to the widest line.
        for (size_t i = (size_t)first; i < doc->count; i++) if (doc->items[i].paint == paint_diff_row) doc->items[i].rc.right = widest - 1;
        doc_item(doc, box)->rc.right = widest;
        if (widest > doc->content_width) doc->content_width = widest;
    }
}

void pull_files_layout(PullFiles *f, Doc *doc, int x, int w) {
    if (f->changed) { doc_label(doc, x + px(4), w - px(8), 0xE72C, "This pull request changed while reading. Showing its latest revision.", FONT_FOOTNOTE, theme.secondary); doc_space(doc, px(10)); }
    if (f->error) {
        doc_notice(doc, x + px(4), w - px(8), f->error);
        doc_space(doc, px(8));
        doc_button(doc, x + px(4), 0, "Try again", BUTTON_BORDERED, f->action_base + ACT_RETRY, 0, !f->loading);
        doc_space(doc, px(12));
    }
    if (json_is_null(f->list.pr)) { if (!f->error) doc_loading(doc, x, w, "Loading changes\xE2\x80\xA6"); return; }
    if (!f->list.file_count && !f->list.next_page) { doc_text(doc, x + px(8), w - px(16), "No files changed", FONT_CALLOUT, theme.secondary, DT_WORDBREAK); return; }
    // The chosen file follows its path across revisions; the first file is chosen until then.
    if (f->selected_path) { f->selected = -1; for (size_t i = 0; i < f->list.file_count; i++) if (str_eq(f->list.files[i].filename, f->selected_path)) { f->selected = (int)i; break; } }
    if (f->selected < 0 || (size_t)f->selected >= f->list.file_count) f->selected = f->list.file_count ? 0 : -1;
    int top = doc->y;
    if (w >= px(720)) {
        // Wide: the tree on the left, the diff on the right, as GitHub's `.pr-toolbar` layout.
        int tree_w = w * 28 / 100, gap = px(16);
        if (tree_w < px(220)) tree_w = px(220);
        if (tree_w > px(320)) tree_w = px(320);
        int tree_first = (int)doc->count;
        layout_tree(f, doc, x, tree_w);
        int tree_last = (int)doc->count, tree_bottom = doc->y;
        doc->y = top;
        layout_diff(f, doc, x + tree_w + gap, w - tree_w - gap);
        // The tree stays in view beside the diff and scrolls on its own, as GitHub's file tree does: the page is as
        // long as the diff, or the view's height when the tree needs that.
        RECT view = pane_content_rect(files_pane(f));
        int tree_h = tree_bottom - top, room = view.bottom - view.top - px(24);
        if (tree_h > room) tree_h = room;
        if (doc->y < top + tree_h) doc->y = top + tree_h;
        doc_sticky(doc, tree_first, tree_last, doc->y);
    } else {
        layout_tree(f, doc, x, w);
        doc_space(doc, px(12));
        layout_diff(f, doc, x, w);
    }
    if (f->list.next_page && !f->error) {
        doc_space(doc, px(8));
        doc_loading(doc, x, w, "Loading more files\xE2\x80\xA6");
        if (!f->loading) SetTimer(pane_hwnd(files_pane(f)), f->page_timer, 1, NULL);
    }
    if (f->list.truncated) { doc_space(doc, px(8)); doc_text(doc, x + px(4), w - px(8), "GitHub lists only the first 3,000 files of this pull request.", FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
}

bool pull_files_action(PullFiles *f, int action, intptr_t arg) {
    int a = action - f->action_base;
    if (a < 0 || a >= PULL_FILES_ACTIONS) return false;
    switch (a) {
    case ACT_SELECT:
        if ((size_t)arg < f->list.file_count) { f->selected = (int)arg; set_string(&f->selected_path, f->list.files[arg].filename); pane_relayout(files_pane(f)); }
        break;
    case ACT_TOGGLE: if ((size_t)arg < f->node_count) { dir_toggle(f, f->nodes[arg].path); pane_relayout(files_pane(f)); } break;
    case ACT_RETRY: pull_files_load(f); pane_relayout(files_pane(f)); break;
    case ACT_WRAP: f->wrap = !f->wrap; pane_relayout(files_pane(f)); break;
    case ACT_OPEN_URL: if (f->selected >= 0 && (size_t)f->selected < f->list.file_count) open_web_url(f->list.files[f->selected].url); break;
    }
    return true;
}

PullFiles *pull_files_new(const Project *project, int number, Screen *host, int action_base, UINT page_timer) {
    PullFiles *f = xcalloc(1, sizeof *f);
    f->host = host; f->action_base = action_base; f->page_timer = page_timer;
    project_copy(&f->project, project); f->number = number;
    pull_file_list_init(&f->list);
    f->selected = -1; f->lines_for = -1; f->wrap = true;
    return f;
}
void pull_files_free(PullFiles *f) {
    if (!f) return;
    request_cancel(&f->req);
    pull_file_list_free(&f->list); project_free(&f->project); free(f->error);
    tree_free(f);
    for (size_t i = 0; i < f->collapsed_count; i++) free(f->collapsed[i]);
    free(f->collapsed);
    free(f->selected_path);
    diff_free(f->lines, f->line_count);
    free(f);
}

// MARK: - Standalone screen

enum { ACT_FILES_BASE = 1000 };
enum { TIMER_NEXT_PAGE = 5 };

typedef struct { Screen base; int number; PullFiles *files; } FilesScreen;

static void files_destroy(Screen *base) { FilesScreen *s = (FilesScreen *)base; pull_files_free(s->files); screen_release(base); }
static void files_layout(Screen *base, Doc *doc) { FilesScreen *s = (FilesScreen *)base; doc_space(doc, px(10)); pull_files_layout(s->files, doc, 0, doc->width); doc_space(doc, px(16)); }
static void files_header(Screen *base, HeaderInfo *info) { FilesScreen *s = (FilesScreen *)base; snprintf(info->title, sizeof info->title, "#%d", s->number); snprintf(info->subtitle, sizeof info->subtitle, "Files changed"); }
static void files_action(Screen *base, int action, intptr_t arg, POINT pt) { (void)pt; FilesScreen *s = (FilesScreen *)base; pull_files_action(s->files, action, arg); }
static void files_timer(Screen *base, UINT id) { FilesScreen *s = (FilesScreen *)base; pull_files_timer(s->files, id); }
static void files_visible(Screen *base, bool shown) { FilesScreen *s = (FilesScreen *)base; if (shown) pull_files_load(s->files); else pull_files_cancel(s->files); }
static void files_refresh(Screen *base) { FilesScreen *s = (FilesScreen *)base; pull_files_refresh(s->files); pane_relayout(base->pane); }
static const ScreenVTable files_vt = {
    .destroy = files_destroy, .layout = files_layout, .header = files_header, .action = files_action, .timer = files_timer,
    .visible = files_visible, .refresh = files_refresh,
};
Screen *pull_files_screen_new(const Project *project, int number) {
    FilesScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &files_vt; s->base.id = xstrfmt("files:%s#%d", project->repo, number);
    s->number = number;
    s->files = pull_files_new(project, number, &s->base, ACT_FILES_BASE, TIMER_NEXT_PAGE);
    return &s->base;
}
