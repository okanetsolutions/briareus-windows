#include "project_files_internal.h"

/// An editor tab: the file's name, lit when it is the one shown; its ✕ is an item of its own over it.
typedef struct { char *name; bool active; } TabData;

/// Line numbers for a block of lines, painted together.
typedef struct { size_t first, count; int row_h; } GutterData;

static void file_forget_lines(FileTab *f);
static void file_prepare(FileTab *f);
static void file_done(void *owner, Request *req);
static void tab_data_free(void *v);
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_gutter(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static COLORREF code_color(CodeKind kind);

// MARK: - Open files

static void file_forget_lines(FileTab *f) {
    free(f->lines); free(f->tokens); free(f->first);
    f->lines = NULL; f->tokens = NULL; f->first = NULL; f->line_count = 0; f->widest = 0;
}

/// Splits the text into lines and colours them, carrying a block comment or a long string from line to line.
static void file_prepare(FileTab *f) {
    file_forget_lines(f);
    const char *text = f->file.content;
    if (!text) return;
    f->lines = repo_lines(text, &f->line_count);
    f->first = xcalloc(f->line_count + 1, sizeof *f->first);
    CodeLexer lexer; code_lexer_init(&lexer, code_language_of(f->path));
    size_t count = 0, cap = 0;
    for (size_t i = 0; i < f->line_count; i++) {
        const RepoLine *l = &f->lines[i];
        f->first[i] = count;
        size_t n; CodeToken *runs = code_lexer_line(&lexer, text + l->start, l->len, &n);
        if (count + n > cap) { cap = (count + n) * 2; f->tokens = xrealloc(f->tokens, cap * sizeof *f->tokens); }
        if (n) memcpy(f->tokens + count, runs, n * sizeof *runs);
        count += n;
        free(runs);
        // Its width in columns, tabs stopping every four as doc_code_line lays them.
        size_t col = 0;
        for (size_t k = 0; k < l->len; k++) {
            unsigned char c = (unsigned char)text[l->start + k];
            if (c == '\t') col += 4 - col % 4; else if ((c & 0xC0) != 0x80) col++;
        }
        if (col > f->widest) f->widest = col;
    }
    f->first[f->line_count] = count;
}

void project_files_file_free(FileTab *f) {
    if (!f) return;
    request_cancel(&f->req);
    repo_file_free(&f->file); file_forget_lines(f);
    free(f->path); free(f->read_at); free(f->error);
    free(f);
}

static void file_done(void *owner, Request *req) {
    FileTab *f = owner;
    ProjectFiles *p = f->owner;
    if (!req->ok) { request_error_into(&f->error, req); project_files_relayout(p); return; }
    RepoFile file;
    if (!repo_file_parse(req->result, &file)) { set_string(&f->error, "The server returned an unexpected response."); project_files_relayout(p); return; }
    repo_file_free(&f->file);
    f->file = file; f->loaded = true;
    set_string(&f->error, NULL);
    set_string(&f->read_at, json_str(json_get(req->args, "ref")));
    file_prepare(f);
    project_files_relayout(p);
}

/// Reads the file when it has not been read at the tree's commit; a failed read waits for Try again.
void project_files_file_ensure(ProjectFiles *p, FileTab *f, bool retry) {
    if (!f || f->req || (!p->has_tree && !p->loaded)) return;
    if (f->error && !retry) return;
    const char *ref = project_files_files_ref(p);
    if (f->loaded && str_eq(f->read_at, ref)) return;
    set_string(&f->error, NULL);
    Json *args = json_object();
    json_set_str(args, "repo", p->repo);
    json_set_str(args, "path", f->path);
    if (ref) json_set_str(args, "ref", ref);
    store_call("repo_file", args, 0, f, file_done, 0, &f->req);
}

void project_files_activate(ProjectFiles *p, int index) {
    if (index < 0 || (size_t)index >= p->tab_count) { p->active = -1; return; }
    p->active = index;
    if (p->has_tree) repo_tree_reveal(&p->tree, p->tabs[index]->path);
    project_files_file_ensure(p, p->tabs[index], false);
}

FileTab *project_files_tab_add(ProjectFiles *p, const char *path) {
    FileTab *f = xcalloc(1, sizeof *f);
    f->owner = p; f->path = xstrdup(path);
    p->tabs = xrealloc(p->tabs, (p->tab_count + 1) * sizeof *p->tabs);
    p->tabs[p->tab_count++] = f;
    return f;
}

void project_files_tab_close(ProjectFiles *p, size_t index) {
    if (index >= p->tab_count) return;
    project_files_file_free(p->tabs[index]);
    memmove(&p->tabs[index], &p->tabs[index + 1], (p->tab_count - index - 1) * sizeof *p->tabs);
    p->tab_count--;
    // The tab to the right takes its place, as an editor's do; the last one's left neighbour.
    if (p->active > (int)index) p->active--;
    else if (p->active == (int)index) p->active = p->tab_count ? (int)(index < p->tab_count ? index : p->tab_count - 1) : -1;
    project_files_activate(p, p->active);
}

/// Opens a file as a tab, or shows the tab it already has; past MAX_TABS the oldest other tab closes.
void project_files_open_path(ProjectFiles *p, const char *path) {
    for (size_t i = 0; i < p->tab_count; i++) if (str_eq(p->tabs[i]->path, path)) { project_files_activate(p, (int)i); project_files_state_save(p); project_files_relayout(p); return; }
    if (p->tab_count >= (size_t)MAX_TABS) project_files_tab_close(p, p->active == 0 ? 1 : 0);
    project_files_tab_add(p, path);
    project_files_activate(p, (int)p->tab_count - 1);
    project_files_state_save(p);
    if (p->host->pane) pane_scroll_to_top(p->host->pane);
    project_files_relayout(p);
}

static void tab_data_free(void *v) { TabData *d = v; free(d->name); free(d); }

static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    const TabData *d = it->data;
    COLORREF fill = d->active ? theme.raise : doc_item_hovered(doc, it) ? blend(theme.raise, theme.canvas, 0.5) : theme.canvas;
    fill_round_rect(cv, rc, px(6), fill, d->active ? theme.line : fill);
    if (d->active) { RECT bar = { rc->left + px(6), rc->bottom - px(2), rc->right - px(6), rc->bottom }; fill_rect(cv, &bar, theme.accent); }
    RECT t = { rc->left + px(10), rc->top, rc->right - px(26), rc->bottom };
    draw_text(cv, d->name, &t, d->active ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, d->active ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void paint_gutter(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    const GutterData *d = it->data;
    for (size_t i = 0; i < d->count; i++) {
        char number[24]; snprintf(number, sizeof number, "%zu", d->first + i + 1);
        RECT r = { rc->left, rc->top + (int)i * d->row_h, rc->right - px(12), rc->top + (int)(i + 1) * d->row_h };
        draw_text(cv, number, &r, FONT_MONO_SMALL, theme.tertiary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

/// The colours of a token, Darcula's in the dark and the light theme's otherwise.
static COLORREF code_color(CodeKind kind) {
    bool dark = theme.dark;
    switch (kind) {
    case CODE_KEYWORD: return dark ? RGB(0xCC, 0x78, 0x32) : RGB(0x00, 0x33, 0xB3);
    case CODE_STRING: return dark ? RGB(0x6A, 0x87, 0x59) : RGB(0x06, 0x7D, 0x17);
    case CODE_COMMENT: return dark ? RGB(0x80, 0x80, 0x80) : RGB(0x8C, 0x8C, 0x8C);
    case CODE_NUMBER: return dark ? RGB(0x68, 0x97, 0xBB) : RGB(0x17, 0x50, 0xEB);
    case CODE_TYPE: return dark ? RGB(0xE8, 0xBF, 0x6A) : RGB(0x37, 0x1F, 0x80);
    case CODE_VARIABLE: return dark ? RGB(0x98, 0x76, 0xAA) : RGB(0x87, 0x10, 0x94);
    case CODE_PLAIN: default: return theme.text;
    }
}

/// The open files as tabs, wrapping, then the active file's Copy and Open on GitHub; each tab closes with its ✕, or from its menu (right click).
void project_files_layout_tabs(ProjectFiles *p, Doc *doc, int x, int w) {
    int h = px(32), tx = x, ty = doc->y;
    for (size_t i = 0; i < p->tab_count; i++) {
        const FileTab *f = p->tabs[i];
        const char *slash = strrchr(f->path, '/');
        TabData *d = xcalloc(1, sizeof *d);
        d->name = xstrdup(slash ? slash + 1 : f->path); d->active = (int)i == p->active;
        int tw = text_width(doc->cv, d->name, FONT_FOOTNOTE_SEMIBOLD) + px(40);
        if (tw > px(260)) tw = px(260);
        if (tx > x && tx + tw > x + w) { tx = x; ty += h + px(4); }
        RECT rc = { tx, ty, tx + tw, ty + h };
        Item *it = doc_item(doc, doc_add(doc, &rc, paint_tab));
        it->data = d; it->free_data = tab_data_free; it->action = p->base + A_TAB; it->arg = (intptr_t)i; it->hover_fill = false;
        it->tip = xstrdup(f->path);
        RECT c = { rc.right - px(24), rc.top + px(6), rc.right - px(6), rc.bottom - px(6) };
        Item *ci = doc_item(doc, doc_add(doc, &c, project_files_paint_clear));
        ci->action = p->base + A_TAB_CLOSE; ci->arg = (intptr_t)i; ci->hand = true; ci->tip = xstrdup("Close");
        tx += tw + px(4);
    }
    // Copy and Open on GitHub follow the tabs, as an editor's tab bar actions do; they wrap below when the row is full.
    FileTab *f = project_files_active_file(p);
    ButtonSpec buttons[2]; size_t bc = 0;
    if (f && f->file.content) buttons[bc++] = (ButtonSpec){ 0xE8C8, "Copy", BUTTON_PLAIN, p->base + A_COPY, 0, true };
    if (f && safe_web_url(f->file.url)) buttons[bc++] = (ButtonSpec){ 0xE8A7, "Open on GitHub", BUTTON_PLAIN, p->base + A_OPEN_GITHUB, 0, true };
    if (bc) {
        // The room they need, measured as doc_button_row lays out plain buttons: the lead-in, each width, 6px gaps.
        int need = px(8) + px(6) * (int)(bc - 1);
        for (size_t k = 0; k < bc; k++) need += text_width(doc->cv, buttons[k].text, FONT_CAPTION) + px(2);
        if (tx > x && x + w - tx < need) { tx = x; ty += h + px(4); }
        doc->y = ty + (h - font_height(doc->cv, FONT_FOOTNOTE) - px(12)) / 2;
        doc_button_row(doc, tx + px(8), x + w - tx - px(8), buttons, bc);
        if (doc->y > ty + h) return;
    }
    doc->y = ty + h;
}

/// The file shown: its path and size, then its numbered lines, or why it has none.
void project_files_layout_file(ProjectFiles *p, Doc *doc, int x, int w) {
    FileTab *f = project_files_active_file(p);
    if (!f) {
        doc_empty_state(doc, x, w, 0xE8A5, "No file open", "Pick a file in the tree, or press Ctrl+P and type a few letters of its name.");
        return;
    }
    project_files_file_ensure(p, f, false);
    // The path, and what is known of the file.
    Str meta; str_init(&meta);
    if (f->loaded) {
        char *size = sftp_format_size(f->file.size);
        str_appendf(&meta, "%s", size); free(size);
        if (f->file.content) str_appendf(&meta, " \xC2\xB7 %zu line%s \xC2\xB7 %s", f->line_count, f->line_count == 1 ? "" : "s", code_language_name(code_language_of(f->path)));
    }
    doc_text(doc, x, w, f->path, FONT_MONO_SMALL, theme.ink, DT_LEFT | DT_WORDBREAK);
    if (meta.len) { doc_space(doc, px(2)); doc_text(doc, x, w, meta.data, FONT_CAPTION2, theme.secondary, DT_LEFT | DT_SINGLELINE); }
    str_free(&meta);
    doc_space(doc, px(8));
    if (f->error) {
        doc_notice(doc, x, w, f->error);
        doc_space(doc, px(8));
        doc_button(doc, x, 0, "Try again", BUTTON_BORDERED, p->base + A_RETRY_FILE, 0, !f->req);
        return;
    }
    if (!f->loaded || (f->req && !f->file.content)) { doc_loading(doc, x, w, "Loading the file\xE2\x80\xA6"); return; }
    if (!f->file.content) {
        char *size = sftp_format_size(f->file.size);
        char *detail = f->file.too_large ? xstrfmt("At %s it is over the 1 MB the server sends. Open it on GitHub to read it.", size)
                                         : xstrfmt("It does not read as text (%s). Open it on GitHub to see it.", size);
        doc_empty_state(doc, x, w, f->file.too_large ? 0xE7BA : 0xE8A5, f->file.too_large ? "This file is too large to show" : "This file is binary", detail);
        free(size); free(detail);
        return;
    }
    if (!f->line_count) { doc_text(doc, x, w, "This file is empty.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_SINGLELINE); return; }
    // The lines on a monospaced grid: one character's advance, in hundredths of a pixel, from a hundred of them.
    int advance = text_width(doc->cv, "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", FONT_MONO_SMALL);
    if (advance <= 0) advance = px(7) * 100;
    int row_h = font_height(doc->cv, FONT_MONO_SMALL) + px(3);
    char digits[24]; snprintf(digits, sizeof digits, "%zu", f->line_count);
    int gutter = (int)strlen(digits) * advance / 100 + px(24);
    int code_x = x + 1 + gutter, text_w = (int)((long long)f->widest * advance / 100) + px(24);
    int right = code_x + text_w > x + w ? code_x + text_w : x + w;
    int box = doc_box_begin(doc, x, w, 0, theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(6));
    const char *text = f->file.content;
    int lit = str_eq(p->goto_path, f->path) ? (p->goto_line < 1 ? 1 : p->goto_line > (int)f->line_count ? (int)f->line_count : p->goto_line) : 0;
    size_t span_cap = 16;
    DocSpan *spans = xcalloc(span_cap, sizeof *spans);
    for (size_t i = 0; i < f->line_count; i++) {
        if (i % (size_t)GUTTER_BLOCK == 0) {
            GutterData *g = xcalloc(1, sizeof *g);
            g->first = i; g->count = f->line_count - i < (size_t)GUTTER_BLOCK ? f->line_count - i : (size_t)GUTTER_BLOCK; g->row_h = row_h;
            RECT gr = { x + 1, doc->y, x + 1 + gutter, doc->y + (int)g->count * row_h };
            Item *gi = doc_item(doc, doc_add(doc, &gr, paint_gutter));
            gi->data = g; gi->free_data = free;
        }
        size_t first = f->first[i], n = f->first[i + 1] - first;
        if (n > span_cap) { span_cap = n * 2; spans = xrealloc(spans, span_cap * sizeof *spans); }
        for (size_t k = 0; k < n; k++) {
            const CodeToken *t = &f->tokens[first + k];
            spans[k] = (DocSpan){ t->start, t->len, code_color(t->kind) };
        }
        RECT rc = { code_x, doc->y, right - px(12), doc->y + row_h };
        if (lit == (int)i + 1) {
            RECT lr = { x + 1, doc->y, right - 1, doc->y + row_h };
            doc_add(doc, &lr, project_files_paint_lit);
            // A few lines above it stay in view.
            p->goto_y = doc->y - row_h * 4 > 0 ? doc->y - row_h * 4 : 0;
        }
        doc_code_line(doc, &rc, text + f->lines[i].start, f->lines[i].len, spans, n, FONT_MONO_SMALL, advance);
        doc->y += row_h;
    }
    free(spans);
    doc_space(doc, px(6));
    doc_box_end(doc, box, 0);
    // Long lines scroll sideways: the box grows to the widest one.
    if (right > x + w) { doc_item(doc, box)->rc.right = right; if (right > doc->content_width) doc->content_width = right; }
}
