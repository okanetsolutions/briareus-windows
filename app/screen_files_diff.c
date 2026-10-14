#include "screen_files_internal.h"

typedef struct { DiffLine *line; bool wrap; } DiffRowData;

/// `.file-header`: the path in mono type, its diffstat, and the wrap and GitHub buttons.
typedef struct { char *path, *from, *add, *del; } FileHeadData;

static void paint_diff_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void file_head_free(void *p);
static void paint_file_head(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void layout_diff(PullFiles *f, Doc *doc, int x, int w);

void screen_files_file_mark(const PullFile *file, MarkData *m) {
    if (str_eq(file->status, "added")) { m->glyph = 0xE710; m->color = theme.success; }
    else if (str_eq(file->status, "removed")) { m->glyph = 0xE738; m->color = theme.danger; }
    else if (str_eq(file->status, "renamed") || str_eq(file->status, "copied")) { m->glyph = 0xE72A; m->color = theme.accent; }
    else { m->glyph = 0xE70F; m->color = theme.warning; }
}

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
        screen_files_layout_tree(f, doc, x, tree_w);
        int tree_last = (int)doc->count, tree_bottom = doc->y;
        doc->y = top;
        layout_diff(f, doc, x + tree_w + gap, w - tree_w - gap);
        // The tree stays in view beside the diff and scrolls on its own, as GitHub's file tree does: the page is as
        // long as the diff, or the view's height when the tree needs that.
        RECT view = pane_content_rect(screen_files_files_pane(f));
        int tree_h = tree_bottom - top, room = view.bottom - view.top - px(24);
        if (tree_h > room) tree_h = room;
        if (doc->y < top + tree_h) doc->y = top + tree_h;
        doc_sticky(doc, tree_first, tree_last, doc->y);
    } else {
        screen_files_layout_tree(f, doc, x, w);
        doc_space(doc, px(12));
        layout_diff(f, doc, x, w);
    }
    if (f->list.next_page && !f->error) {
        doc_space(doc, px(8));
        doc_loading(doc, x, w, "Loading more files\xE2\x80\xA6");
        if (!f->loading) SetTimer(pane_hwnd(screen_files_files_pane(f)), f->page_timer, 1, NULL);
    }
    if (f->list.truncated) { doc_space(doc, px(8)); doc_text(doc, x + px(4), w - px(8), "GitHub lists only the first 3,000 files of this pull request.", FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
}
