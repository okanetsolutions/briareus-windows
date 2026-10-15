#include "project_files_internal.h"

/// A finder's result: a file's name and folder, a declaration's name and where it is (a letter in its kind's colour
/// before it, as PhpStorm's C, m and K), or a line holding the text searched for and where it is.
typedef struct { char *name, *detail; char badge; COLORREF badge_color; bool picked, line; } ResultData;

static LRESULT CALLBACK find_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static void find_ensure(ProjectFiles *p);
static void paint_find(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void result_free(void *v);
static void paint_result(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static char *hit_line(const RepoIndexFile *f, const RepoTextHit *h);

static const char *const FIND_TITLES[FIND_MODES] = { "Classes", "Files", "Symbols", "Text" };

static const wchar_t *const FIND_CUES[FIND_MODES] = { L"Go to class (Ctrl+N)", L"Go to file (Ctrl+Shift+N)", L"Go to symbol (Ctrl+Shift+Alt+N)", L"Find in files (Ctrl+Shift+F)" };

// MARK: - Finding

void project_files_find_update(ProjectFiles *p) {
    free(p->results); p->results = NULL; free(p->hits); p->hits = NULL; p->result_count = p->text_total = 0;
    if (str_empty(p->query)) { p->pick = 0; return; }
    if (p->mode == FIND_FILES) { if (p->has_tree) p->results = repo_find_files(&p->tree, p->query, MAX_RESULTS, &p->result_count); }
    else if (p->mode == FIND_TEXT) { if (strlen(p->query) >= 2) p->hits = repo_index_find_text(&p->index, p->query, MAX_HITS, &p->result_count, &p->text_total); }
    else {
        // While files come in, the declarations are worked out again now and then, not for each one.
        if (p->symbols_stale && GetTickCount64() - p->symbols_at >= SYMBOLS_EVERY_MS) project_files_symbols_refresh(p);
        p->results = repo_index_find_symbols(&p->index, p->query, p->mode == FIND_CLASSES, MAX_RESULTS, &p->result_count);
    }
    if (p->pick >= (int)p->result_count) p->pick = p->result_count ? (int)p->result_count - 1 : 0;
    if (p->pick < 0) p->pick = 0;
}

void project_files_find_clear(ProjectFiles *p) {
    if (p->find) SetWindowTextW(p->find, L"");   // its EN_CHANGE clears the query
    set_string(&p->query, NULL); project_files_find_update(p);
}

/// Opens a file at a line, which shows lit once the file is laid out; 0 for its top.
void project_files_open_at(ProjectFiles *p, const char *path, int line) {
    set_string(&p->goto_path, line > 0 ? path : NULL);
    p->goto_line = line; p->goto_scroll = line > 0; p->goto_y = -1;
    project_files_open_path(p, path);
}

void project_files_open_result(ProjectFiles *p, size_t k) {
    if (k >= p->result_count) return;
    char *path; int line = 0;
    if (p->mode == FIND_FILES) path = xstrdup(p->tree.entries[p->results[k]].path);
    else if (p->mode == FIND_TEXT) { path = xstrdup(p->index.files[p->hits[k].file].path); line = p->hits[k].line; }
    else { const RepoSymbol *sym = &p->index.symbols[p->results[k]]; path = xstrdup(p->index.files[sym->file].path); line = sym->line; }
    // The query stays for Find in Files, so the next hit is a click away; the Go to finders close as PhpStorm's do.
    if (p->mode != FIND_TEXT) project_files_find_clear(p);
    if (p->host->pane) SetFocus(pane_hwnd(p->host->pane));
    project_files_open_at(p, path, line);
    free(path);
}

void project_files_set_mode(ProjectFiles *p, int mode) {
    if (mode < 0 || mode >= FIND_MODES) return;
    p->mode = mode; p->pick = 0;
    if (p->find) SendMessageW(p->find, EM_SETCUEBANNER, TRUE, (LPARAM)FIND_CUES[mode]);
    project_files_find_update(p);
    project_files_state_save(p);
    project_files_relayout(p);
}

/// PhpStorm's keys: Ctrl+N a class, Ctrl+Shift+N a file (Ctrl+P too), Ctrl+Shift+Alt+N a symbol, Ctrl+Shift+F text.
int project_files_mode_for_key(WPARAM vk, bool ctrl, bool shift) {
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (!ctrl) return -1;
    if (vk == 'N') return alt ? (shift ? FIND_SYMBOLS : -1) : shift ? FIND_FILES : FIND_CLASSES;
    if (alt) return -1;
    if (vk == 'P' && !shift) return FIND_FILES;
    if (vk == 'O' && shift) return FIND_FILES;
    if (vk == 'F' && shift) return FIND_TEXT;
    return -1;
}

static LRESULT CALLBACK find_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ProjectFiles *p = (ProjectFiles *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN: {
        int mode = project_files_mode_for_key(wp, ctrl, shift);
        if (mode >= 0) { project_files_set_mode(p, mode); SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_DOWN || wp == VK_UP) {
            int n = (int)p->result_count;
            if (n) { p->pick = (p->pick + (wp == VK_DOWN ? 1 : n - 1)) % n; project_files_relayout(p); }
            return 0;
        }
        if (wp == VK_RETURN) { project_files_open_result(p, (size_t)p->pick); return 0; }
        if (wp == VK_ESCAPE) { project_files_find_clear(p); SetFocus(GetParent(hwnd)); project_files_relayout(p); return 0; }
        if (wp == 'A' && ctrl && !(GetKeyState(VK_MENU) & 0x8000)) { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        break;
    }
    case WM_CHAR:
        // Enter, Escape and the control characters the shortcuts above leave behind (^N, ^P, ^O, ^F, ^A);
        // the rest reach the edit, so Ctrl+V, Ctrl+C, Ctrl+X and Ctrl+Z still paste, copy, cut and undo.
        if (wp == '\r' || wp == 0x1B) return 0;
        if (!ctrl) break;
        switch (wp) {
        case 0x0E: case 0x10: case 0x0F: case 0x06: case 0x01: return 0;
        }
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, find_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void find_ensure(ProjectFiles *p) {
    if (p->find || !p->host->pane) return;
    p->find = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, pane_hwnd(p->host->pane),
                              (HMENU)(INT_PTR)ID_FIND, GetModuleHandleW(NULL), NULL);
    SendMessageW(p->find, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(p->find, EM_SETCUEBANNER, TRUE, (LPARAM)FIND_CUES[p->mode]);
    SetWindowSubclass(p->find, find_proc, 0, (DWORD_PTR)p);
    theme_apply_control(p->find);
}

// MARK: - Painting

/// The Go to File box; the edit sits over it.
static void paint_find(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    const ProjectFiles *p = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, p->find && GetFocus() == p->find ? theme.accent_dim : theme.line);
    RECT g = { rc->left + px(10), rc->top, rc->left + px(26), rc->bottom };
    draw_glyph(cv, 0xE721, &g, FONT_ICON_SMALL, theme.muted);
}

void project_files_paint_clear(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RECT g = *rc;
    draw_glyph(cv, 0xE711, &g, FONT_ICON_SMALL, doc_item_hovered(doc, it) ? theme.ink : theme.muted);
}

static void result_free(void *v) { ResultData *d = v; free(d->name); free(d->detail); free(d); }

static void paint_result(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    const ResultData *d = it->data;
    if (d->picked || doc_item_hovered(doc, it)) {
        COLORREF fill = d->picked ? blend(theme.accent, theme.canvas, 0.16) : theme.raise;
        fill_round_rect(cv, rc, px(6), fill, fill);
    }
    int x = rc->left + px(8), right = rc->right - px(6);
    if (d->line) {
        // The line, then its file and number under it.
        int mid = (rc->top + rc->bottom) / 2;
        RECT t = { x, rc->top + px(2), right, mid + px(1) };
        draw_text(cv, d->name, &t, FONT_MONO_CAPTION2, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EXPANDTABS);
        RECT f = { x, mid + px(1), right, rc->bottom - px(2) };
        draw_text(cv, d->detail, &f, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        return;
    }
    int cy = (rc->top + rc->bottom) / 2;
    if (d->badge) {
        fill_circle(cv, x + px(7), cy, px(7), d->badge_color);
        char letter[2] = { d->badge, 0 };
        RECT b = { x, cy - px(7), x + px(14), cy + px(7) };
        draw_text(cv, letter, &b, FONT_TINY_SEMIBOLD, RGB(0xFF, 0xFF, 0xFF), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, 0xE8A5, &g, FONT_ICON_SMALL, theme.muted);
    }
    x += px(22);
    int nw = text_width(cv, d->name, FONT_FOOTNOTE_SEMIBOLD);
    RECT n = { x, rc->top, right, rc->bottom };
    draw_text(cv, d->name, &n, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!str_empty(d->detail) && x + nw + px(8) < right) {
        RECT f = { x + nw + px(8), rc->top, right, rc->bottom };
        draw_text(cv, d->detail, &f, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    }
}

/// A lit line: the one a finder opened.
void project_files_paint_lit(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; (void)it;
    fill_rect(cv, rc, blend(theme.accent, theme.raise, 0.18));
}

// MARK: - Layout

/// The Go to File box at (x, cursor), the edit over it; advances.
void project_files_layout_find(ProjectFiles *p, Doc *doc, int x, int w) {
    find_ensure(p);
    int h = px(34), fh = edit_line_height(FONT_BODY);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_find));
    it->data = p; it->action = p->base + A_FOCUS_FIND; it->hover_fill = false;
    int clear_w = !str_empty(p->query) ? px(28) : 0;
    p->find_rc = (RECT){ x + px(32), box.top + (h - fh) / 2, x + w - px(10) - clear_w, box.top + (h - fh) / 2 + fh };
    p->find_laid = true;
    if (clear_w) {
        RECT c = { x + w - px(8) - clear_w, box.top, x + w - px(8), box.bottom };
        Item *ci = doc_item(doc, doc_add(doc, &c, project_files_paint_clear));
        ci->action = p->base + A_CLEAR_FIND; ci->hand = true; ci->tip = xstrdup("Clear");
    }
    doc->y = box.bottom;
}

/// The text of a hit's line, its leading spaces dropped and cut at 200 bytes.
static char *hit_line(const RepoIndexFile *f, const RepoTextHit *h) {
    size_t n = f->len, start = h->start < n ? h->start : n;
    const char *s = f->content;
    while (start < n && (s[start] == ' ' || s[start] == '\t')) start++;
    size_t end = start;
    while (end < n && s[end] != '\n' && s[end] != '\r' && end - start < 200) end++;
    return xstrndup(s + start, end - start);
}

void project_files_layout_results(ProjectFiles *p, Doc *doc, int x, int w) {
    if (p->mode == FIND_TEXT && strlen(p->query) < 2) { doc_text(doc, x + px(8), w - px(16), "Type at least two characters.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); return; }
    if (!p->result_count) {
        static const char *const NONE[FIND_MODES] = { "No class matches.", "No file matches.", "No symbol matches.", "No line holds this text." };
        doc_text(doc, x + px(8), w - px(16), NONE[p->mode], FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        return;
    }
    if (p->mode == FIND_TEXT) {
        char *count = p->text_total > p->result_count ? xstrfmt("%zu lines; the first %zu are listed", p->text_total, p->result_count)
                                                      : xstrfmt("%zu line%s", p->text_total, p->text_total == 1 ? "" : "s");
        doc_text(doc, x + px(8), w - px(16), count, FONT_CAPTION2, theme.secondary, DT_LEFT | DT_SINGLELINE);
        free(count);
        doc_space(doc, px(4));
    }
    for (size_t k = 0; k < p->result_count; k++) {
        ResultData *d = xcalloc(1, sizeof *d);
        char *tip;
        int h = px(26);
        if (p->mode == FIND_FILES) {
            const RepoEntry *e = &p->tree.entries[p->results[k]];
            d->name = xstrdup(e->name);
            d->detail = xstrndup(e->path, (size_t)(e->name - e->path) ? (size_t)(e->name - e->path) - 1 : 0);
            tip = xstrdup(e->path);
        } else if (p->mode == FIND_TEXT) {
            const RepoTextHit *hit = &p->hits[k];
            const RepoIndexFile *f = &p->index.files[hit->file];
            d->name = hit_line(f, hit); d->line = true;
            d->detail = xstrfmt("%s:%d", f->path, hit->line);
            tip = xstrdup(d->detail);
            h = px(38);
        } else {
            const RepoSymbol *sym = &p->index.symbols[p->results[k]];
            const char *path = p->index.files[sym->file].path;
            d->name = xstrdup(sym->name);
            d->detail = sym->container ? xstrfmt("%s \xC2\xB7 %s:%d", sym->container, path, sym->line) : xstrfmt("%s:%d", path, sym->line);
            d->badge = sym->kind == SYMBOL_CLASS ? 'C' : sym->kind == SYMBOL_FUNCTION ? (sym->container ? 'm' : 'f') : 'K';
            d->badge_color = sym->kind == SYMBOL_CLASS ? RGB(0x3B, 0x7C, 0xD6) : sym->kind == SYMBOL_FUNCTION ? RGB(0xD0, 0x6F, 0x2A) : RGB(0x8E, 0x5C, 0xC4);
            tip = xstrdup(d->detail);
        }
        d->picked = (int)k == p->pick;
        int i = doc_custom(doc, x, w, h, paint_result, d, result_free, p->base + A_RESULT, (intptr_t)k);
        doc_item(doc, i)->hover_fill = false;
        doc_item(doc, i)->tip = tip;
    }
    if (p->mode != FIND_TEXT && p->result_count == (size_t)MAX_RESULTS) { doc_space(doc, px(4)); doc_text(doc, x + px(8), w - px(16), "Only the 50 best matches are listed.", FONT_CAPTION2, theme.tertiary, DT_LEFT | DT_WORDBREAK); }
}

/// The finders' picker, and what the index is doing: read from disk, catching up, filling, or how much it holds.
void project_files_layout_modes(ProjectFiles *p, Doc *doc, int x, int w) {
    doc_segments(doc, x, w, FIND_TITLES, FIND_MODES, p->mode, p->base + A_MODE, 0, true);
    char *status = NULL;
    if (p->index_error) {
        doc_space(doc, px(6));
        char *line = xstrfmt("Indexing stopped: %s", p->index_error);
        doc_notice(doc, x, w, line); free(line);
        doc_space(doc, px(4));
        doc_button(doc, x, 0, "Index again", BUTTON_BORDERED, p->base + A_INDEX_RETRY, 0, !p->target);
        return;
    }
    if (!p->index_read && p->loading) status = xstrdup("Reading the index\xE2\x80\xA6");
    else if (p->target && p->req_walk) status = xstrfmt("Looking for what changed since %.7s\xE2\x80\xA6", project_files_index_base(p));
    else if (p->target) status = xstrfmt("Indexing %zu of %zu files\xE2\x80\xA6", p->fetched, p->queue_count);
    else if (p->mode != FIND_FILES && p->index_read && p->index.sha)
        status = xstrfmt("%zu files and %zu declarations indexed at %.7s", p->index.count, p->index.symbol_count, p->index.sha);
    if (status) { doc_space(doc, px(6)); doc_text(doc, x + px(4), w - px(8), status, FONT_CAPTION2, p->target ? theme.secondary : theme.tertiary, DT_LEFT | DT_WORDBREAK); free(status); }
}
